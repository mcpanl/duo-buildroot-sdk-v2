/**
 * mcu51_up - Load 8051 firmware into RTC SRAM (or DDR) and release reset.
 *
 * Based on milkv-duo/duo-8051 tools/8051_up, extended for:
 *  - CLI firmware path / boot address
 *  - Factory + runtime update paths
 *  - Optional post-load RTC_INFO0 handshake check
 */

#include "devmem.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RTC_SRAM_BASE		0x05200000UL
#define RTC_INFO0		0x0502601CUL
#define REG_RTCSYS_RST		0x05025018UL
#define REG_MCU51_CTRL0		0x05025020UL
#define REG_RTC2AP_ENABLE	0x03000248UL

#define MCU_FW_FACTORY		"/lib/firmware/mcu51/mars_mcu_fw.bin"
#define MCU_FW_RUNTIME		"/mnt/data/mcu51/mars_mcu_fw.bin"
#define MCU_BOOT_CFG		"/mnt/data/mcu51/boot_cfg.ini"
#define MCU_BOOT_CFG_SYS		"/lib/firmware/mcu51/boot_cfg.ini"

#define MCU51_MAGIC		0x8051U
#define MCU51_MAX_SRAM_SIZE	(8 * 1024)

static uint32_t boot_addr = RTC_SRAM_BASE;
static int check_alive = 1;
static int quiet;

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [-f firmware.bin] [-a boot_addr] [-c cfg.ini] [-n] [-q]\n"
		"  -f  firmware path (default: runtime then factory)\n"
		"  -a  boot address (default 0x05200000 RTC SRAM)\n"
		"  -c  boot cfg file containing hex address\n"
		"  -n  do not wait/check RTC_INFO0==0x8051\n"
		"  -q  quiet\n",
		prog);
}

static int load_boot_addr(const char *filename)
{
	FILE *file;
	char tmp[128];

	file = fopen(filename, "r");
	if (!file)
		return -1;
	if (fgets(tmp, sizeof(tmp), file)) {
		unsigned int addr = 0;
		if (sscanf(tmp, "%x", &addr) == 1 && addr != 0) {
			boot_addr = addr;
			if (!quiet)
				printf("mcu51: boot address 0x%08x (from %s)\n",
				       boot_addr, filename);
			fclose(file);
			return 0;
		}
	}
	fclose(file);
	return -1;
}

static int load_file(const char *filename, unsigned char **out, size_t *out_size)
{
	FILE *f;
	long size;
	unsigned char *buf;

	f = fopen(filename, "rb");
	if (!f)
		return -1;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return -2;
	}
	size = ftell(f);
	if (size <= 0 || size > 512 * 1024) {
		fclose(f);
		return -2;
	}
	if (fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return -2;
	}
	buf = malloc((size_t)size + 4);
	if (!buf) {
		fclose(f);
		return -3;
	}
	memset(buf, 0, (size_t)size + 4);
	if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
		free(buf);
		fclose(f);
		return -2;
	}
	fclose(f);
	*out = buf;
	*out_size = (size_t)size;
	return 0;
}

static void mcu51_hold_reset(void)
{
	/* Keep 8051 in reset while writing firmware */
	devmem_writel(REG_RTCSYS_RST, 0x8107fffd);
}

static void mcu51_release(void)
{
	uint32_t reg = boot_addr;

	reg &= 0xFFFFF000U;
	reg |= 0x084U; /* mars memory scheme + enable */

	devmem_writel(REG_RTC2AP_ENABLE, 0x1);
	devmem_writel(REG_MCU51_CTRL0, reg);
	devmem_writel(REG_RTCSYS_RST, 0x8107ffff);
}

static int mcu51_wait_alive(int timeout_ms)
{
	int waited = 0;

	while (waited < timeout_ms) {
		uint32_t info0 = devmem_readl(RTC_INFO0);
		if ((info0 & 0xFFFFU) == MCU51_MAGIC)
			return 0;
		usleep(10000);
		waited += 10;
	}
	return -1;
}

static const char *pick_default_fw(void)
{
	if (access(MCU_FW_RUNTIME, R_OK) == 0)
		return MCU_FW_RUNTIME;
	if (access(MCU_FW_FACTORY, R_OK) == 0)
		return MCU_FW_FACTORY;
	return NULL;
}

int main(int argc, char *argv[])
{
	const char *fw_path = NULL;
	const char *cfg_path = NULL;
	unsigned char *content = NULL;
	size_t size = 0;
	size_t i;
	int opt;
	int ret;

	while ((opt = getopt(argc, argv, "f:a:c:nqh")) != -1) {
		switch (opt) {
		case 'f':
			fw_path = optarg;
			break;
		case 'a':
			boot_addr = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		case 'c':
			cfg_path = optarg;
			break;
		case 'n':
			check_alive = 0;
			break;
		case 'q':
			quiet = 1;
			break;
		case 'h':
		default:
			usage(argv[0]);
			return (opt == 'h') ? 0 : 1;
		}
	}

	if (cfg_path) {
		load_boot_addr(cfg_path);
	} else {
		if (load_boot_addr(MCU_BOOT_CFG) != 0)
			load_boot_addr(MCU_BOOT_CFG_SYS);
	}

	if (!fw_path)
		fw_path = pick_default_fw();
	if (!fw_path) {
		fprintf(stderr, "mcu51: no firmware found (%s or %s)\n",
			MCU_FW_RUNTIME, MCU_FW_FACTORY);
		return 1;
	}

	ret = load_file(fw_path, &content, &size);
	if (ret != 0) {
		fprintf(stderr, "mcu51: failed to load %s\n", fw_path);
		return 1;
	}

	if (boot_addr == RTC_SRAM_BASE && size > MCU51_MAX_SRAM_SIZE) {
		fprintf(stderr,
			"mcu51: firmware %zu bytes exceeds RTC SRAM limit %d\n",
			size, MCU51_MAX_SRAM_SIZE);
		free(content);
		return 1;
	}

	if (!quiet)
		printf("mcu51: load %s (%zu bytes) @ 0x%08x\n",
		       fw_path, size, boot_addr);

	mcu51_hold_reset();

	for (i = 0; i < size; i += 4) {
		uint32_t value = (uint32_t)content[i] |
				 ((uint32_t)content[i + 1] << 8) |
				 ((uint32_t)content[i + 2] << 16) |
				 ((uint32_t)content[i + 3] << 24);
		devmem_writel(boot_addr + (uint32_t)i, value);
	}

	free(content);
	mcu51_release();

	if (check_alive) {
		if (mcu51_wait_alive(500) != 0) {
			fprintf(stderr,
				"mcu51: RTC_INFO0 handshake failed (got 0x%08x)\n",
				devmem_readl(RTC_INFO0));
			return 2;
		}
		if (!quiet)
			printf("mcu51: alive (RTC_INFO0=0x%08x)\n",
			       devmem_readl(RTC_INFO0));
	}

	if (!quiet)
		printf("mcu51: done\n");
	return 0;
}
