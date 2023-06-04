/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2011  Black Sphere Technologies Ltd.
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
 * Copyright (C) 2019 - 2021 Uwe Bonnes bon@elektron.ikp.physik.tu-darmstadt.de
 * Copyright (C) 2022-2023 1BitSquared <info@1bitsquared.com>
 * Modified by Rachel Mant <git@dragonmux.network>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * This file implements the SW-DP specific functions of the
 * ARM Debug Interface v5 Architecture Specification, ARM doc IHI0031A.
 */

#include <assert.h>
#include "general.h"
#include "exception.h"
#include "target.h"
#include "target_internal.h"
#include "adiv5.h"
#include "jlink.h"
#include "jlink_protocol.h"
#include "buffer_utils.h"
#include "cli.h"

static uint32_t jlink_swd_seq_in(size_t clock_cycles);
static bool jlink_swd_seq_in_parity(uint32_t *result, size_t clock_cycles);
static void jlink_swd_seq_out(uint32_t tms_states, size_t clock_cycles);
static void jlink_swd_seq_out_parity(uint32_t tms_states, size_t clock_cycles);

static bool jlink_adiv5_swdp_write_nocheck(uint16_t addr, uint32_t data);
static uint32_t jlink_adiv5_swdp_read_nocheck(uint16_t addr);
static uint32_t jlink_adiv5_swdp_error(adiv5_debug_port_s *dp, bool protocol_recovery);
static uint32_t jlink_adiv5_swdp_low_access(adiv5_debug_port_s *dp, uint8_t RnW, uint16_t addr, uint32_t value);

/*
 * Write at least 50 bits high, two bits low and read DP_IDR and put
 * idle cycles at the end
 */
static bool line_reset(bmp_info_s *const info)
{
	uint8_t cmd[44];
	memset(cmd, 0, sizeof(cmd));

	cmd[0] = JLINK_CMD_IO_TRANSACT;
	/* write 19 bytes */
	cmd[2] = 19U * 8U;
	uint8_t *const direction = cmd + 4U;
	memset(direction + 5U, 0xffU, 9U);
	direction[18] = 0xe0U;
	uint8_t *const data = direction + 19U;
	memset(data + 5U, 0xffU, 7U);
	data[13] = 0xa5U;

	uint8_t res[19];
	bmda_usb_transfer(info->usb_link, cmd, 42U, res, 19U);
	bmda_usb_transfer(info->usb_link, NULL, 0U, res, 1U);

	if (res[0] != 0) {
		DEBUG_ERROR("Line reset failed\n");
		return false;
	}
	return true;
}

bool jlink_swd_init(adiv5_debug_port_s *dp)
{
	DEBUG_PROBE("-> jlink_swd_init(%u)\n", dp->dev_index);
	/* Try to switch the adaptor into SWD mode */
	uint8_t res[4];
	if (jlink_simple_request(JLINK_CMD_TARGET_IF, JLINK_IF_GET_AVAILABLE, res, sizeof(res)) < 0 ||
		!(res[0] & JLINK_IF_SWD) || jlink_simple_request(JLINK_CMD_TARGET_IF, SELECT_IF_SWD, res, sizeof(res)) < 0) {
		DEBUG_ERROR("SWD not available\n");
		return false;
	}
	platform_delay(10);

	/* Set up the underlying SWD functions using the implementation below */
	swd_proc.seq_in = jlink_swd_seq_in;
	swd_proc.seq_in_parity = jlink_swd_seq_in_parity;
	swd_proc.seq_out = jlink_swd_seq_out;
	swd_proc.seq_out_parity = jlink_swd_seq_out_parity;

	/* Set up the accelerated SWD functions for basic target operations */
	dp->dp_low_write = jlink_adiv5_swdp_write_nocheck;
	dp->dp_read = firmware_swdp_read;
	dp->error = jlink_adiv5_swdp_error;
	dp->low_access = jlink_adiv5_swdp_low_access;
	return true;
}

static void jlink_swd_seq_out(const uint32_t tms_states, const size_t clock_cycles)
{
	DEBUG_PROBE("%s %zu clock_cycles: %08" PRIx32 "\n", __func__, clock_cycles, tms_states);
	/* Encode the sequence data appropriately */
	uint8_t data[4];
	write_le4(data, 0, tms_states);
	/* Attempt the transfer */
	if (!jlink_transfer_swd(clock_cycles, JLINK_SWD_OUT, data, NULL)) {
		/* If things go wrong, report it */
		DEBUG_ERROR("jlink_swd_seq_out failed\n");
	}
}

