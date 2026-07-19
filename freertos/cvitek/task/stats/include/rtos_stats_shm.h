/* SPDX-License-Identifier: GPL-2.0 */
/*
 * RTOS performance stats shared between FreeRTOS (C906L) and Linux (CA53).
 * Lives in display_shm 1MB carveout at offset RTOS_STATS_SHM_OFFSET.
 * Keep this header identical on both sides (see osdrv/include/rtos_stats_shm.h).
 *
 * Structures are naturally aligned (no packed) so C906L can safely access
 * 32/64-bit fields without unaligned traps.
 */
#ifndef __RTOS_STATS_SHM_H__
#define __RTOS_STATS_SHM_H__

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#define RTOS_STATS_SHM_MAGIC		0x5A535441u	/* 'ZSTA' */
#define RTOS_STATS_SHM_VERSION		1
#define RTOS_STATS_SHM_OFFSET		0x90000u	/* within display_shm 1MB */
#define RTOS_STATS_RING_CAP		150		/* 15s @ 100ms */
#define RTOS_STATS_MAX_TASKS		16
#define RTOS_STATS_SAMPLE_INTERVAL_MS	100
#define RTOS_STATS_TASK_NAME_LEN	16

#ifndef CVIMMAP_DISPLAY_SHM_ADDR
#define CVIMMAP_DISPLAY_SHM_ADDR	0x95300000u
#endif

#define RTOS_STATS_SHM_ADDR \
	((CVIMMAP_DISPLAY_SHM_ADDR) + (RTOS_STATS_SHM_OFFSET))

struct rtos_stats_task {
	char     name[RTOS_STATS_TASK_NAME_LEN];
	uint8_t  cpu_pct;
	uint8_t  state;		/* eTaskState */
	uint16_t stack_hwm_words;
};

struct rtos_stats_sample {
	uint32_t timestamp_ms;
	uint16_t cpu_usage_pct;		/* 100 - idle% */
	uint16_t heap_total_kb;
	uint16_t heap_free_kb;
	uint16_t heap_min_free_kb;
	uint16_t display_fps_x100;	/* FPS * 100, e.g. 2997 => 29.97 */
	uint8_t  task_count;
	uint8_t  display_ready;		/* rtos_ready && owner==RTOS */
	uint16_t reserved0;
	uint32_t te_sync_cnt;
	uint32_t frame_seq;
	struct rtos_stats_task tasks[RTOS_STATS_MAX_TASKS];
};

struct rtos_stats_shm {
	uint32_t magic;
	uint32_t version;
	uint32_t sample_interval_ms;
	uint32_t ring_capacity;
	uint32_t write_idx;		/* next slot to write (mod capacity) */
	uint32_t seq;			/* seqlock: odd=writing, even=stable */
	uint64_t total_samples;
	struct rtos_stats_sample samples[RTOS_STATS_RING_CAP];
} __attribute__((aligned(64)));

#endif /* __RTOS_STATS_SHM_H__ */
