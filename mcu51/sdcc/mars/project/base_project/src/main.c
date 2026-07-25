/*
 * 8051 MCU status LED on GPIOE[0] / PWR_GPIO[0] (RTC power domain).
 *
 * Scheme A: RTC DW timer one-shot + LED state machine (no software busy-wait).
 *
 * Always-on note:
 *   TOP pinmux (0x030010a4) lives in the main power domain and is lost when
 *   VDD_SYS drops. RTC pad unlock at RTC_IOCTRL survives on VBAT/RTC 1.8V.
 *   Pad PWR_GPIO0 default function is already PWR_GPIO_0; after main-power
 *   loss the MCU must keep the RTC unlock bit set and only touch RTC_GPIO /
 *   RTC timer / RTC_INFO — never TOP MMIO (can hang the 8051 robot).
 *
 * Mem wake (GPIOE[1] / PWR_GPIO[1], active-low with external pull-up):
 *   Linux gpio-keys + wakeup-source only arms a GIC IRQ. True mem (ST_SUSP)
 *   drops the main domain / GIC, so that path cannot wake. PWR_GPIO1 is also
 *   not a dedicated PWR_WAKEUP/PWR_BUTTON pad. While FSM is ST_SUSP/PRE_SUSP
 *   the MCU polls GPIOE1 and, on press, programs an RTC alarm +
 *   RTC_EN_PWR_WAKEUP[5:4] — the same hardware resume path as Linux rtcwake.
 *
 * RTC_INFO mailbox (Linux <-> 8051):
 *   RTC_INFO0 (0x0502601c): alive magic 0x8051
 *   RTC_INFO1 (0x05026020): [7:0] LED mode (Linux)
 *                           [31:16] Linux heartbeat (optional status)
 *                           0 = blink 300ms ON / 700ms OFF (default / release)
 *                           1 = blink 1000ms ON / 1000ms OFF
 *                           2 = constant ON
 *                           3 = constant OFF
 *   RTC_INFO2 (0x05026024): free-running run_ms (MCU; +phase each timer fire)
 *   RTC_INFO3 (0x05026028): [7:0] applied mode echo
 *                           [31:8] completed blink-loop count
 */

#include "cvi1822.h"
#include "chip_cv1822.h"
#include "cvi_gpio.h"
#include "dw_timer.h"

#define LED_GPIO_BASE		RTC_GPIO_BASE
#define LED_PIN			0U	/* PWR_GPIO0 / GPIOE_0 */
#define WAKE_PIN		1U	/* PWR_GPIO1 / GPIOE_1, active-low */

/* DW APB GPIO input data (same offset as dw_gpio.h REG_GPIO_READ). */
#define REG_GPIO_EXT_PORTA	(RTC_GPIO_BASE + 0x50UL)

/*
 * RTC_IOCTRL pad unlock (always-on). CV181x pad offsets match TOP FMUX:
 *   PWR_GPIO0 @ +0xa4, PWR_GPIO1 @ +0xa8 (see cv181x_reg_fmux_gpio.h).
 * Same idea as Duo writing 0x05027078/7c for its PWR_GPIO0/1 pads.
 * Write 0x11 (or 0x11111111) to unlock the pad buffer.
 */
#define RTC_IO_PWR_GPIO0	(RTC_IOCTRL + 0xa4UL)
#define RTC_IO_PWR_GPIO1	(RTC_IOCTRL + 0xa8UL)
#define RTC_IO_UNLOCK_VAL	0x11UL

/* RTC core regs used for alarm resume (offsets from RTC_BASE). */
#define RTC_ALARM_TIME		(RTC_BASE + 0x08UL)
#define RTC_ALARM_ENABLE	(RTC_BASE + 0x0CUL)
#define RTC_SEC_CNTR_VALUE	(RTC_BASE + 0x18UL)
#define RTC_APB_RDATA_SEL	(RTC_BASE + 0x3CUL)
#define RTC_EN_PWR_WAKEUP	(RTC_BASE + 0xBCUL)
#define RTC_ALARM_WAKE_BITS	0x30UL	/* bits [5:4], same as cvi_rtc_set_alarm */

/*
 * Probe scratch in RTC SRAM (0x05200000..0x05201fff).
 *
 * Firmware code is linked with --code-size 0x1F00, so the top 0x100 bytes of
 * the first 8 KiB bank are unused by code image. Use the tail for a simple
 * retention / liveness probe across suspend and RTC-only scenarios.
 */
#define RTC_SCRATCH_BASE	(RTC_SRAM_BASE + 0x1fe0UL)
#define RTC_SCRATCH_MAGIC	(RTC_SCRATCH_BASE + 0x00UL)
#define RTC_SCRATCH_SEQ		(RTC_SCRATCH_BASE + 0x04UL)
#define RTC_SCRATCH_MIX		(RTC_SCRATCH_BASE + 0x08UL)
#define RTC_SCRATCH_STATE	(RTC_SCRATCH_BASE + 0x0cUL)
#define RTC_SCRATCH_MAGIC_VAL	0x4d435530UL	/* "MCU0" */