static void jlink_swd_seq_out_parity(const uint32_t tms_states, const size_t clock_cycles)
{
	DEBUG_PROBE("%s %zu clock_cycles: %08" PRIx32 "\n", __func__, clock_cycles, tms_states);
	/* Encode the sequence data appropriately */
	uint8_t data[5] = {0};
	write_le4(data, 0, tms_states);
	/* Construct the parity bit */
	const size_t byte = clock_cycles >> 3U;
	const uint8_t bit = clock_cycles & 7U;
	data[byte] |= (__builtin_parity(tms_states) & 1U) << bit;
	/* Attempt the transfer */
	if (!jlink_transfer_swd(clock_cycles + 1U, JLINK_SWD_OUT, data, NULL)) {
		/* If things go wrong, report it */
		DEBUG_ERROR("jlink_swd_seq_out_parity failed\n");
	}
}

static uint32_t jlink_swd_seq_in(const size_t clock_cycles)
{
	/* Create a buffer to hold the result of the transfer */
	uint8_t data_out[4] = {0};
	/* Attempt the transfer */
	if (!jlink_transfer_swd(clock_cycles, JLINK_SWD_IN, NULL, data_out)) {
		DEBUG_ERROR("jlink_swd_seq_in failed\n");
		return 0U;
	}
	/* Everything went well, so now convert the result and return it */
	const uint32_t result = read_le4(data_out, 0);
	DEBUG_PROBE("%s %zu clock_cycles: %08" PRIx32 "\n", __func__, clock_cycles, result);
	return result;
}

static bool jlink_swd_seq_in_parity(uint32_t *const result, const size_t clock_cycles)
{
	/* Create a buffer to hold the result of the transfer */
	uint8_t data_out[5] = {0};
	/* Attempt the transfer */
	if (!jlink_transfer_swd(clock_cycles + 1U, JLINK_SWD_IN, NULL, data_out)) {
		DEBUG_ERROR("jlink_swd_seq_in_parity failed\n");
		return false;
	}

	/* Everything went well, so pull out the sequence result and store it */
	const uint32_t data = read_le4(data_out, 0);
	/* Compute the parity and validate it */
	const size_t byte = clock_cycles >> 3U;
	const uint8_t bit = clock_cycles & 7U;
	uint8_t parity = __builtin_parity(data) & 1U;
	parity ^= (data_out[byte] >> bit) & 1U;
	/* Retrn the result of the calculation */
	DEBUG_PROBE("%s %zu clock_cycles: %08" PRIx32 " %s\n", __func__, clock_cycles, data, parity ? "ERR" : "OK");
	*result = data;
	return !parity;
}

/*
 * The first byte in this defines 8 OUT bits to write the request out.
 * The second then defines 1 IN bit for turn-around to read the status response
 * followed by either 2 (read) or 3 (write) IN bits to read the response.
 * Read only uses the first 3 bits of the second byte.
 * Write uses the first 5 and defines the last bit it uses as an OUT bit
 * for the final turn-around to write the request data.
 */
static const uint8_t jlink_adiv5_request[2] = {0xffU, 0xf0U};

/* Direction sequence for the data phase of a write transaction */
static const uint8_t jlink_adiv5_write_request[6] = {
	/* clang-format off */
	/* 32 OUT cycles */
	0xffU, 0xffU, 0xffU, 0xffU,
	/* 1 more OUT cycle (parity) followed by 8 OUT (idle) cycles */
	0xffU, 0x01U,
	/* clang-format on */
};

/* Direction sequence for the dta phase of a read transaction */
static const uint8_t jlink_adiv5_read_request[5] = {
	/* clang-format off */
	/* 32 IN cycles */
	0x00U, 0x00U, 0x00U, 0x00U,
	/* 1 more IN cycle (parity) followed by 2 OUT (idle) cycles */
	0xfeU,
	/* clang-format on */
};

