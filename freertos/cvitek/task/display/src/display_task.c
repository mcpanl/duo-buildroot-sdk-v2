/* SPDX-License-Identifier: GPL-2.0 */
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "printf.h"
#include "rtos_cmdqu.h"
#include "comm.h"
#include "arch_helpers.h"
#include "arch_sleep.h"
#include "display_shm.h"
#include "jd9853_panel.h"
#include "display_main.h"

static struct display_shm *g_shm;
static QueueHandle_t xQueueDisplay;
static int g_spi_ready;
static int g_owner_rtos;

/* Progress breadcrumb in reserved_mirror for Linux-side diagnosis */
#define DISP_PROG_WAIT_SHM	1
#define DISP_PROG_WAIT_LINUX	2
#define DISP_PROG_OPEN_SPI	3
#define DISP_PROG_SPI_OK	4
#define DISP_PROG_SPI_FAIL	5
#define DISP_PROG_BL_ONLY	6

static void display_hdr_inv(void)
{
	inv_dcache_range((uintptr_t)g_shm, 64);
}

/* Publish RTOS header writes; prefer clean over flush(cipa) on shared DRAM. */
static void display_hdr_publish(void)
{
	clean_dcache_range((uintptr_t)g_shm, 64);
}

static void display_hdr_flush(void)
{
	display_hdr_publish();
}

static void display_set_prog(uint8_t prog)
{
	if (!g_shm)
		return;
	display_hdr_inv();
	g_shm->reserved_mirror = prog;
	display_hdr_publish();
}

/* Write progress crumb; always inv first so clean cannot clobber Linux dirty. */
static void display_set_prog_raw(uint8_t prog)
{
	if (!g_shm)
		return;
	/*
	 * Header sits in one 64B cache line. Cleaning without a prior inv
	 * publishes RTOS's stale dirty=0 and drops Linux's dirty=1 (false
	 * sharing). Pull the line first, then update only reserved_mirror.
	 */
	display_hdr_inv();
	g_shm->reserved_mirror = prog;
	display_hdr_publish();
}

static void display_apply_bl(unsigned level)
{
	if (level > 100)
		level = 100;

	jd9853_set_backlight_level(level);

	if (!g_shm)
		return;

	display_hdr_inv();
	g_shm->bl_on = level ? 1 : 0;
	g_shm->bl_level = (uint8_t)level;
	display_hdr_publish();
}

static void display_sync_mirror_from_shm(void)
{
	if (!g_shm)
		return;

	inv_dcache_range((uintptr_t)g_shm, 64);
	jd9853_set_mirror(g_shm->mirror_x, g_shm->mirror_y);
}

static void display_inv_frame(const void *src, size_t bytes)
{
	/*
	 * Invalidate in small chunks. A single 110KB dcache.ipa sweep has been
	 * observed to wedge C906 on this board; chunking also lets other tasks
	 * run between blocks.
	 */
	const uint8_t *p = src;
	size_t off = 0;

	while (off < bytes) {
		size_t n = 4096;

		if (n > bytes - off)
			n = bytes - off;
		inv_dcache_range((uintptr_t)(p + off), n);
		off += n;
	}
}

static void display_flush_frame(void)
{
	uint32_t idx;
	const uint16_t *src;

	if (!g_shm || !g_spi_ready)
		return;

	/* Crumb first — no cache op — proves we entered flush even if IPA hangs. */
	display_set_prog_raw(0xA0);

	display_hdr_inv();
	if (!g_shm->dirty) {
		display_set_prog_raw(DISP_PROG_SPI_OK);
		return;
	}

	idx = g_shm->write_idx;
	if (idx >= DISPLAY_BUF_COUNT)
		idx = 0;

	src = (const uint16_t *)g_shm->buf[idx];

	display_set_prog_raw(0xA1);
	display_inv_frame(src, DISPLAY_FRAME_BYTES);

	display_set_prog_raw(0xA2);
	display_sync_mirror_from_shm();

	display_set_prog_raw(0xA3);
	if (jd9853_apply_orientation() != 0) {
		printf("display: orientation/SPI failed, drop frame\n");
		display_hdr_inv();
		g_shm->dirty = 0;
		g_shm->reserved_mirror = 0xE1;
		display_hdr_publish();
		return;
	}

	display_set_prog_raw(0xA4);
	if (jd9853_set_addr_win(0, 0, JD9853_WIDTH - 1, JD9853_HEIGHT - 1) != 0) {
		printf("display: addr win failed\n");
		display_hdr_inv();
		g_shm->dirty = 0;
		g_shm->reserved_mirror = 0xE2;
		display_hdr_publish();
		return;
	}

	display_set_prog_raw(0xA5);
	if (jd9853_write_pixels_be(src, (size_t)JD9853_WIDTH * JD9853_HEIGHT) != 0) {
		printf("display: pixel xfer failed\n");
		display_hdr_inv();
		g_shm->dirty = 0;
		g_shm->reserved_mirror = 0xE3;
		display_hdr_publish();
		return;
	}

	display_hdr_inv();
	g_shm->dirty = 0;
	g_shm->te_sync_cnt++;
	g_shm->frame_seq++;
	g_shm->reserved_mirror = DISP_PROG_SPI_OK;
	display_hdr_publish();
}