#define LED_MODE_BLINK0		0U	/* 300 / 700 */
#define LED_MODE_BLINK1		1U	/* 1000 / 1000 */
#define LED_MODE_ON		2U
#define LED_MODE_OFF		3U
#define LED_MODE_MAX		LED_MODE_OFF

#define LED_PHASE_ON		0U
#define LED_PHASE_OFF		1U

/* 25 MHz DW timer: 25000 ticks = 1 ms (same as dw_timer_mdelay). */
#define TIMER_TICKS_PER_MS	25000UL

/* Service mailbox every N loops. */
#define MAILBOX_DIV		32U

/* Active-low debounce: consecutive low samples while in ST_SUSP. */
#define WAKE_DEBOUNCE		8U

static void rtc_pad_unlock(void)
{
	/* RTC-domain only — safe after main power loss. */
	mmio_write_32(RTC_IO_PWR_GPIO0, RTC_IO_UNLOCK_VAL);
	/* Keep wake input pad unlocked too (input path). */
	mmio_write_32(RTC_IO_PWR_GPIO1, RTC_IO_UNLOCK_VAL);
}

static void led_hw_init(void)
{
	rtc_pad_unlock();
	set_gpio_direction(LED_GPIO_BASE, LED_PIN, GPIO_DIRECTION_OUT);
	set_gpio_level(LED_GPIO_BASE, LED_PIN, GPIO_LEVEL_LOW);
	/* Wake key: input only, never drive. */
	set_gpio_direction(LED_GPIO_BASE, WAKE_PIN, GPIO_DIRECTION_IN);
}

static void led_apply_level(uint8_t on)
{
	/* Re-assert unlock each drive: survives TOP/power sequencing glitches. */
	rtc_pad_unlock();
	set_gpio_direction(LED_GPIO_BASE, LED_PIN, GPIO_DIRECTION_OUT);
	set_gpio_level(LED_GPIO_BASE, LED_PIN,
		       on ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW);
	/* Keep wake pin as input after any DDR RMW on the bank. */
	set_gpio_direction(LED_GPIO_BASE, WAKE_PIN, GPIO_DIRECTION_IN);
}

static uint8_t read_mode(uint32_t info1)
{
	uint32_t v = info1 & 0xFFU;

	if (v > LED_MODE_MAX)
		return LED_MODE_BLINK0;
	return (uint8_t)v;
}

