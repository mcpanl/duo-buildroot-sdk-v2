#include "perf_stats.h"

#include <stdio.h>

static const char *perf_names[PERF_SLOT_COUNT] = {
	"vpss_get_frame",
	"vpss_map",
	"rgb888_rgb565",
	"fb_clear",
	"fb_blit",
	"vpss_unmap",
	"vpss_release",
	"frame_total",
};

static uint64_t perf_total_ns[PERF_SLOT_COUNT];
static uint64_t perf_count[PERF_SLOT_COUNT];

void perf_record(perf_slot_e slot, uint64_t ns)
{
	if (slot < PERF_SLOT_COUNT) {
		perf_total_ns[slot] += ns;
		perf_count[slot]++;
	}
}

uint64_t perf_elapsed_ns(const struct timespec *t0, const struct timespec *t1)
{
	return (uint64_t)(t1->tv_sec - t0->tv_sec) * 1000000000ULL
	     + (uint64_t)(t1->tv_nsec - t0->tv_nsec);
}

void perf_timespec_now(struct timespec *ts)
{
	clock_gettime(CLOCK_MONOTONIC, ts);
}

int perf_report_due(struct timespec *last, int interval_sec)
{
	struct timespec now;

	perf_timespec_now(&now);
	if (perf_elapsed_ns(last, &now) >= (uint64_t)interval_sec * 1000000000ULL) {
		*last = now;
		return 1;
	}
	return 0;
}

void perf_print_report(void)
{
	int i;
	double interval_sec = PERF_REPORT_INTERVAL_SEC;
	uint64_t frame_cnt = perf_count[PERF_FRAME_TOTAL];

	printf("[perf] last %.0fs window averages:\n", interval_sec);
	for (i = 0; i < PERF_SLOT_COUNT; i++) {
		if (perf_count[i] == 0)
			continue;
		printf("  %-16s %8.3f ms  (n=%llu)\n",
		       perf_names[i],
		       (double)perf_total_ns[i] / perf_count[i] / 1e6,
		       (unsigned long long)perf_count[i]);
	}
	if (frame_cnt > 0)
		printf("  implied_fps      %8.1f\n", (double)frame_cnt / interval_sec);
}

void perf_reset_window(void)
{
	int i;

	for (i = 0; i < PERF_SLOT_COUNT; i++) {
		perf_total_ns[i] = 0;
		perf_count[i] = 0;
	}
}
