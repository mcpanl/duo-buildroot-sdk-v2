/*
 * axp2101ctl - user space helper for AXP2101 power supply and input events
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PSY_PATH "/sys/class/power_supply/axp2101-battery"
#define INPUT_BY_NAME "axp2101-pek"

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

static int cmd_status(void)
{
	char buf[64];
	int online, present, voltage, capacity, current, status;

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
	if (!read_sysfs_int(PSY_PATH "/constant_charge_current", &current))
		printf("charge_current: %d uA (%.0f mA)\n", current, current / 1000.0);
	if (!read_sysfs_int(PSY_PATH "/status", &status))
		printf("status_code: %s\n", status_to_str(status));

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