static int display_wait_shm(void)
{
	int tries = 0;

	g_shm = (struct display_shm *)(uintptr_t)CVIMMAP_DISPLAY_SHM_ADDR;
	printf("display: wait shm @ %x\n", (unsigned)CVIMMAP_DISPLAY_SHM_ADDR);
	display_set_prog(DISP_PROG_WAIT_SHM);

	/*
	 * Do NOT clear/write magic here. Soft-reboot leftovers are handled by
	 * display_resolve_owner(). Writing magic=0 races with U-Boot's
	 * jd9853_init_display_shm() (D-cache flush can clobber a fresh logo
	 * init) and is why Linux often sees "display_shm magic missing".
	 */
	for (;;) {
		inv_dcache_range((uintptr_t)g_shm, 64);
		if (g_shm->magic == DISPLAY_SHM_MAGIC) {
			if (g_shm->owner == DISPLAY_OWNER_LINUX) {
				printf("display: owner=linux (BL via mailbox)\n");
				g_owner_rtos = 0;
				return 0;
			}
			if (g_shm->owner == DISPLAY_OWNER_RTOS) {
				printf("display: owner=rtos\n");
				g_owner_rtos = 1;
				return 0;
			}
		}
		/*
		 * U-Boot logo may have failed; Linux proxy creates SHM itself.
		 * Wake as soon as linux_ready appears with valid magic.
		 */
		if (g_shm->magic == DISPLAY_SHM_MAGIC && g_shm->linux_ready) {
			printf("display: shm from Linux proxy, owner=%u\n",
			       (unsigned)g_shm->owner);
			g_owner_rtos = (g_shm->owner != DISPLAY_OWNER_LINUX);
			return 0;
		}
		if (++tries % 100 == 0)
			printf("display: waiting U-Boot shm init...\n");
		vTaskDelay(pdMS_TO_TICKS(50));
	}
}

static void display_wait_linux_ready(void);

/*
 * After soft reboot from lcd_owner=linux, SHM may still say owner=LINUX until
 * U-Boot/Linux rewrite it. Proxy sets linux_ready only in rtos mode — use that
 * as a late switch to RTOS SPI ownership.
 */
static void display_resolve_owner(void)
{
	int tries = 0;

	if (g_owner_rtos) {
		display_set_prog(DISP_PROG_WAIT_LINUX);
		display_wait_linux_ready();
		return;
	}

	printf("display: owner=linux path; watch for late rtos handoff...\n");
	for (;;) {
		inv_dcache_range((uintptr_t)g_shm, 64);
		if (g_shm->owner == DISPLAY_OWNER_RTOS) {
			printf("display: late owner=rtos\n");
			g_owner_rtos = 1;
			display_set_prog(DISP_PROG_WAIT_LINUX);
			display_wait_linux_ready();
			return;
		}
		if (g_shm->linux_ready) {
			printf("display: linux_ready with owner=%u -> rtos SPI\n",
			       (unsigned)g_shm->owner);
			g_shm->owner = DISPLAY_OWNER_RTOS;
			display_hdr_publish();
			g_owner_rtos = 1;
			return;
		}
		/* ~30s then stay in BL-only linux mode */
		if (++tries >= 600) {
			display_set_prog(DISP_PROG_BL_ONLY);
			return;
		}
		vTaskDelay(pdMS_TO_TICKS(50));
	}
}

static void display_wait_linux_ready(void)
{
	int tries = 0;

	printf("display: wait linux_ready...\n");
	for (;;) {
		inv_dcache_range((uintptr_t)g_shm, 64);
		if (g_shm->linux_ready) {
			printf("display: linux_ready set\n");
			return;
		}
		if (++tries % 40 == 0)
			printf("display: still waiting linux_ready...\n");
		vTaskDelay(pdMS_TO_TICKS(50));
	}
}

