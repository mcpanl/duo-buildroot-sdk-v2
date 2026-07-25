/**
 * mcu51-ledctl - Control 8051 MCU LED on GPIOE0 / PWR_GPIO0 via RTC_INFO.
 *
 * Usage:
 *   mcu51-ledctl mode 0|1|2|3
 *   mcu51-ledctl on | off | blink [0|1] | release
 *   mcu51-ledctl count | run-ms | status | hb-once
 *
 * Modes (RTC_INFO1[7:0]):
 *   0 = blink 300/700ms
 *   1 = blink 1000/1000ms
 *   2 = constant ON
 *   3 = constant OFF (default / release)
 */
#include "devmem.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RTC_INFO0		0x0502601CUL
#define RTC_INFO1		0x05026020UL
#define RTC_INFO2		0x05026024UL
#define RTC_INFO3		0x05026028UL
#define MCU51_MAGIC		0x8051U

#define INFO1_MODE_MASK		0xFFU
#define INFO1_HB_SHIFT		16

#define LED_MODE_BLINK0		0
#define LED_MODE_BLINK1		1
#define LED_MODE_ON		2
#define LED_MODE_OFF		3

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s mode 0|1|2|3  set LED mode\n"
		"                   0=blink 300/700, 1=blink 1000/1000,\n"
		"                   2=on, 3=off\n"
		"  %s on            constant ON (mode 2)\n"
		"  %s off           constant OFF (mode 3)\n"
		"  %s blink [0|1]   blink mode 0 or 1 (default 0)\n"
		"  %s release       restore default OFF (mode 3)\n"
		"  %s count         print blink loop count (INFO3[31:8])\n"
		"  %s run-ms        print free-running run_ms (INFO2)\n"
		"  %s status        show alive/mode/hb/run_ms/count\n"
		"  %s hb-once       bump Linux heartbeat (INFO1[31:16])\n",
		prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

static const char *mode_name(unsigned mode)
{
	switch (mode) {
	case LED_MODE_BLINK0:
		return "blink 300ms on / 700ms off";
	case LED_MODE_BLINK1:
		return "blink 1000ms on / 1000ms off";
	case LED_MODE_ON:
		return "constant ON";
	case LED_MODE_OFF:
		return "constant OFF";
	default:
		return "unknown";
	}
}

static int warn_if_dead(void)
{
	uint32_t alive = devmem_readl(RTC_INFO0);

	if ((alive & 0xFFFFU) != MCU51_MAGIC) {
		fprintf(stderr,
			"warning: MCU not alive (RTC_INFO0=0x%08x); is mcu51-up running?\n",
			alive);
		return 1;
	}
	return 0;
}

static int cmd_hb_once(void)
{
	uint32_t info1 = devmem_readl(RTC_INFO1);
	uint16_t hb = (uint16_t)((info1 >> INFO1_HB_SHIFT) & 0xFFFFU);

	hb++;
	info1 = (info1 & INFO1_MODE_MASK) | ((uint32_t)hb << INFO1_HB_SHIFT);
	devmem_writel(RTC_INFO1, info1);
	return 0;
}

static int cmd_mode(int mode)
{
	uint32_t info1;
	uint16_t hb;

	if (mode < LED_MODE_BLINK0 || mode > LED_MODE_OFF) {
		fprintf(stderr, "mode must be 0..3\n");
		return 1;
	}

	info1 = devmem_readl(RTC_INFO1);
	hb = (uint16_t)((info1 >> INFO1_HB_SHIFT) & 0xFFFFU);
	info1 = ((uint32_t)mode & INFO1_MODE_MASK) |
		((uint32_t)hb << INFO1_HB_SHIFT);
	devmem_writel(RTC_INFO1, info1);

	(void)warn_if_dead();
	printf("mcu51-ledctl: mode set to %d (%s)\n", mode, mode_name(mode));
	return 0;
}

static int cmd_count(void)
{
	uint32_t info3 = devmem_readl(RTC_INFO3);

	printf("%u\n", (info3 >> 8) & 0xFFFFFFU);
	return 0;
}

static int cmd_run_ms(void)
{
	printf("%u\n", devmem_readl(RTC_INFO2));
	return 0;
}

static int cmd_status(void)
{
	uint32_t info0 = devmem_readl(RTC_INFO0);
	uint32_t info1 = devmem_readl(RTC_INFO1);
	uint32_t info2 = devmem_readl(RTC_INFO2);
	uint32_t info3 = devmem_readl(RTC_INFO3);
	int alive = ((info0 & 0xFFFFU) == MCU51_MAGIC);
	uint16_t hb = (uint16_t)((info1 >> INFO1_HB_SHIFT) & 0xFFFFU);
	uint8_t cmd_mode = (uint8_t)(info1 & INFO1_MODE_MASK);
	uint8_t applied = (uint8_t)(info3 & 0xFFU);
	uint32_t loop_count = (info3 >> 8) & 0xFFFFFFU;

	printf("alive=%s RTC_INFO0=0x%08x\n", alive ? "yes" : "NO", info0);
	printf("cmd_mode=%u (%s)\n", cmd_mode, mode_name(cmd_mode));
	printf("applied_mode=%u (%s, MCU RTC_INFO3[7:0])\n",
	       applied, mode_name(applied));
	printf("hb=%u (Linux->MCU via RTC_INFO1[31:16])\n", hb);
	printf("run_ms=%u (MCU RTC_INFO2, free-running)\n", info2);
	printf("loop_count=%u (MCU RTC_INFO3[31:8])\n", loop_count);
	printf("led=GPIOE0/PWR_GPIO0 (RTC domain, MCU exclusive)\n");
	return alive ? 0 : 2;
}

int main(int argc, char *argv[])
{
	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	if (strcmp(argv[1], "mode") == 0) {
		if (argc < 3) {
			usage(argv[0]);
			return 1;
		}
		return cmd_mode(atoi(argv[2]));
	}
	if (strcmp(argv[1], "on") == 0)
		return cmd_mode(LED_MODE_ON);
	if (strcmp(argv[1], "off") == 0)
		return cmd_mode(LED_MODE_OFF);
	if (strcmp(argv[1], "blink") == 0) {
		int mode = LED_MODE_BLINK0;

		if (argc >= 3)
			mode = atoi(argv[2]);
		if (mode != LED_MODE_BLINK0 && mode != LED_MODE_BLINK1) {
			fprintf(stderr, "blink mode must be 0 or 1\n");
			return 1;
		}
		return cmd_mode(mode);
	}
	if (strcmp(argv[1], "release") == 0)
		return cmd_mode(LED_MODE_OFF);
	if (strcmp(argv[1], "count") == 0)
		return cmd_count();
	if (strcmp(argv[1], "run-ms") == 0)
		return cmd_run_ms();
	if (strcmp(argv[1], "status") == 0)
		return cmd_status();
	if (strcmp(argv[1], "hb-once") == 0)
		return cmd_hb_once();
	/* Compatibility alias from older tooling */
	if (strcmp(argv[1], "release-led") == 0)
		return cmd_mode(LED_MODE_OFF);

	usage(argv[0]);
	return 1;
}