static void mode_to_ms(uint8_t mode, uint16_t *on_ms, uint16_t *off_ms)
{
	if (mode == LED_MODE_BLINK1) {
		*on_ms = 1000;
		*off_ms = 1000;
	} else {
		/* Default blink and unknown: 300/700 */
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

static void write_info3(uint8_t mode, uint32_t loop_count)
{
	mmio_write_32(RTC_INFO3, ((loop_count & 0xFFFFFFU) << 8) | mode);
}

static void scratch_write(uint32_t seq, uint32_t run_ms, uint32_t loop_count,
			  uint8_t mode, uint8_t phase, uint16_t phase_ms)
{
	uint32_t mix;
	uint32_t state;

	/*
	 * Lightweight evolving checksum so we can distinguish "memory retained but
	 * never updated" from "MCU kept executing and storing new values".
	 */
	mix = seq ^ (run_ms << 1) ^ (run_ms >> 3) ^
	      (loop_count << 9) ^ ((uint32_t)mode << 24) ^
	      ((uint32_t)phase << 20) ^ (uint32_t)phase_ms ^ 0x9e3779b9UL;
	state = ((uint32_t)mode << 24) | ((uint32_t)phase << 16) | phase_ms;

	mmio_write_32(RTC_SCRATCH_MAGIC, RTC_SCRATCH_MAGIC_VAL);
	mmio_write_32(RTC_SCRATCH_SEQ, seq);
	mmio_write_32(RTC_SCRATCH_MIX, mix);
	mmio_write_32(RTC_SCRATCH_STATE, state);
}

/*
 * Request ST_SUSP -> ST_ON via RTC alarm wake bits (Linux cvi_rtc_set_alarm).
 * ALARM_TIME = now so SEC_CNTR >= alarm fires as soon as enable latches.
 */
static void rtc_req_resume_via_alarm(void)
{
	uint32_t now;
	uint32_t mask;

	now = mmio_read_32(RTC_SEC_CNTR_VALUE);

	mmio_write_32(RTC_ALARM_ENABLE, 0);
	(void)mmio_read_32(RTC_ALARM_ENABLE);

	mmio_write_32(RTC_ALARM_TIME, now);
	mmio_write_32(RTC_APB_RDATA_SEL, 1);
	mmio_write_32(RTC_ALARM_ENABLE, 1);

	mask = mmio_read_32(RTC_EN_PWR_WAKEUP) | RTC_ALARM_WAKE_BITS;
	mmio_write_32(RTC_EN_PWR_WAKEUP, mask);

	(void)mmio_read_32(RTC_SEC_CNTR_VALUE);
}

static void check_wake_button(void)
{
	static uint8_t low_count;
	static uint8_t armed;
	static uint8_t saw_released; /* require high then falling edge */
	uint32_t fsm;
	uint32_t ext;

	fsm = mmio_read_32(RTC_FSM_STATE) & 0xFU;
	if (fsm != ST_SUSP && fsm != ST_PRE_SUSP) {
		low_count = 0;
		armed = 0;
		saw_released = 0;
		return;
	}

	/* Once per suspend stay: do not keep re-arming the alarm. */
	if (armed)
		return;

	rtc_pad_unlock();
	set_gpio_direction(LED_GPIO_BASE, WAKE_PIN, GPIO_DIRECTION_IN);
	ext = mmio_read_32(REG_GPIO_EXT_PORTA);

	/*
	 * Active-low with external pull-up: idle=1, pressed=0.
	 * If the pin is stuck low at suspend entry (no pull-up / short), a
	 * level check would wake immediately. Require a released (high)
	 * sample first, then a debounced falling edge.
	 */
	if ((ext & (1UL << WAKE_PIN)) != 0UL) {
		saw_released = 1;
		low_count = 0;
		return;
	}

	if (!saw_released)
		return;

	if (low_count < 0xFFU)
		low_count++;

	if (low_count < WAKE_DEBOUNCE)
		return;

	rtc_req_resume_via_alarm();
	armed = 1;
	low_count = 0;
}

void main(void)
{
	uint32_t loop_count = 0;
	uint32_t run_ms = 0;
	uint32_t scratch_seq = 0;
	uint32_t info1;
	uint16_t mb_div = 0;
	uint16_t on_ms;
	uint16_t off_ms;
	uint16_t phase_ms;
	uint8_t mode;
	uint8_t phase;

	led_hw_init();

	mmio_write_32(RTC_INFO0, 0x8051);
	mmio_write_32(RTC_INFO2, 0);
	write_info3(LED_MODE_BLINK0, 0);

	info1 = mmio_read_32(RTC_INFO1);
	mode = read_mode(info1);
	mode_to_ms(mode, &on_ms, &off_ms);
	write_info3(mode, 0);

	phase = LED_PHASE_ON;
	phase_ms = on_ms;
	if (mode == LED_MODE_ON) {
		led_apply_level(1);
		phase_ms = 1000;
	} else if (mode == LED_MODE_OFF) {
		led_apply_level(0);
		phase_ms = 1000;
	} else {
		led_apply_level(1);
	}
	scratch_write(++scratch_seq, run_ms, loop_count, mode, phase, phase_ms);
	timer_arm_ms(phase_ms);

	while (1) {
		/* Poll RTC-domain wake key every busy-wait iteration. */
		check_wake_button();

		if (++mb_div >= MAILBOX_DIV) {
			uint8_t new_mode;

			mb_div = 0;
			info1 = mmio_read_32(RTC_INFO1);
			new_mode = read_mode(info1);
			if (new_mode != mode) {
				mode = new_mode;
				mode_to_ms(mode, &on_ms, &off_ms);
				phase = LED_PHASE_ON;
				if (mode == LED_MODE_ON) {
					led_apply_level(1);
					phase_ms = 1000;
				} else if (mode == LED_MODE_OFF) {
					led_apply_level(0);
					phase_ms = 1000;
				} else {
					led_apply_level(1);
					phase_ms = on_ms;
				}
				timer_ack();
				timer_arm_ms(phase_ms);
				write_info3(mode, loop_count);
			}

			mmio_write_32(RTC_INFO0, 0x8051);
			write_info3(mode, loop_count);
			mmio_write_32(RTC_INFO2, run_ms);
			scratch_write(++scratch_seq, run_ms, loop_count, mode, phase,
				      phase_ms);
		}

		if (!timer_expired())
			continue;

		timer_ack();

		run_ms += phase_ms;
		mmio_write_32(RTC_INFO2, run_ms);

		if (mode == LED_MODE_ON) {
			led_apply_level(1);
			phase_ms = 1000;
		} else if (mode == LED_MODE_OFF) {
			led_apply_level(0);
			phase_ms = 1000;
		} else if (phase == LED_PHASE_ON) {
			phase = LED_PHASE_OFF;
			phase_ms = off_ms;
			led_apply_level(0);
		} else {
			phase = LED_PHASE_ON;
			phase_ms = on_ms;
			led_apply_level(1);
			loop_count++;
			write_info3(mode, loop_count);
		}

		scratch_write(++scratch_seq, run_ms, loop_count, mode, phase,
			      phase_ms);
		timer_arm_ms(phase_ms);
	}
}
