/*
 * 8051 USER_LED blink on GPIOA[18] (PAD_JTAG_CPU_TCK).
 *
 * Scheme A: RTC DW timer one-shot + LED state machine (no software busy-wait).
 *
 * GPIOA18 shares SWPORTA_DR with backlight soft-PWM (GPIOA20) and other LEDs.
 * DW GPIO has no atomic bit set/clear, so MCU must not RMW the bank while Linux
 * is alive. Linux bumps INFO1[31:16] heartbeat; MCU drives the pin only after
 * heartbeat goes stale (~2s). Timer / run_ms always advance (survives main-power
 * loss while RTC VBAT keeps RTCSYS alive).
 *
 * RTC_INFO mailbox (Linux <-> 8051):
 *   RTC_INFO0 (0x0502601c): alive magic 0x8051
 *   RTC_INFO1 (0x05026020): [7:0] blink mode (Linux)
 *                           [31:16] Linux heartbeat counter
 *                           0 = 300ms ON / 700ms OFF
 *                           1 = 1000ms ON / 1000ms OFF
 *   RTC_INFO2 (0x05026024): free-running run_ms (MCU; +phase each timer fire)
 *   RTC_INFO3 (0x05026028): [7:0] owner echo (0=linux, 1=mcu)
 *                           [31:8] completed blink-loop count
 */

#include "cvi1822.h"
#include "chip_cv1822.h"
#include "cvi_gpio.h"
#include "dw_timer.h"

#define LED_GPIO_BASE		GPIO0_BASE
#define LED_PIN			18

#define PINMUX_JTAG_CPU_TCK	0x03001068UL
#define PINMUX_FUNC_XGPIOA_18	3U
#define REG_RTCSYS_CTRL		0x03000248UL

#define BLINK_MODE0		0U	/* 300 / 700 */
#define BLINK_MODE1		1U	/* 1000 / 1000 */

#define LED_PHASE_ON		0U
#define LED_PHASE_OFF		1U

#define OWNER_LINUX		0U
#define OWNER_MCU		1U

/* 25 MHz DW timer: 25000 ticks = 1 ms (same as dw_timer_mdelay). */
#define TIMER_TICKS_PER_MS	25000UL

/* Service mailbox every N loops. */
#define MAILBOX_DIV		32U

/*
 * Stale threshold in ms of elapsed blink-phase time without HB change.
 * Linux bumps HB every ~500ms; 2000ms leaves margin.
 */
#define HB_STALE_MS		2000U

static void led_hw_init(void)
{
	/* Pinmux only; do not touch DR until MCU owns the LED. */
	mmio_write_32(REG_RTCSYS_CTRL, 0x1);
	mmio_write_32(PINMUX_JTAG_CPU_TCK, PINMUX_FUNC_XGPIOA_18);
}

static void led_claim_and_apply(uint8_t phase)
{
	set_gpio_direction(LED_GPIO_BASE, LED_PIN, GPIO_DIRECTION_OUT);
	set_gpio_level(LED_GPIO_BASE, LED_PIN,
		       phase == LED_PHASE_ON ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW);
}

static uint8_t read_mode(uint32_t info1)
{
	uint32_t v = info1 & 0xFFU;

	if (v > BLINK_MODE1)
		return BLINK_MODE0;
	return (uint8_t)v;
}

static uint16_t read_hb(uint32_t info1)
{
	return (uint16_t)((info1 >> 16) & 0xFFFFU);
}

static void mode_to_ms(uint8_t mode, uint16_t *on_ms, uint16_t *off_ms)
{
	if (mode == BLINK_MODE1) {
		*on_ms = 1000;
		*off_ms = 1000;
	} else {
		*on_ms = 300;
		*off_ms = 700;
	}
}

static void timer_arm_ms(uint16_t ms)
{
	mmio_write_32(REG_TIMER1_CONTROL, 0x0);
	(void)mmio_read_32(REG_TIMER1_EOI);
	mmio_write_32(REG_TIMER1_LOADCNT, (uint32_t)ms * TIMER_TICKS_PER_MS);
	mmio_write_32(REG_TIMER1_CONTROL, 0x3);
}

static uint8_t timer_expired(void)
{
	return mmio_read_32(REG_TIMER1_INTSTATUS) != 0U;
}

static void timer_ack(void)
{
	(void)mmio_read_32(REG_TIMER1_EOI);
	mmio_write_32(REG_TIMER1_CONTROL, 0x0);
}

static void write_info3(uint8_t owner, uint32_t loop_count)
{
	mmio_write_32(RTC_INFO3, ((loop_count & 0xFFFFFFU) << 8) | owner);
}

void main(void)
{
	uint32_t loop_count = 0;
	uint32_t run_ms = 0;
	uint32_t info1;
	uint16_t mb_div = 0;
	uint16_t on_ms;
	uint16_t off_ms;
	uint16_t last_hb;
	uint16_t stale_ms = 0;
	uint16_t phase_ms;
	uint8_t mode;
	uint8_t phase;
	uint8_t owner;
	uint8_t claimed = 0;

	led_hw_init();

	/* Default linux_owns until HB goes stale (~2s). Do not write INFO1
	 * (Linux owns mode + heartbeat). */
	owner = OWNER_LINUX;
	mmio_write_32(RTC_INFO0, 0x8051);
	mmio_write_32(RTC_INFO2, 0);
	write_info3(OWNER_LINUX, 0);

	info1 = mmio_read_32(RTC_INFO1);
	mode = read_mode(info1);
	last_hb = read_hb(info1);
	mode_to_ms(mode, &on_ms, &off_ms);

	phase = LED_PHASE_ON;
	phase_ms = on_ms;
	timer_arm_ms(phase_ms);

	while (1) {
		if (++mb_div >= MAILBOX_DIV) {
			mb_div = 0;
			info1 = mmio_read_32(RTC_INFO1);
			mode = read_mode(info1);
			mode_to_ms(mode, &on_ms, &off_ms);

			if (read_hb(info1) != last_hb) {
				last_hb = read_hb(info1);
				stale_ms = 0;
				if (owner != OWNER_LINUX) {
					owner = OWNER_LINUX;
					claimed = 0;
					write_info3(OWNER_LINUX, loop_count);
				}
			}

			mmio_write_32(RTC_INFO0, 0x8051);
			write_info3(owner, loop_count);
			/* Refresh run_ms so Linux can observe progress even mid-phase. */
			mmio_write_32(RTC_INFO2, run_ms);
		}

		if (!timer_expired())
			continue;

		timer_ack();

		/* Free-running uptime in RTC domain (independent of LED owner). */
		run_ms += phase_ms;
		mmio_write_32(RTC_INFO2, run_ms);

		/* Accumulate phase duration toward HB stale detection. */
		if (owner == OWNER_LINUX) {
			uint16_t add = phase_ms;

			if ((uint16_t)(stale_ms + add) < stale_ms)
				stale_ms = HB_STALE_MS;
			else
				stale_ms = (uint16_t)(stale_ms + add);

			if (stale_ms >= HB_STALE_MS) {
				owner = OWNER_MCU;
				claimed = 0;
				write_info3(OWNER_MCU, loop_count);
			}
		}

		if (phase == LED_PHASE_ON) {
			phase = LED_PHASE_OFF;
			phase_ms = off_ms;
		} else {
			phase = LED_PHASE_ON;
			phase_ms = on_ms;
			loop_count++;
			write_info3(owner, loop_count);
		}

		if (owner == OWNER_MCU) {
			if (!claimed) {
				claimed = 1;
			}
			led_claim_and_apply(phase);
		}

		timer_arm_ms(phase_ms);
	}
}
