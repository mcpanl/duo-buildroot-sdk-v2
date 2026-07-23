/* SPDX-License-Identifier: GPL-2.0 */
/*
 * JD9853 backlight soft-PWM on GPIOA20 (PAD_JTAG_CPU_TRST, active-high).
 * Runs on C906L so Linux only sends on/off + duty via DISPLAY_CMD_BL.
 *
 * IMPORTANT: mid-level duty must use vTaskDelay (not udelay busy-wait).
 * A busy soft-PWM loop starves lower-priority tasks (stats, etc.) and can
 * stall mailbox consumers on this single-core RTOS.
 */
#include "FreeRTOS.h"
#include "task.h"

#include "delay.h"
#include "gpio.h"
#include "mmio.h"
#include "pinctrl.h"
#include "jd9853_panel.h"

#define PIN_BL			GPIOA(20)
#define BL_MAX			100u
/* Coarse software PWM period; must be >= 2 FreeRTOS ticks. */
#define BL_PWM_PERIOD_MS	10u

static volatile unsigned g_bl_level = 0;	/* 0..100 effective duty */
static volatile int g_bl_ready;

static void jd9853_bl_set_pin(int high)
{
	gpio_set_value(PIN_BL, high ? 1 : 0);
}

static void jd9853_bl_pwm_task(void *pvParameters)
{
	(void)pvParameters;

	for (;;) {
		unsigned level = g_bl_level;

		if (!g_bl_ready || level == 0) {
			jd9853_bl_set_pin(0);
			vTaskDelay(pdMS_TO_TICKS(BL_PWM_PERIOD_MS));
			continue;
		}
		if (level >= BL_MAX) {
			jd9853_bl_set_pin(1);
			vTaskDelay(pdMS_TO_TICKS(BL_PWM_PERIOD_MS));
			continue;
		}

		/*
		 * One soft-PWM period with blocking delays so lower-priority
		 * tasks can run. Granularity is one tick (~1–10 ms).
		 */
		{
			TickType_t period = pdMS_TO_TICKS(BL_PWM_PERIOD_MS);
			TickType_t on_ticks;
			TickType_t off_ticks;

			if (period < 2)
				period = 2;
			on_ticks = (period * level) / BL_MAX;
			if (on_ticks == 0)
				on_ticks = 1;
			if (on_ticks >= period)
				on_ticks = period - 1;
			off_ticks = period - on_ticks;

			jd9853_bl_set_pin(1);
			vTaskDelay(on_ticks);
			jd9853_bl_set_pin(0);
			vTaskDelay(off_ticks);
		}
	}
}

void jd9853_bl_pwm_init(void)
{
	PINMUX_CONFIG(JTAG_CPU_TRST, XGPIOA_20);
	gpio_direction_output(PIN_BL, 0);
	g_bl_level = 0;
	g_bl_ready = 1;

	xTaskCreate(jd9853_bl_pwm_task, "BL_PWM", configMINIMAL_STACK_SIZE,
		    NULL, tskIDLE_PRIORITY + 2, NULL);
}

void jd9853_set_backlight_level(unsigned level)
{
	if (level > BL_MAX)
		level = BL_MAX;
	g_bl_level = level;

	/* Apply steady levels immediately so off/full-on do not wait for task. */
	if (!g_bl_ready)
		return;
	if (level == 0)
		jd9853_bl_set_pin(0);
	else if (level >= BL_MAX)
		jd9853_bl_set_pin(1);
}

unsigned jd9853_get_backlight_level(void)
{
	return g_bl_level;
}

void jd9853_set_backlight(int on)
{
	jd9853_set_backlight_level(on ? BL_MAX : 0);
}