static void jlink_adiv5_swdp_make_packet_request(
	uint8_t *cmd, size_t cmd_length, const uint8_t RnW, const uint16_t addr)
{
	assert(cmd_length == 8U);
	memset(cmd, 0, cmd_length);
	cmd[0] = JLINK_CMD_IO_TRANSACT;

	/*
	 * It seems that JLink samples the data to read at the end of the
	 * previous clock cycle, so reading target data must start at the
	 * 12th clock cycle, while writing starts as expected at the 14th
	 * clock cycle (8 cmd, 3 response, 2 turn around).
	 */
	cmd[2] = RnW ? 11 : 13;

	cmd[4] = 0xffU; /* 8 bits command OUT */
	/*
	 * one IN bit to turn around to read, read 2
	 * (read) or 3 (write) IN bits for response and
	 * and one OUT bit to turn around to write on write
	 */
	cmd[5] = 0xf0U;
	cmd[6] = make_packet_request(RnW, addr);
}

static bool jlink_adiv5_swdp_write_nocheck(const uint16_t addr, const uint32_t data)
{
	DEBUG_PROBE("jlink_swd_write_reg_no_check %04x <- %08" PRIx32 "\n", addr, data);
	/* Build the request buffer */
	const uint8_t request[2] = {make_packet_request(ADIV5_LOW_WRITE, addr)};
	uint8_t result[2] = {0};
	/* Try making a request to the device (13 cycles, we start writing on the 14th) */
	if (!jlink_transfer(13U, jlink_adiv5_request, request, result)) {
		DEBUG_ERROR("jlink_swd_write_no_check failed\n");
		return false;
	}
	const uint8_t ack = result[1] & 7U;

	/* Build the response payload buffer */
	uint8_t response[6] = {0};
	write_le4(response, 0, data);
	response[4] = __builtin_popcount(data) & 1U;
	/* Try sending the data to the device */
	if (!jlink_transfer(33U + 8U, jlink_adiv5_write_request, response, NULL)) {
		DEBUG_ERROR("jlink_swd_write_no_check failed\n");
		return false;
	}
	return ack != SWDP_ACK_OK;
}

static uint32_t jlink_adiv5_swdp_read_nocheck(const uint16_t addr)
{
	/* Build the request buffer */
	const uint8_t request[2] = {make_packet_request(ADIV5_LOW_READ, addr)};
	uint8_t result[2] = {0};
	/* Try making a request to the device (11 cycles, we start reading on the 12th) */
	if (!jlink_transfer(11U, jlink_adiv5_request, request, result)) {
		DEBUG_ERROR("jlink_swd_read_no_check failed\n");
		return 0U;
	}
	const uint8_t ack = result[1] & 7U;

	uint8_t response[5] = {0};
	/* Try to receive the response payload */
	if (!jlink_transfer(33U + 2U, jlink_adiv5_read_request, NULL, response)) {
		DEBUG_ERROR("jlink_swd_read_no_check failed\n");
		return 0U;
	}
	/* Extract the data phase and return it if the transaction suceeded */
	const uint32_t data = read_le4(response, 0);
	DEBUG_PROBE("jlink_swd_read_reg_no_check %04x -> %08" PRIx32 " %s\n", addr, data,
		__builtin_parity(data) ^ response[4] ? "ERR" : "OK");
	return ack == SWDP_ACK_OK ? data : 0U;
}

static uint32_t jlink_adiv5_swdp_error(adiv5_debug_port_s *const dp, const bool protocol_recovery)
{
	DEBUG_PROBE("jlink_swd_clear_error (protocol recovery? %s)\n", protocol_recovery ? "true" : "false");
	/* Only do the comms reset dance on DPv2+ w/ fault or to perform protocol recovery. */
	if ((dp->version >= 2 && dp->fault) || protocol_recovery) {
		/*
		 * Note that on DPv2+ devices, during a protocol error condition
		 * the target becomes deselected during line reset. Once reset,
		 * we must then re-select the target to bring the device back
		 * into the expected state.
		 */
		line_reset(&info);
		if (dp->version >= 2)
			jlink_adiv5_swdp_write_nocheck(ADIV5_DP_TARGETSEL, dp->targetsel);
		jlink_adiv5_swdp_read_nocheck(ADIV5_DP_DPIDR);
		/* Exception here is unexpected, so do not catch */
	}
	const uint32_t err = jlink_adiv5_swdp_read_nocheck(ADIV5_DP_CTRLSTAT) &
		(ADIV5_DP_CTRLSTAT_STICKYORUN | ADIV5_DP_CTRLSTAT_STICKYCMP | ADIV5_DP_CTRLSTAT_STICKYERR |
			ADIV5_DP_CTRLSTAT_WDATAERR);
	uint32_t clr = 0;

	if (err & ADIV5_DP_CTRLSTAT_STICKYORUN)
		clr |= ADIV5_DP_ABORT_ORUNERRCLR;
	if (err & ADIV5_DP_CTRLSTAT_STICKYCMP)
		clr |= ADIV5_DP_ABORT_STKCMPCLR;
	if (err & ADIV5_DP_CTRLSTAT_STICKYERR)
		clr |= ADIV5_DP_ABORT_STKERRCLR;
	if (err & ADIV5_DP_CTRLSTAT_WDATAERR)
		clr |= ADIV5_DP_ABORT_WDERRCLR;

	if (clr)
		jlink_adiv5_swdp_write_nocheck(ADIV5_DP_ABORT, clr);
	dp->fault = 0;
	return err;
}

