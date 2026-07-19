/* SPDX-License-Identifier: GPL-2.0 */
/*
 * FreeRTOS small-core performance statistics collector.
 * Samples CPU / heap / per-task usage and display FPS into a shared
 * ring buffer at display_shm + RTOS_STATS_SHM_OFFSET for Linux to read.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "printf.h"
#include "arch_helpers.h"
#include "display_shm.h"
#include "rtos_stats_shm.h"
#include "rtos_stats.h"

#define STATS_TASK_STACK		(configMINIMAL_STACK_SIZE * 4)
#define STATS_TASK_PRIO			(tskIDLE_PRIORITY + 1)
#define STATS_MAX_SYSTEM_TASKS		24

static struct rtos_stats_shm *g_stats;
static struct display_shm *g_disp;

static TaskStatus_t s_prev_status[STATS_MAX_SYSTEM_TASKS];
static UBaseType_t s_prev_count;
static uint32_t s_prev_total_runtime;
static uint8_t s_have_prev;

static uint32_t s_prev_te;
static uint32_t s_prev_ms;
static uint8_t s_have_fps_prev;

static uint32_t stats_now_ms(void)
{
	return (uint32_t)(xTaskGetTickCount() * (1000u / configTICK_RATE_HZ));
}

static void stats_shm_init(void)
{
	g_disp = (struct display_shm *)(uintptr_t)CVIMMAP_DISPLAY_SHM_ADDR;
	g_stats = (struct rtos_stats_shm *)(uintptr_t)RTOS_STATS_SHM_ADDR;

	memset(g_stats, 0, sizeof(*g_stats));
	g_stats->magic = RTOS_STATS_SHM_MAGIC;
	g_stats->version = RTOS_STATS_SHM_VERSION;
	g_stats->sample_interval_ms = RTOS_STATS_SAMPLE_INTERVAL_MS;
	g_stats->ring_capacity = RTOS_STATS_RING_CAP;
	g_stats->write_idx = 0;
	g_stats->seq = 0;
	g_stats->total_samples = 0;
	flush_dcache_range((uintptr_t)g_stats, sizeof(*g_stats));

	printf("rtos_stats: shm @ %x size=%u\n",
	       (unsigned)RTOS_STATS_SHM_ADDR, (unsigned)sizeof(*g_stats));
}

static uint16_t stats_calc_display_fps(uint32_t now_ms, uint32_t te_cnt,
				       uint8_t display_ready)
{
	uint32_t delta_ms;
	uint32_t delta_te;
	uint16_t fps_x100 = 0;

	if (!display_ready) {
		s_have_fps_prev = 0;
		return 0;
	}

	if (!s_have_fps_prev) {
		s_prev_te = te_cnt;
		s_prev_ms = now_ms;
		s_have_fps_prev = 1;
		return 0;
	}

	delta_ms = now_ms - s_prev_ms;
	delta_te = te_cnt - s_prev_te;
	s_prev_te = te_cnt;
	s_prev_ms = now_ms;

	if (delta_ms == 0)
		return 0;

	/* FPS * 100 = delta_te * 100000 / delta_ms */
	fps_x100 = (uint16_t)((delta_te * 100000ULL) / delta_ms);
	return fps_x100;
}

static uint32_t stats_find_prev_runtime(const char *name)
{
	UBaseType_t i;

	for (i = 0; i < s_prev_count; i++) {
		if (strncmp(s_prev_status[i].pcTaskName, name,
			    configMAX_TASK_NAME_LEN) == 0)
			return s_prev_status[i].ulRunTimeCounter;
	}
	return 0;
}

static void stats_fill_tasks(struct rtos_stats_sample *sample,
			     TaskStatus_t *status, UBaseType_t count,
			     uint32_t total_delta, uint16_t *cpu_usage_pct)
{
	UBaseType_t i;
	uint8_t n = 0;
	uint32_t idle_delta = 0;

	*cpu_usage_pct = 0;

	for (i = 0; i < count && n < RTOS_STATS_MAX_TASKS; i++) {
		uint32_t prev_rt = stats_find_prev_runtime(status[i].pcTaskName);
		uint32_t delta = status[i].ulRunTimeCounter - prev_rt;
		uint8_t pct = 0;

		if (total_delta > 0)
			pct = (uint8_t)((delta * 100ULL) / total_delta);

		memset(sample->tasks[n].name, 0, RTOS_STATS_TASK_NAME_LEN);
		strncpy(sample->tasks[n].name, status[i].pcTaskName,
			RTOS_STATS_TASK_NAME_LEN - 1);
		sample->tasks[n].cpu_pct = pct;
		sample->tasks[n].state = (uint8_t)status[i].eCurrentState;
		sample->tasks[n].stack_hwm_words =
			(uint16_t)status[i].usStackHighWaterMark;
		n++;

		if (strncmp(status[i].pcTaskName, "IDLE", 4) == 0)
			idle_delta = delta;
	}

	sample->task_count = n;

	if (total_delta > 0) {
		uint32_t idle_pct = (uint32_t)((idle_delta * 100ULL) / total_delta);

		if (idle_pct > 100)
			idle_pct = 100;
		*cpu_usage_pct = (uint16_t)(100 - idle_pct);
	} else {
		*cpu_usage_pct = 0;
	}
}

