/*
 * 8051 USER_LED blink demo on GPIOA[18] (PAD_JTAG_CPU_TCK).
 *
 * RTC_INFO mailbox (Linux <-> 8051):
 *   RTC_INFO0 (0x0502601c): alive magic 0x8051
 *   RTC_INFO1 (0x05026020): blink mode written by Linux
 *                           0 = 300ms ON / 700ms OFF
 *                           1 = 1000ms ON / 1000ms OFF
 *   RTC_INFO2 (0x05026024): completed blink-loop count (MCU)
 *   RTC_INFO3 (0x05026028): current mode echo (MCU)
 */

#include "cvi1822.h"
#include "chip_cv1822.h"
#include "cvi_gpio.h"

#define LED_GPIO_BASE		GPIO0_BASE
#define LED_PIN			18

#define PINMUX_JTAG_CPU_TCK	0x03001068UL
#define PINMUX_FUNC_XGPIOA_18	3U
#define REG_RTCSYS_CTRL		0x03000248UL

#define BLINK_MODE0		0U	/* 300 / 700 */
#define BLINK_MODE1		1U	/* 1000 / 1000 */

/*
 * Busy-wait delay. Tuned for ~25MHz MCU51 with robot MMIO overhead
 * kept out of the hot path. Accuracy is good enough for LED demo.
 */
static void delay_ms(uint16_t ms)
{
	uint16_t m;
	uint16_t n;

	for (m = 0; m < ms; m++) {
		/* ~1ms at 25MHz (empirical for this core/toolchain) */
		for (n = 0; n < 600U; n++) {
			__asm
				nop
			__endasm;
		}
	}
}

static void led_init(void)
{
	mmio_write_32(REG_RTCSYS_CTRL, 0x1);
	mmio_write_32(PINMUX_JTAG_CPU_TCK, PINMUX_FUNC_XGPIOA_18);
	set_gpio_direction(LED_GPIO_BASE, LED_PIN, GPIO_DIRECTION_OUT);
	set_gpio_level(LED_GPIO_BASE, LED_PIN, GPIO_LEVEL_LOW);
}

static uint8_t read_mode(void)
{
	uint32_t v = mmio_read_32(RTC_INFO1) & 0xFFU;

	if (v > BLINK_MODE1)
		return BLINK_MODE0;
	return (uint8_t)v;
}

void main(void)
{
	uint32_t loop_count = 0;
	uint8_t mode;
	uint16_t on_ms;
	uint16_t off_ms;

	led_init();

	mmio_write_32(RTC_INFO0, 0x8051);
	mmio_write_32(RTC_INFO1, BLINK_MODE0);
	mmio_write_32(RTC_INFO2, 0);
	mmio_write_32(RTC_INFO3, BLINK_MODE0);

	while (1) {
		mode = read_mode();
		if (mode == BLINK_MODE1) {
			on_ms = 1000;
			off_ms = 1000;
		} else {
			on_ms = 300;
			off_ms = 700;
		}

		mmio_write_32(RTC_INFO3, mode);
		mmio_write_32(RTC_INFO0, 0x8051);

		set_gpio_level(LED_GPIO_BASE, LED_PIN, GPIO_LEVEL_HIGH);
		delay_ms(on_ms);

		set_gpio_level(LED_GPIO_BASE, LED_PIN, GPIO_LEVEL_LOW);
		delay_ms(off_ms);

		loop_count++;
		mmio_write_32(RTC_INFO2, loop_count);
	}
}
