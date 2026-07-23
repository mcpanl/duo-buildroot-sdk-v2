/**
 * mcu51-ledctl - Control 8051 USER_LED blink demo via RTC_INFO mailbox.
 *
 * Usage:
 *   mcu51-ledctl mode 0|1     # 0: 300/700ms, 1: 1000/1000ms
 *   mcu51-ledctl count        # print blink loop count (INFO3[31:8])
 *   mcu51-ledctl run-ms       # print free-running run_ms (INFO2)
 *   mcu51-ledctl status       # alive / mode / hb / owner / run_ms / count
 *   mcu51-ledctl hb-once      # bump Linux heartbeat in INFO1[31:16]
 *   mcu51-ledctl release-led  # hand USER_LED from Linux gpio-leds to MCU
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

#define OWNER_LINUX		0U
#define OWNER_MCU		1U

#define USER_LED_TRIGGER	"/sys/class/leds/user-led/trigger"

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s mode 0|1      set blink mode (0=300/700ms, 1=1000/1000ms)\n"
		"  %s count         print blink loop count (INFO3[31:8])\n"
		"  %s run-ms        print free-running run_ms (INFO2)\n"
		"  %s status        show alive/mode/hb/owner/run_ms/count\n"
		"  %s hb-once       bump Linux heartbeat (INFO1[31:16])\n"
		"  %s release-led   stop kernel gpio-leds trigger on user-led\n",
		prog, prog, prog, prog, prog, prog);
}

static int write_sysfs(const char *path, const char *val)
{
	FILE *f = fopen(path, "w");

	if (!f)
		return -1;
	if (fputs(val, f) < 0) {
		fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

static int release_user_led(void)
{
	if (access(USER_LED_TRIGGER, W_OK) != 0) {
		fprintf(stderr, "mcu51-ledctl: %s not present (ok if LED unbound)\n",
			USER_LED_TRIGGER);
		return 0;
	}
	if (write_sysfs(USER_LED_TRIGGER, "none") != 0) {
		perror("set trigger=none");
		return 1;
	}
	printf("mcu51-ledctl: released user-led trigger=none\n");
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
	uint32_t alive;
	uint32_t info1;
	uint16_t hb;

	if (mode != 0 && mode != 1) {
		fprintf(stderr, "mode must be 0 or 1\n");
		return 1;
	}

	/* Preserve heartbeat; only update mode byte. */
	info1 = devmem_readl(RTC_INFO1);
	hb = (uint16_t)((info1 >> INFO1_HB_SHIFT) & 0xFFFFU);
	info1 = ((uint32_t)mode & INFO1_MODE_MASK) |
		((uint32_t)hb << INFO1_HB_SHIFT);
	devmem_writel(RTC_INFO1, info1);

	alive = devmem_readl(RTC_INFO0);
	if ((alive & 0xFFFFU) != MCU51_MAGIC) {
		fprintf(stderr,
			"warning: MCU not alive (RTC_INFO0=0x%08x); is mcu51-up running?\n",
			alive);
	}
	printf("mcu51-ledctl: mode set to %d (%s)\n", mode,
	       mode == 0 ? "300ms on / 700ms off" : "1000ms on / 1000ms off");
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
	uint8_t mode = (uint8_t)(info1 & INFO1_MODE_MASK);
	uint8_t owner = (uint8_t)(info3 & 0xFFU);
	uint32_t loop_count = (info3 >> 8) & 0xFFFFFFU;

	printf("alive=%s RTC_INFO0=0x%08x\n", alive ? "yes" : "NO", info0);
	printf("cmd_mode=%u (Linux->MCU via RTC_INFO1[7:0])\n", mode);
	printf("hb=%u (Linux->MCU via RTC_INFO1[31:16])\n", hb);
	printf("owner=%s (%u, MCU RTC_INFO3[7:0])\n",
	       owner == OWNER_MCU ? "mcu" : "linux", owner);
	printf("run_ms=%u (MCU RTC_INFO2, free-running)\n", info2);
	printf("loop_count=%u (MCU RTC_INFO3[31:8])\n", loop_count);
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
	if (strcmp(argv[1], "count") == 0)
		return cmd_count();
	if (strcmp(argv[1], "run-ms") == 0)
		return cmd_run_ms();
	if (strcmp(argv[1], "status") == 0)
		return cmd_status();
	if (strcmp(argv[1], "hb-once") == 0)
		return cmd_hb_once();
	if (strcmp(argv[1], "release-led") == 0)
		return release_user_led();

	usage(argv[0]);
	return 1;
}
