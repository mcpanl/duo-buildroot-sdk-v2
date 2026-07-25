/* SPDX-License-Identifier: GPL-2.0 */
/*
 * JD9853 backlight on GPIOA20 (PAD_JTAG_CPU_TRST, active-high).
 *
 * Early boot (U-Boot + RTOS before Linux lcd-bl): this file drives the pin.
 * After Linux sets display_shm.linux_ready and probes zonhor_lcd_bl, Linux
 * owns GPIOA20 via gpiod. RTOS must stop RMW on SWPORTA_DR — otherwise it
 * races Linux bgpio shadow (sys-led on A29) and BL flashes with activity.
 *
 * Dimming is digital only here: 0 = off, 1..100 = on. True PWM needs a
 * dedicated PWM block or a pin outside GPIOA.
 */
#include "gpio.h"
#include "mmio.h"
#include "pinctrl.h"
#include "jd9853_panel.h"
#include "display_shm.h"
#include "arch_helpers.h"

#define PIN_BL			GPIOA(20)
#define BL_MAX			100u

static volatile unsigned g_bl_level = 0;
static volatile int g_bl_ready;
static struct display_shm *g_bl_shm;

static int jd9853_bl_linux_owns_pin(void)
{
	if (!g_bl_shm)
		return 0;
	inv_dcache_range((uintptr_t)g_bl_shm, 64);
	return g_bl_shm->linux_ready ? 1 : 0;
}

void jd9853_bl_pwm_init(unsigned initial_level)
{
	if (initial_level > BL_MAX)
		initial_level = BL_MAX;

	g_bl_shm = (struct display_shm *)(uintptr_t)CVIMMAP_DISPLAY_SHM_ADDR;

	PINMUX_CONFIG(JTAG_CPU_TRST, XGPIOA_20);
	/*
	 * Inherit U-Boot backlight state. Do not force low here — that used
	 * to create a multi-second black gap until Linux lcd-bl probed.
	 */
	gpio_direction_output(PIN_BL, initial_level ? 1 : 0);
	g_bl_level = initial_level;
	g_bl_ready = 1;
}

void jd9853_set_backlight_level(unsigned level)
{
	if (level > BL_MAX)
		level = BL_MAX;
	g_bl_level = level;

	if (!g_bl_ready)
		return;

	/* Hand off pin to Linux once lcd-bl / proxy has claimed GPIOA. */
	if (jd9853_bl_linux_owns_pin())
		return;

	gpio_set_value(PIN_BL, level ? 1 : 0);
}

unsigned jd9853_get_backlight_level(void)
{
	return g_bl_level;
}

void jd9853_set_backlight(int on)
{
	jd9853_set_backlight_level(on ? BL_MAX : 0);
}
