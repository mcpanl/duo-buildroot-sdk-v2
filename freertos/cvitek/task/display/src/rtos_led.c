/* SPDX-License-Identifier: GPL-2.0 */
/*
 * FreeRTOS status LED on GPIOA18 (PAD_JTAG_CPU_TCK, active-high).
 *
 * Early boot: RTOS blinks GPIOA18.
 * After Linux claims the GPIOA bank (sys-led A29 / lcd-bl A20 via bgpio), any
 * RTOS RMW of SWPORTA_DR races Linux's software shadow and the LED sticks on.
 * Once display_shm.linux_ready is set (or a fallback timeout elapses), RTOS
 * stops driving the pin so Linux gpio-leds "rtos-led" can own it.
 *
 * DISPLAY_CMD_LED still updates the logical mode for early-boot / pre-handoff.
 */
#include "FreeRTOS.h"
#include "task.h"
#include "gpio.h"
#include "mmio.h"
#include "pinctrl.h"
#include "arch_helpers.h"
#include "display_shm.h"
#include "rtos_led.h"
#include "printf.h"

#define PIN_LED			GPIOA(18)
#define BLINK_ON_MS		300u
#define BLINK_OFF_MS		700u
/* If proxy never sets linux_ready, stop fighting GPIOA after this.
 * gpio-leds (sys-led / rtos-led) typically claims the bank within a few
 * seconds of Linux boot — keep the RTOS window short.
 */
#define HANDOFF_FALLBACK_MS	5000u

static volatile unsigned g_led_mode = DISPLAY_LED_BLINK;
static volatile int g_led_level;
static volatile int g_led_handed_off;
static TaskHandle_t g_led_task;
static struct display_shm *g_led_shm;

static int rtos_led_linux_owns(void)
{
	if (!g_led_shm)
		return 0;
	inv_dcache_range((uintptr_t)g_led_shm, 64);
	return g_led_shm->linux_ready ? 1 : 0;
}

static void rtos_led_drive(int on)
{
	if (g_led_handed_off)
		return;

	g_led_level = on ? 1 : 0;
	gpio_set_value(PIN_LED, g_led_level);
}

void rtos_led_set_mode(unsigned mode)
{
	switch (mode) {
	case DISPLAY_LED_ON:
	case DISPLAY_LED_OFF:
	case DISPLAY_LED_BLINK:
	case DISPLAY_LED_RELEASE:
		break;
	default:
		mode = DISPLAY_LED_RELEASE;
		break;
	}

	if (mode == DISPLAY_LED_RELEASE)
		mode = DISPLAY_LED_BLINK;

	g_led_mode = mode;

	if (g_led_handed_off)
		return;

	if (mode == DISPLAY_LED_ON)
		rtos_led_drive(1);
	else if (mode == DISPLAY_LED_OFF)
		rtos_led_drive(0);
}

unsigned rtos_led_get_mode(void)
{
	return g_led_mode;
}

static void prvRtosLedTask(void *pvParameters)
{
	TickType_t delay;
	TickType_t start = xTaskGetTickCount();

	(void)pvParameters;

	g_led_shm = (struct display_shm *)(uintptr_t)CVIMMAP_DISPLAY_SHM_ADDR;

	PINMUX_CONFIG(JTAG_CPU_TCK, XGPIOA_18);
	gpio_direction_output(PIN_LED, 1);
	g_led_level = 1;
	g_led_mode = DISPLAY_LED_BLINK;
	g_led_handed_off = 0;
	printf("rtos_led: task start GPIOA18 blink\n");

	for (;;) {
		if (!g_led_handed_off) {
			TickType_t elapsed = xTaskGetTickCount() - start;

			if (rtos_led_linux_owns() ||
			    elapsed >= pdMS_TO_TICKS(HANDOFF_FALLBACK_MS)) {
				g_led_handed_off = 1;
				printf("rtos_led: hand off GPIOA18 to Linux\n");
				/*
				 * Park in a long delay loop; mode updates are
				 * ignored for pin drive after handoff.
				 */
				for (;;)
					vTaskDelay(pdMS_TO_TICKS(1000));
			}
		}

		{
			unsigned mode = g_led_mode;

			if (mode == DISPLAY_LED_ON) {
				if (!g_led_level)
					rtos_led_drive(1);
				delay = pdMS_TO_TICKS(200);
			} else if (mode == DISPLAY_LED_OFF) {
				if (g_led_level)
					rtos_led_drive(0);
				delay = pdMS_TO_TICKS(200);
			} else {
				if (g_led_level) {
					rtos_led_drive(0);
					delay = pdMS_TO_TICKS(BLINK_OFF_MS);
				} else {
					rtos_led_drive(1);
					delay = pdMS_TO_TICKS(BLINK_ON_MS);
				}
			}
		}

		vTaskDelay(delay);
	}
}

void rtos_led_start(void)
{
	if (g_led_task)
		return;

	if (xTaskCreate(prvRtosLedTask, "RTOS_LED",
			configMINIMAL_STACK_SIZE * 2, NULL,
			tskIDLE_PRIORITY + 2, &g_led_task) != pdPASS) {
		g_led_task = NULL;
		printf("rtos_led: create failed\n");
	}
}
