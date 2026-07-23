/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Shared RGB565 framebuffer protocol between Linux (CA53) and FreeRTOS (C906L).
 * Keep this header identical on both sides.
 */
#ifndef __DISPLAY_SHM_H__
#define __DISPLAY_SHM_H__

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

#define DISPLAY_SHM_MAGIC		0x5A4C4344u	/* 'ZLCD' */
#define DISPLAY_SHM_VERSION		3

/* Default panel orientation (matches ESP-IDF / Linux fb_jd9853 bring-up) */
#define LCD_MIRROR_X_DEFAULT		1
#define LCD_MIRROR_Y_DEFAULT		0

#define DISPLAY_W			172
#define DISPLAY_H			320
#define DISPLAY_BPP			16
#define DISPLAY_FRAME_BYTES		(DISPLAY_W * DISPLAY_H * 2) /* 110080 */
#define DISPLAY_BUF_COUNT		3

#define DISPLAY_OWNER_RTOS		0
#define DISPLAY_OWNER_LINUX		1

#ifndef CVIMMAP_DISPLAY_SHM_ADDR
#define CVIMMAP_DISPLAY_SHM_ADDR	0x95300000u
#endif
#ifndef CVIMMAP_DISPLAY_SHM_SIZE
#define CVIMMAP_DISPLAY_SHM_SIZE	0x100000u
#endif

enum display_cmd_id {
	DISPLAY_CMD_FLUSH = 0,
	DISPLAY_CMD_BL,		/* param_ptr = brightness 0..100 */
	DISPLAY_CMD_MIRROR,
	DISPLAY_CMD_LIMIT,
};

struct display_shm {
	uint32_t magic;
	uint32_t version;
	uint8_t  owner;		/* DISPLAY_OWNER_* */
	uint8_t  bl_on;		/* backlight on hint (bl_level != 0) */
	uint8_t  linux_ready;	/* set by proxy after Linux probe */
	uint8_t  rtos_ready;	/* set by RTOS after SPI init */
	uint32_t frame_seq;
	uint32_t write_idx;	/* buffer index ready for RTOS SPI blit */
	uint32_t dirty;		/* Linux sets 1, RTOS clears after blit */
	uint32_t te_sync_cnt;
	uint8_t  mirror_x;	/* 0/1: horizontal mirror (MADCTL MX) */
	uint8_t  mirror_y;	/* 0/1: vertical mirror (MADCTL MY) */
	uint8_t  bl_level;	/* 0..100 backlight duty (RTOS soft-PWM) */
	uint8_t  reserved_mirror;
	uint8_t  buf[DISPLAY_BUF_COUNT][DISPLAY_FRAME_BYTES];
} __attribute__((packed, aligned(64)));

#endif /* __DISPLAY_SHM_H__ */