static uint32_t jlink_adiv5_swdp_low_read(adiv5_debug_port_s *const dp)
{
	uint8_t result[5] = {0};
	/* Try to receive the result payload */
	if (!jlink_transfer(33U + 2U, jlink_adiv5_read_request, NULL, result)) {
		DEBUG_ERROR("jlink_swd_raw_read failed\n");
		return 0U;
	}
	/* Extract the data phase */
	const uint32_t response = read_le4(result, 0);
	/* Calculate and do a parity check */
	uint8_t parity = __builtin_parity(response) & 1U;
	parity ^= result[4] & 1U;
	/* If that fails, turn it into an error */
	if (parity) {
		dp->fault = 1;
		DEBUG_ERROR("SWD access resulted in parity error\n");
		raise_exception(EXCEPTION_ERROR, "SWD parity error");
	}
	return response;
}

static uint32_t jlink_adiv5_swdp_low_write(const uint32_t request_value)
{
	/* Build the response payload buffer */
	uint8_t request[6] = {0};
	write_le4(request, 0, request_value);
	request[4] = __builtin_popcount(request_value) & 1U;
	/* Allocate storage for the result */
	uint8_t result[6] = {0};
	/* Try sending the data to the device */
	if (!jlink_transfer(33U + 8U, jlink_adiv5_write_request, request, result))
		raise_exception(EXCEPTION_ERROR, "jlink_swd_raw_write failed\n");
	/* Unpack the result */
	const uint32_t result_value = read_le4(result, 0);
	return result_value;
}

static uint32_t jlink_adiv5_swdp_low_access(
	adiv5_debug_port_s *const dp, const uint8_t RnW, const uint16_t addr, const uint32_t value)
{
	if ((addr & ADIV5_APnDP) && dp->fault)
		return 0;

	uint8_t cmd[8];
	jlink_adiv5_swdp_make_packet_request(cmd, sizeof(cmd), RnW, addr);

	uint8_t res[3];
	platform_timeout_s timeout;
	platform_timeout_set(&timeout, 2000);
	uint8_t ack = SWDP_ACK_WAIT;
	while (ack == SWDP_ACK_WAIT && !platform_timeout_is_expired(&timeout)) {
		bmda_usb_transfer(info.usb_link, cmd, 8U, res, 2U);
		bmda_usb_transfer(info.usb_link, NULL, 0U, res + 2U, 1U);

		if (res[2] != 0)
			raise_exception(EXCEPTION_ERROR, "Low access setup failed");
		ack = res[1] & 7U;
	};

	if (ack == SWDP_ACK_WAIT) {
		DEBUG_WARN("SWD access resulted in wait, aborting\n");
		dp->abort(dp, ADIV5_DP_ABORT_DAPABORT);
		dp->fault = ack;
		return 0;
	}

	if (ack == SWDP_ACK_FAULT) {
		DEBUG_ERROR("SWD access resulted in fault\n");
		dp->fault = ack;
		return 0;
	}

	if (ack == SWDP_ACK_NO_RESPONSE) {
		DEBUG_ERROR("SWD access resulted in no response\n");
		dp->fault = ack;
		return 0;
	}

	if (ack != SWDP_ACK_OK) {
		DEBUG_ERROR("SWD access has invalid ack %x\n", ack);
		raise_exception(EXCEPTION_ERROR, "SWD invalid ACK");
	}

	/* Always append 8 idle cycles (SWDIO = 0)! */
	if (RnW)
		return jlink_adiv5_swdp_low_read(dp);
	jlink_adiv5_swdp_low_write(value);
	return 0;
}