static int display_open_spi(void)
{
	if (g_spi_ready)
		return 0;

	display_set_prog(DISP_PROG_OPEN_SPI);

	/*
	 * Always full-init: after U-Boot, RTOS re-opens DW SSI and skip-init
	 * plus TE-alive heuristics are unreliable (false TE → no pixels).
	 */
	if (jd9853_panel_init(0 /* full init */) != 0) {
		printf("display: panel init failed\n");
		display_set_prog(DISP_PROG_SPI_FAIL);
		return -1;
	}

	display_sync_mirror_from_shm();
	printf("display: mirror_x=%u mirror_y=%u\n",
	       (unsigned)g_shm->mirror_x, (unsigned)g_shm->mirror_y);

	g_spi_ready = 1;
	display_hdr_inv();
	g_shm->rtos_ready = 1;
	g_shm->reserved_mirror = DISP_PROG_SPI_OK;
	display_hdr_flush();

	/*
	 * Full panel re-init clears GRAM. If U-Boot left a logo frame in SHM
	 * (dirty=1), push it immediately so the splash survives the handoff.
	 */
	display_hdr_inv();
	if (g_shm->dirty)
		display_flush_frame();

	/* Panel init must not leave BL stuck at 100 — restore SHM duty. */
	display_hdr_inv();
	display_apply_bl(g_shm->bl_on ? (g_shm->bl_level ? g_shm->bl_level
							 : 100)
				      : 0);

	printf("display: spi ready\n");
	return 0;
}

void prvDisplayRunTask(void *pvParameters)
{
	cmdqu_t rtos_cmdq;
	int spi_tries = 0;

	(void)pvParameters;

	xQueueDisplay = main_GetMODHandle(E_QUEUE_DISPLAY);
	printf("display: task start\n");

	if (display_wait_shm() != 0) {
		for (;;)
			vTaskDelay(pdMS_TO_TICKS(1000));
	}

	/*
	 * Own GPIOA20 soft-PWM for both lcd_owner=rtos and linux.
	 * Inherit U-Boot bl_level so the splash backlight stays on through
	 * the Linux probe gap (previously forced to 0 here).
	 */
	{
		unsigned bl = 100;

		display_hdr_inv();
		if (!g_shm->bl_on)
			bl = 0;
		else if (g_shm->bl_level)
			bl = g_shm->bl_level;
		jd9853_bl_pwm_init(bl);
		printf("display: bl_pwm init level=%u\n", bl);
	}

	display_resolve_owner();
	if (g_owner_rtos) {
		/* Retry SPI bring-up instead of parking forever. */
		while (display_open_spi() != 0) {
			if (++spi_tries % 5 == 0)
				printf("display: retry panel init...\n");
			vTaskDelay(pdMS_TO_TICKS(1000));
		}
	}

	for (;;) {
		/*
		 * Heartbeat with clean-only publish. Never block on the mailbox
		 * queue or vTaskDelay: after a long SPI busy-poll the tick can
		 * appear stuck and parking here freezes refresh forever.
		 */
		if (g_owner_rtos && g_shm) {
			uint8_t hb = g_shm->reserved_mirror;

			hb = (uint8_t)(DISP_PROG_SPI_OK |
				       ((((hb >> 4) + 1) & 0xf) << 4));
			display_set_prog_raw(hb);
		}

		while (xQueueDisplay &&
		       xQueueReceive(xQueueDisplay, &rtos_cmdq, 0) == pdTRUE) {
			if (rtos_cmdq.ip_id == IP_DISPLAY) {
				switch (rtos_cmdq.cmd_id) {
				case DISPLAY_CMD_FLUSH:
					if (g_owner_rtos)
						display_flush_frame();
					break;
				case DISPLAY_CMD_BL:
					display_apply_bl(rtos_cmdq.param_ptr);
					break;
				case DISPLAY_CMD_MIRROR:
					if (g_owner_rtos) {
						display_sync_mirror_from_shm();
						jd9853_apply_orientation();
						if (g_shm->dirty)
							display_flush_frame();
					}
					break;
				default:
					break;
				}
			}
		}

		if (g_owner_rtos && g_shm) {
			display_hdr_inv();
			if (g_shm->dirty)
				display_flush_frame();
			/*
			 * Apply BL only when SHM duty changes. Re-driving the
			 * pin every loop RMW-fights Linux gpio-leds on GPIOA
			 * (sys-led A29) and makes BL flash with activity.
			 */
			if (g_shm->bl_level != jd9853_get_backlight_level())
				display_apply_bl(g_shm->bl_level);
		} else if (g_shm) {
			display_hdr_inv();
			if (g_shm->bl_level != jd9853_get_backlight_level())
				display_apply_bl(g_shm->bl_level);
		}

		/*
		 * Prefer a real tick delay so CMDQU can run. Fall back to a
		 * short busy pace + yield if the tick looks unhealthy.
		 */
		{
			TickType_t t0 = xTaskGetTickCount();

			vTaskDelay(pdMS_TO_TICKS(2));
			if (xTaskGetTickCount() == t0) {
				arch_usleep(2000);
				taskYIELD();
			}
		}
	}
}
