/*
 * axp2101ctl - user space helper for AXP2101 power supply and input events
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/i2c-dev.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define PSY_PATH "/sys/class/power_supply/axp2101-battery"
#define INPUT_BY_NAME "axp2101-pek"
#define AXP2101_I2C_DEV "/dev/i2c-1"
#define AXP2101_I2C_ADDR 0x34

#define AXP2101_PWR_CTRL 0x80
#define AXP2101_DCDC1_V_OUT 0x82
#define AXP2101_DCDC2_V_OUT 0x83
#define AXP2101_DCDC3_V_OUT 0x84
#define AXP2101_DCDC4_V_OUT 0x85
#define AXP2101_DCDC5_V_OUT 0x86
#define AXP2101_LDO_ONOFF0 0x90
#define AXP2101_LDO_ONOFF1 0x91
#define AXP2101_ALDO1_V_OUT 0x92
#define AXP2101_ALDO2_V_OUT 0x93
#define AXP2101_ALDO3_V_OUT 0x94
#define AXP2101_ALDO4_V_OUT 0x95
#define AXP2101_BLDO1_V_OUT 0x96
#define AXP2101_BLDO2_V_OUT 0x97
#define AXP2101_CPUSLDO_V_OUT 0x98
#define AXP2101_DLDO1_V_OUT 0x99
#define AXP2101_DLDO2_V_OUT 0x9a

#define BIT(n) (1U << (n))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

enum axp2101_voltage_type {
	AXP2101_VOLTAGE_LINEAR,
	AXP2101_VOLTAGE_DCDC23,
	AXP2101_VOLTAGE_DCDC4,
};

struct axp2101_output {
	const char *name;
	uint8_t enable_reg;
	uint8_t enable_mask;
	uint8_t voltage_reg;
	uint8_t voltage_mask;
	enum axp2101_voltage_type voltage_type;
	int min_uv;
	int max_uv;
	int step_uv;
};

static const struct axp2101_output axp2101_outputs[] = {
	{ "dcdc1", AXP2101_PWR_CTRL, BIT(0), AXP2101_DCDC1_V_OUT, 0x1f,
	  AXP2101_VOLTAGE_LINEAR, 1500000, 3400000, 100000 },
	{ "dcdc2", AXP2101_PWR_CTRL, BIT(1), AXP2101_DCDC2_V_OUT, 0x7f,
	  AXP2101_VOLTAGE_DCDC23, 0, 0, 0 },
	{ "dcdc3", AXP2101_PWR_CTRL, BIT(2), AXP2101_DCDC3_V_OUT, 0x7f,
	  AXP2101_VOLTAGE_DCDC23, 0, 0, 0 },
	{ "dcdc4", AXP2101_PWR_CTRL, BIT(3), AXP2101_DCDC4_V_OUT, 0x7f,
	  AXP2101_VOLTAGE_DCDC4, 0, 0, 0 },
	{ "dcdc5", AXP2101_PWR_CTRL, BIT(4), AXP2101_DCDC5_V_OUT, 0x1f,
	  AXP2101_VOLTAGE_LINEAR, 1400000, 3700000, 100000 },
	{ "aldo1", AXP2101_LDO_ONOFF0, BIT(0), AXP2101_ALDO1_V_OUT, 0x1f,
	  AXP2101_VOLTAGE_LINEAR, 500000, 3500000, 100000 },
	{ "aldo2", AXP2101_LDO_ONOFF0, BIT(1), AXP2101_ALDO2_V_OUT, 0x1f,
	  AXP2101_VOLTAGE_LINEAR, 500000, 3500000, 100000 },
	{ "aldo3", AXP2101_LDO_ONOFF0, BIT(2), AXP2101_ALDO3_V_OUT, 0x1f,
	  AXP2101_VOLTAGE_LINEAR, 500000, 3500000, 100000 },
	{ "aldo4", AXP2101_LDO_ONOFF0, BIT(3), AXP2101_ALDO4_V_OUT, 0x1f,
	  AXP2101_VOLTAGE_LINEAR, 500000, 3500000, 100000 },
	{ "bldo1", AXP2101_LDO_ONOFF0, BIT(4), AXP2101_BLDO1_V_OUT, 0x1f,
	  AXP2101_VOLTAGE_LINEAR, 500000, 3500000, 100000 },
	{ "bldo2", AXP2101_LDO_ONOFF0, BIT(5), AXP2101_BLDO2_V_OUT, 0x1f,
	  AXP2101_VOLTAGE_LINEAR, 500000, 3500000, 100000 },
	{ "cpusldo", AXP2101_LDO_ONOFF0, BIT(6), AXP2101_CPUSLDO_V_OUT, 0x1f,
	  AXP2101_VOLTAGE_LINEAR, 500000, 1400000, 50000 },
	{ "dldo1", AXP2101_LDO_ONOFF0, BIT(7), AXP2101_DLDO1_V_OUT, 0x1f,
	  AXP2101_VOLTAGE_LINEAR, 500000, 3300000, 100000 },
	{ "dldo2", AXP2101_LDO_ONOFF1, BIT(0), AXP2101_DLDO2_V_OUT, 0x1f,
	  AXP2101_VOLTAGE_LINEAR, 500000, 1400000, 50000 },
};

static int read_sysfs_int(const char *path, int *out)
{
	FILE *fp;
	int val;

	fp = fopen(path, "r");
	if (!fp)
		return -1;

	if (fscanf(fp, "%d", &val) != 1) {
		fclose(fp);
		return -1;
	}

	fclose(fp);
	*out = val;
	return 0;
}

static int read_sysfs_str(const char *path, char *buf, size_t len)
{
	FILE *fp;

	fp = fopen(path, "r");
	if (!fp)
		return -1;

	if (!fgets(buf, len, fp)) {
		fclose(fp);
		return -1;
	}

	fclose(fp);
	buf[strcspn(buf, "\n")] = '\0';
	return 0;
}

static const char *status_to_str(int status)
{
	switch (status) {
	case 1: return "unknown";
	case 2: return "charging";
	case 3: return "discharging";
	case 4: return "not-charging";
	case 5: return "full";
	default: return "invalid";
	}
}

static int axp2101_i2c_open(void)
{
	int fd;

	fd = open(AXP2101_I2C_DEV, O_RDWR);
	if (fd < 0)
		return -1;

	if (ioctl(fd, I2C_SLAVE, AXP2101_I2C_ADDR) < 0 &&
	    ioctl(fd, I2C_SLAVE_FORCE, AXP2101_I2C_ADDR) < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

static int axp2101_read_reg(int fd, uint8_t reg, uint8_t *val)
{
	if (write(fd, &reg, 1) != 1)
		return -1;
	if (read(fd, val, 1) != 1)
		return -1;

	return 0;
}

static int axp2101_linear_voltage_uv(const struct axp2101_output *out,
				     uint8_t selector)
{
	int uv = out->min_uv + selector * out->step_uv;

	if (uv > out->max_uv)
		return -1;

	return uv;
}

static int axp2101_dcdc23_voltage_uv(uint8_t selector)
{
	if (selector <= 0x46)
		return 500000 + selector * 10000;
	if (selector >= 0x47 && selector <= 0x57)
		return 1220000 + (selector - 0x47) * 20000;

	return -1;
}

static int axp2101_dcdc4_voltage_uv(uint8_t selector)
{
	if (selector <= 0x46)
		return 500000 + selector * 10000;
	if (selector >= 0x47 && selector <= 0x66)
		return 1220000 + (selector - 0x47) * 20000;

	return -1;
}

static int axp2101_output_voltage_uv(const struct axp2101_output *out,
				     uint8_t raw)
{
	uint8_t selector = raw & out->voltage_mask;

	switch (out->voltage_type) {
	case AXP2101_VOLTAGE_LINEAR:
		return axp2101_linear_voltage_uv(out, selector);
	case AXP2101_VOLTAGE_DCDC23:
		return axp2101_dcdc23_voltage_uv(selector);
	case AXP2101_VOLTAGE_DCDC4:
		return axp2101_dcdc4_voltage_uv(selector);
	default:
		return -1;
	}
}

static int axp2101_print_outputs(void)
{
	int fd;
	size_t i;

	fd = axp2101_i2c_open();
	if (fd < 0) {
		printf("pmic_outputs: unavailable (%s)\n", strerror(errno));
		return -1;
	}

	printf("pmic_outputs:\n");
	for (i = 0; i < ARRAY_SIZE(axp2101_outputs); i++) {
		const struct axp2101_output *out = &axp2101_outputs[i];
		uint8_t enable_raw, voltage_raw, selector;
		int uv;

		if (axp2101_read_reg(fd, out->enable_reg, &enable_raw) ||
		    axp2101_read_reg(fd, out->voltage_reg, &voltage_raw)) {
			printf("  %-7s: read-failed (%s)\n",
			       out->name, strerror(errno));
			continue;
		}

		selector = voltage_raw & out->voltage_mask;
		uv = axp2101_output_voltage_uv(out, voltage_raw);
		if (uv < 0) {
			printf("  %-7s: %-8s voltage=unknown raw=0x%02x\n",
			       out->name,
			       (enable_raw & out->enable_mask) ? "enabled" : "disabled",
			       selector);
			continue;
		}

		printf("  %-7s: %-8s voltage=%d uV (%.3f V) raw=0x%02x\n",
		       out->name,
		       (enable_raw & out->enable_mask) ? "enabled" : "disabled",
		       uv, uv / 1000000.0, selector);
	}

	close(fd);
	return 0;
}

static int cmd_status(void)
{
	char buf[64];
	int online, present, voltage, capacity, current, status;
	int temp_ntc, temp_die;

	if (read_sysfs_str(PSY_PATH "/status", buf, sizeof(buf)))
		return fprintf(stderr, "failed to read battery status\n"), 1;
	printf("status: %s\n", buf);

	if (!read_sysfs_int(PSY_PATH "/online", &online))
		printf("external_power: %s\n", online ? "yes" : "no");
	if (!read_sysfs_int(PSY_PATH "/present", &present))
		printf("battery_present: %s\n", present ? "yes" : "no");
	if (!read_sysfs_int(PSY_PATH "/voltage_now", &voltage))
		printf("voltage_now: %d uV (%.3f V)\n", voltage, voltage / 1000000.0);
	if (!read_sysfs_int(PSY_PATH "/capacity", &capacity))
		printf("capacity: %d %%\n", capacity);
	if (!read_sysfs_int(PSY_PATH "/temp", &temp_ntc))
		printf("ntc_temp: %d (%.1f C)\n", temp_ntc, temp_ntc / 10.0);
	if (!read_sysfs_int(PSY_PATH "/temp_ambient", &temp_die))
		printf("die_temp: %d (%.1f C)\n", temp_die, temp_die / 10.0);
	if (!read_sysfs_int(PSY_PATH "/constant_charge_current", &current))
		printf("charge_current: %d uA (%.0f mA)\n", current, current / 1000.0);
	if (!read_sysfs_int(PSY_PATH "/status", &status))
		printf("status_code: %s\n", status_to_str(status));
	axp2101_print_outputs();

	return 0;
}

static int find_input_device(char *path, size_t len)
{
	DIR *dir;
	struct dirent *ent;
	char name_path[256];
	char name[64];

	dir = opendir("/sys/class/input");
	if (!dir)
		return -1;

	while ((ent = readdir(dir))) {
		if (strncmp(ent->d_name, "input", 5) != 0)
			continue;

		snprintf(name_path, sizeof(name_path),
			 "/sys/class/input/%s/name", ent->d_name);
		if (read_sysfs_str(name_path, name, sizeof(name)))
			continue;
		if (strcmp(name, INPUT_BY_NAME) != 0)
			continue;

		snprintf(path, len, "/dev/input/%s", ent->d_name);
		closedir(dir);
		return 0;
	}

	closedir(dir);
	return -1;
}

static int cmd_events(void)
{
	char dev_path[64];
	int fd;
	struct input_event ev;

	if (find_input_device(dev_path, sizeof(dev_path))) {
		fprintf(stderr, "input device '%s' not found\n", INPUT_BY_NAME);
		return 1;
	}

	fd = open(dev_path, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		perror(dev_path);
		return 1;
	}

	printf("listening on %s for KEY_POWER events\n", dev_path);
	for (;;) {
		ssize_t n = read(fd, &ev, sizeof(ev));

		if (n != sizeof(ev)) {
			if (errno == EAGAIN) {
				usleep(10000);
				continue;
			}
			perror("read");
			break;
		}

		if (ev.type == EV_KEY && ev.code == KEY_POWER)
			printf("KEY_POWER %s\n", ev.value ? "press" : "release");
	}

	close(fd);
	return 0;
}

static int cmd_set_charge_current(const char *arg)
{
	char path[128];
	int ua, ma;
	FILE *fp;

	ma = atoi(arg);
	if (ma <= 0 || ma > 1000) {
		fprintf(stderr, "charge current must be between 1 and 1000 mA\n");
		return 1;
	}

	ua = ma * 1000;
	snprintf(path, sizeof(path), PSY_PATH "/constant_charge_current");
	fp = fopen(path, "w");
	if (!fp) {
		perror(path);
		return 1;
	}

	fprintf(fp, "%d", ua);
	fclose(fp);
	printf("charge current set to %d mA\n", ma);
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"usage:\n"
		"  %s status\n"
		"  %s set-charge-current <mA>\n"
		"  %s events\n",
		prog, prog, prog);
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	if (!strcmp(argv[1], "status"))
		return cmd_status();
	if (!strcmp(argv[1], "events"))
		return cmd_events();
	if (!strcmp(argv[1], "set-charge-current") && argc == 3)
		return cmd_set_charge_current(argv[2]);

	usage(argv[0]);
	return 1;
}
