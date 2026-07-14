#ifndef PERF_STATS_H
#define PERF_STATS_H

#include <stdint.h>
#include <time.h>

/* Temporary profiling: report rolling averages every N seconds. */
#define PERF_REPORT_INTERVAL_SEC 3

typedef enum {
	PERF_VPSS_GET_FRAME,
	PERF_VPSS_MAP,
	PERF_RGB888_RGB565,
	PERF_FB_CLEAR,
	PERF_FB_BLIT,
	PERF_VPSS_UNMAP,
	PERF_VPSS_RELEASE,
	PERF_FRAME_TOTAL,
	PERF_SLOT_COUNT
} perf_slot_e;

void perf_record(perf_slot_e slot, uint64_t ns);
uint64_t perf_elapsed_ns(const struct timespec *t0, const struct timespec *t1);
void perf_timespec_now(struct timespec *ts);
int perf_report_due(struct timespec *last, int interval_sec);
void perf_print_report(void);
void perf_reset_window(void);

#endif /* PERF_STATS_H */
