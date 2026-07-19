#include "sys_status.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BATTERY_PSY_PATH "/sys/class/power_supply/axp2101-battery"
#define THERMAL_ZONE0_PATH "/sys/class/thermal/thermal_zone0/temp"

#define TEMP_POLL_MS	500
#define BATTERY_POLL_MS	2000

static pthread_t g_status_tid;
static pthread_mutex_t g_status_lock = PTHREAD_MUTEX_INITIALIZER;
static SYS_STATUS_S g_status_cached;
static volatile int g_status_running;

static int read_sysfs_int(const char *path, int *out)
{
	FILE *fp;
	int val;

	if (!path || !out)
		return -1;

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

static void read_battery_locked(SYS_STATUS_S *st)
{
	int present = 0;
	int cap = 0;

	if (!st)
		return;

	if (read_sysfs_int(BATTERY_PSY_PATH "/present", &present) != 0 ||
	    !present) {
		st->battery_valid = false;
		return;
	}

	if (read_sysfs_int(BATTERY_PSY_PATH "/capacity", &cap) != 0) {
		st->battery_valid = false;
		return;
	}

	if (cap > 100)
		cap = 100;
	if (cap < 0)
		cap = 0;

	st->battery_valid = true;
	st->battery_pct = cap;
}

static void read_temp_locked(SYS_STATUS_S *st)
{
	int mc;

	if (!st)
		return;

	if (read_sysfs_int(THERMAL_ZONE0_PATH, &mc) != 0) {
		st->temp_valid = false;
		return;
	}

	st->temp_valid = true;
	st->temp_c = (mc + 500) / 1000;
}

static void *sys_status_thread(void *arg)
{
	unsigned battery_elapsed_ms = BATTERY_POLL_MS;
	SYS_STATUS_S local;

	(void)arg;

	while (g_status_running) {
		memset(&local, 0, sizeof(local));

		read_temp_locked(&local);

		if (battery_elapsed_ms >= BATTERY_POLL_MS) {
			read_battery_locked(&local);
			battery_elapsed_ms = 0;
		} else {
			pthread_mutex_lock(&g_status_lock);
			local.battery_valid = g_status_cached.battery_valid;
			local.battery_pct = g_status_cached.battery_pct;
			pthread_mutex_unlock(&g_status_lock);
		}

		pthread_mutex_lock(&g_status_lock);
		g_status_cached = local;
		pthread_mutex_unlock(&g_status_lock);

		usleep(TEMP_POLL_MS * 1000);
		battery_elapsed_ms += TEMP_POLL_MS;
	}

	return NULL;
}

int sys_status_start(void)
{
	memset(&g_status_cached, 0, sizeof(g_status_cached));
	g_status_running = 1;

	if (pthread_create(&g_status_tid, NULL, sys_status_thread, NULL) != 0)
		return -1;

	return 0;
}

void sys_status_stop(void)
{
	if (!g_status_running)
		return;

	g_status_running = 0;
	pthread_join(g_status_tid, NULL);
}

void sys_status_get(SYS_STATUS_S *st)
{
	if (!st)
		return;

	pthread_mutex_lock(&g_status_lock);
	*st = g_status_cached;
	pthread_mutex_unlock(&g_status_lock);
}