static void stats_take_sample(void)
{
	TaskStatus_t status[STATS_MAX_SYSTEM_TASKS];
	UBaseType_t count;
	uint32_t total_runtime = 0;
	uint32_t total_delta;
	uint32_t now_ms;
	uint32_t te_cnt = 0;
	uint32_t frame_seq = 0;
	uint8_t display_ready = 0;
	struct rtos_stats_sample *slot;
	uint32_t idx;
	uint16_t cpu_pct = 0;

	count = uxTaskGetSystemState(status, STATS_MAX_SYSTEM_TASKS,
				     &total_runtime);
	if (count == 0)
		return;

	now_ms = stats_now_ms();

	inv_dcache_range((uintptr_t)g_disp, 64);
	if (g_disp->magic == DISPLAY_SHM_MAGIC) {
		te_cnt = g_disp->te_sync_cnt;
		frame_seq = g_disp->frame_seq;
		display_ready = (g_disp->owner == DISPLAY_OWNER_RTOS &&
				 g_disp->rtos_ready) ? 1 : 0;
	}

	if (!s_have_prev) {
		memcpy(s_prev_status, status, count * sizeof(TaskStatus_t));
		s_prev_count = count;
		s_prev_total_runtime = total_runtime;
		s_have_prev = 1;
		/* Still record FPS baseline */
		(void)stats_calc_display_fps(now_ms, te_cnt, display_ready);
		return;
	}

	total_delta = total_runtime - s_prev_total_runtime;
	idx = g_stats->write_idx % RTOS_STATS_RING_CAP;
	slot = &g_stats->samples[idx];

	/* seqlock: odd while writing */
	g_stats->seq++;
	flush_dcache_range((uintptr_t)&g_stats->seq, sizeof(g_stats->seq));

	memset(slot, 0, sizeof(*slot));
	slot->timestamp_ms = now_ms;
	slot->heap_total_kb = (uint16_t)(configTOTAL_HEAP_SIZE / 1024);
	slot->heap_free_kb = (uint16_t)(xPortGetFreeHeapSize() / 1024);
	slot->heap_min_free_kb =
		(uint16_t)(xPortGetMinimumEverFreeHeapSize() / 1024);
	slot->te_sync_cnt = te_cnt;
	slot->frame_seq = frame_seq;
	slot->display_ready = display_ready;
	slot->display_fps_x100 =
		stats_calc_display_fps(now_ms, te_cnt, display_ready);

	stats_fill_tasks(slot, status, count, total_delta, &cpu_pct);
	slot->cpu_usage_pct = cpu_pct;

	flush_dcache_range((uintptr_t)slot, sizeof(*slot));

	g_stats->write_idx = (idx + 1) % RTOS_STATS_RING_CAP;
	g_stats->total_samples++;
	g_stats->seq++; /* even = stable */
	flush_dcache_range((uintptr_t)g_stats,
			   offsetof(struct rtos_stats_shm, samples));

	memcpy(s_prev_status, status, count * sizeof(TaskStatus_t));
	s_prev_count = count;
	s_prev_total_runtime = total_runtime;
}

static void prvStatsRunTask(void *pvParameters)
{
	(void)pvParameters;

	stats_shm_init();
	printf("rtos_stats: task started, interval=%ums\n",
	       (unsigned)RTOS_STATS_SAMPLE_INTERVAL_MS);

	for (;;) {
		stats_take_sample();
		vTaskDelay(pdMS_TO_TICKS(RTOS_STATS_SAMPLE_INTERVAL_MS));
	}
}

void rtos_stats_start(void)
{
	BaseType_t ret;

	ret = xTaskCreate(prvStatsRunTask, "STATS", STATS_TASK_STACK, NULL,
			  STATS_TASK_PRIO, NULL);
	if (ret != pdPASS)
		printf("rtos_stats: xTaskCreate failed\n");
}
