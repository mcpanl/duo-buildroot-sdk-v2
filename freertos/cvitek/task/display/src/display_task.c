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
#include "display_shm.h"
#include "jd9853_panel.h"
#include "display_main.h"

static struct display_shm *g_shm;
static QueueHandle_t xQueueDisplay;
static int g_spi_ready;

static void display_sync_mirror_from_shm(void)
{
	if (!g_shm)
		return;

	inv_dcache_range((uintptr_t)g_shm, 64);
	jd9853_set_mirror(g_shm->mirror_x, g_shm->mirror_y);
}

static void display_flush_frame(void)
{
	uint32_t idx;
	const uint16_t *src;

	if (!g_shm || !g_spi_ready)
		return;

	/* Always invalidate header before reading dirty/write_idx */
	inv_dcache_range((uintptr_t)g_shm, 64);
	if (!g_shm->dirty)
		return;

	idx = g_shm->write_idx;
	if (idx >= DISPLAY_BUF_COUNT)
		idx = 0;

	src = (const uint16_t *)g_shm->buf[idx];
	inv_dcache_range((uintptr_t)src, DISPLAY_FRAME_BYTES);

	display_sync_mirror_from_shm();

	jd9853_wait_te();
	if (jd9853_apply_orientation() != 0)
		return;
	jd9853_set_addr_win(0, 0, JD9853_WIDTH - 1, JD9853_HEIGHT - 1);
	jd9853_write_pixels_be(src, (size_t)JD9853_WIDTH * JD9853_HEIGHT);

	g_shm->dirty = 0;
	g_shm->te_sync_cnt++;
	g_shm->frame_seq++;
	flush_dcache_range((uintptr_t)g_shm, 64);
}

static int display_wait_owner_rtos(void)
{
	int tries = 0;

	g_shm = (struct display_shm *)(uintptr_t)CVIMMAP_DISPLAY_SHM_ADDR;
	printf("display: wait shm @ %x\n", (unsigned)CVIMMAP_DISPLAY_SHM_ADDR);

	for (;;) {
		inv_dcache_range((uintptr_t)g_shm, 64);
		if (g_shm->magic == DISPLAY_SHM_MAGIC) {
			if (g_shm->owner == DISPLAY_OWNER_LINUX) {
				printf("display: owner=linux, idle\n");
				return -1;
			}
			if (g_shm->owner == DISPLAY_OWNER_RTOS) {
				printf("display: owner=rtos\n");
				return 0;
			}
		}
		if (++tries % 100 == 0)
			printf("display: waiting U-Boot shm init...\n");
		vTaskDelay(pdMS_TO_TICKS(50));
	}
}

/*
 * Delay SPI bring-up until Linux proxy is ready. That guarantees Linux has
 * already skipped SPI3 probe (cvi.lcd_owner=rtos), so DW SSI reset cannot
 * clobber the panel link after we open it.
 */
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

	if (jd9853_panel_init(1 /* skip_init */) != 0) {
		printf("display: panel init failed\n");
		return -1;
	}

	display_sync_mirror_from_shm();
	printf("display: mirror_x=%u mirror_y=%u\n",
	       (unsigned)g_shm->mirror_x, (unsigned)g_shm->mirror_y);

	g_spi_ready = 1;
	g_shm->rtos_ready = 1;
	flush_dcache_range((uintptr_t)g_shm, 64);

	/* Proof of life once Linux+RTOS handshake is complete */
	jd9853_wait_te();
	jd9853_fill_color(0x07E0);
	printf("display: self-test green OK (te=%u)\n",
	       (unsigned)g_shm->te_sync_cnt);
	return 0;
}

void prvDisplayRunTask(void *pvParameters)
{
	cmdqu_t rtos_cmdq;

	(void)pvParameters;

	xQueueDisplay = main_GetMODHandle(E_QUEUE_DISPLAY);
	printf("display: task start\n");

	if (display_wait_owner_rtos() != 0) {
		for (;;)
			vTaskDelay(pdMS_TO_TICKS(1000));
	}

	display_wait_linux_ready();
	if (display_open_spi() != 0) {
		for (;;)
			vTaskDelay(pdMS_TO_TICKS(1000));
	}

	for (;;) {
		if (xQueueReceive(xQueueDisplay, &rtos_cmdq,
				  pdMS_TO_TICKS(16)) == pdTRUE) {
			if (rtos_cmdq.ip_id == IP_DISPLAY) {
				switch (rtos_cmdq.cmd_id) {
				case DISPLAY_CMD_FLUSH:
					display_flush_frame();
					break;
				case DISPLAY_CMD_BL:
					jd9853_set_backlight(rtos_cmdq.param_ptr ? 1 : 0);
					break;
				case DISPLAY_CMD_MIRROR:
					display_sync_mirror_from_shm();
					jd9853_apply_orientation();
					if (g_shm->dirty)
						display_flush_frame();
					break;
				default:
					break;
				}
			}
		}

		inv_dcache_range((uintptr_t)g_shm, 64);
		if (g_shm->dirty)
			display_flush_frame();
	}
}
