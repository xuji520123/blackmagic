/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2011  Black Sphere Technologies Ltd.
 * Written by Gareth McMullin <gareth@blacksphere.co.nz>
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

/* This file implements the platform specific functions for the F4 Discovery implementation. */

#include "general.h"
#include "platform.h"
#include "usb.h"
#include "aux_serial.h"
#include "morse.h"
#include "exception.h"

#include <libopencm3/stm32/rcc.h>
#include <libopencm3/cm3/scb.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/stm32/exti.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/stm32/adc.h>
#include <libopencm3/stm32/syscfg.h>
#include <libopencm3/usb/usbd.h>
#include <libopencm3/cm3/systick.h>
#include <libopencm3/cm3/cortex.h>
#include <libopencm3/usb/dwc/otg_fs.h>

#define CTXLINK_BATTERY_INPUT        0 // ADC Channel for battery input
#define CTXLINK_TARGET_VOLTAGE_INPUT 8 // ADC Chanmel for target voltage

#define CTXLINK_ADC_BATTERY 0
#define CTXLINK_ADC_TARGET  1

static uint32_t input_voltages[2] = {0};
static uint8_t adc_channels[] = {CTXLINK_BATTERY_INPUT, CTXLINK_TARGET_VOLTAGE_INPUT}; /// ADC channels used by ctxLink

static void adc_init(void)
{
	rcc_periph_clock_enable(RCC_ADC1);
	gpio_mode_setup(TPWR_PORT, GPIO_MODE_ANALOG, GPIO_PUPD_NONE, TPWR_PIN); // Target voltage monitor input
	gpio_mode_setup(VBAT_PORT, GPIO_MODE_ANALOG, GPIO_PUPD_NONE, VBAT_PIN); // Battery voltage monitor input

	adc_power_off(ADC1);
	adc_disable_scan_mode(ADC1);
	adc_set_sample_time_on_all_channels(ADC1, ADC_SMPR_SMP_480CYC);

	adc_power_on(ADC1);
	/* Wait for ADC starting up. */
	for (volatile size_t i = 0; i < 800000; i++) /* Wait a bit. */
		__asm__("nop");
}

void platform_adc_read(void)
{
	adc_set_regular_sequence(ADC1, 1, &adc_channels[CTXLINK_ADC_BATTERY]);
	adc_start_conversion_regular(ADC1);
	/* Wait for end of conversion. */
	while (!adc_eoc(ADC1))
		continue;
	input_voltages[CTXLINK_ADC_BATTERY] = adc_read_regular(ADC1);
	adc_set_regular_sequence(ADC1, 1, &adc_channels[CTXLINK_ADC_TARGET]);
	adc_start_conversion_regular(ADC1);
	/* Wait for end of conversion. */
	while (!adc_eoc(ADC1))
		continue;
	input_voltages[CTXLINK_ADC_TARGET] = adc_read_regular(ADC1);
}

// TODO Fix this to return the target voltage
uint32_t platform_target_voltage_sense(void)
{
	return 0;
}

int platform_hwversion(void)
{
	return 0;
}

void platform_init(void)
{
	/* Enable GPIO peripherals */
	rcc_periph_clock_enable(RCC_GPIOA);
	rcc_periph_clock_enable(RCC_GPIOB);
	rcc_periph_clock_enable(RCC_GPIOC);
	rcc_periph_clock_enable(RCC_GPIOD);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
	//
	// Initialize the "Bootloader" input, it is used in
	// normal running mode as the WPS selector switch
	// The switch is active low and therefore needs
	// a pullup
	//
	gpio_mode_setup(SWITCH_PORT, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP, SW_BOOTLOADER_PIN);
	/*
	Check the Bootloader button, not sure this is needed fro the native-derived hardware, need to check if
	this switch is looked at in the DFU bootloader
	*/
	if (!(gpio_get(SWITCH_PORT,
			SW_BOOTLOADER_PIN))) // SJP - 0118_2016, changed to use defs in platform.h and the switch is active low!
	{
		platform_request_boot(); // Does not return from this call
	}

#pragma GCC diagnostic pop
	rcc_clock_setup_pll(&rcc_hse_8mhz_3v3[RCC_CLOCK_3V3_84MHZ]);

	/* Enable peripherals */
	rcc_periph_clock_enable(RCC_OTGFS);
	rcc_periph_clock_enable(RCC_CRC);

	/* Set up USB pins and alternate function */
	gpio_mode_setup(GPIOA, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO9 | GPIO11 | GPIO12);
	gpio_set_af(GPIOA, GPIO_AF10, GPIO9 | GPIO10 | GPIO11 | GPIO12);

	GPIOC_OSPEEDR &= ~0xf30U;
	GPIOC_OSPEEDR |= 0xa20U;

	gpio_mode_setup(JTAG_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, TCK_PIN | TDI_PIN);
	gpio_mode_setup(JTAG_PORT, GPIO_MODE_INPUT, GPIO_PUPD_NONE, TMS_PIN);
	gpio_set_output_options(JTAG_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ, TCK_PIN | TDI_PIN | TMS_PIN);
	gpio_mode_setup(TDO_PORT, GPIO_MODE_INPUT, GPIO_PUPD_NONE, TDO_PIN);
	gpio_set_output_options(TDO_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_2MHZ, TDO_PIN | TMS_PIN);

	gpio_mode_setup(LED_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, LED_IDLE_RUN | LED_ERROR | LED_MODE);

	gpio_mode_setup(LED_PORT_UART, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, LED_UART);

#ifdef PLATFORM_HAS_POWER_SWITCH
	gpio_set(PWR_BR_PORT, PWR_BR_PIN);
	gpio_mode_setup(PWR_BR_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, PWR_BR_PIN);
#endif

	platform_timing_init();
	adc_init();
	blackmagic_usb_init();
	aux_serial_init();

	/* https://github.com/libopencm3/libopencm3/pull/1256#issuecomment-779424001 */
	OTG_FS_GCCFG |= OTG_GCCFG_NOVBUSSENS | OTG_GCCFG_PWRDWN;
	OTG_FS_GCCFG &= ~(OTG_GCCFG_VBUSBSEN | OTG_GCCFG_VBUSASEN);

	/* By default, do not drive the SWD bus too fast. */
}

void platform_nrst_set_val(bool assert)
{
	(void)assert;
}

bool platform_nrst_get_val(void)
{
	return false;
}

const char *platform_target_voltage(void)
{
	return NULL;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"

void platform_request_boot(void)
{
	scb_reset_system();
}

#pragma GCC diagnostic pop

#ifdef PLATFORM_HAS_POWER_SWITCH
bool platform_target_get_power(void)
{
	return !gpio_get(PWR_BR_PORT, PWR_BR_PIN);
}

bool platform_target_set_power(const bool power)
{
	gpio_set_val(PWR_BR_PORT, PWR_BR_PIN, !power);
	return true;
}
#endif

void platform_target_clk_output_enable(bool enable)
{
	(void)enable;
}

bool platform_spi_init(const spi_bus_e bus)
{
	(void)bus;
	return false;
}

bool platform_spi_deinit(const spi_bus_e bus)
{
	(void)bus;
	return false;
}

bool platform_spi_chip_select(const uint8_t device_select)
{
	(void)device_select;
	return false;
}

uint8_t platform_spi_xfer(const spi_bus_e bus, const uint8_t value)
{
	(void)bus;
	return value;
}