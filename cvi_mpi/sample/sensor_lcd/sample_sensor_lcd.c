#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <time.h>

#include "cvi_buffer.h"
#include "cvi_comm_isp.h"
#include "cvi_comm_sns.h"
#include "cvi_vpss.h"
#include "sample_comm.h"
#include "fb_lcd.h"
#include "nv21_rgb565.h"

#define VPSS_ALIGN 64
#define VPSS_ALIGN_UP(x) ((((x) + VPSS_ALIGN - 1) / VPSS_ALIGN) * VPSS_ALIGN)

/*
 * VPSS channel size before ROTATION_90 (both must be 64-aligned for GDC).
 * 320x172 fails (172 % 64 != 0); use 320x192 -> effective 192x320 after rotate.
 */
#define VPSS_OUT_W 320
#define VPSS_OUT_H VPSS_ALIGN_UP(172)

/* Max RGB565 scratch (VPSS_OUT_W * VPSS_OUT_H). */
#define VPSS_RGB_PIXELS (VPSS_OUT_W * VPSS_OUT_H)

static SAMPLE_VI_CONFIG_S g_stViConfig;
static SIZE_S g_stSensorSize;
static volatile sig_atomic_t g_running = 1;
static CVI_BOOL g_vpss_started = CVI_FALSE;
static CVI_BOOL g_vpss_bound = CVI_FALSE;
static CVI_BOOL g_vpss_chn_enabled[VPSS_MAX_PHY_CHN_NUM] = {0};
static CVI_BOOL g_frame_held = CVI_FALSE;
static VIDEO_FRAME_INFO_S g_held_frame;

static void sys_handle_signal(int sig)
{
	(void)sig;
	g_running = 0;
}

static CVI_U32 vb_pool_blk_size(CVI_U32 w, CVI_U32 h, PIXEL_FORMAT_E fmt)
{
	return COMMON_GetPicBufferSize(w, h, fmt, DATA_BITWIDTH_8,
				     COMPRESS_MODE_NONE, DEFAULT_ALIGN);
}

static CVI_S32 sys_mm_init(CVI_BOOL mirror, CVI_BOOL flip)
{
	MMF_VERSION_S stVersion;
	SAMPLE_INI_CFG_S stIniCfg;
	SAMPLE_VI_CONFIG_S stViConfig;
	PIC_SIZE_E enPicSize;
	CVI_S32 s32Ret;
	LOG_LEVEL_CONF_S log_conf;
	VB_CONFIG_S stVbConf;
	CVI_U32 u32ViBlk, u32ViRotBlk, u32VpssBlk, u32VpssRotBlk;
	VPSS_GRP VpssGrp = 0;
	VPSS_CHN VpssChn = VPSS_CHN0;
	VPSS_GRP_ATTR_S stVpssGrpAttr;
	VPSS_CHN_ATTR_S astVpssChnAttr[VPSS_MAX_PHY_CHN_NUM];
	CVI_BOOL abChnEnable[VPSS_MAX_PHY_CHN_NUM] = {0};
	struct sigaction sa;

	CVI_SYS_GetVersion(&stVersion);
	SAMPLE_PRT("MMF Version:%s\n", stVersion.version);

	log_conf.enModId = CVI_ID_LOG;
	log_conf.s32Level = CVI_DBG_INFO;
	CVI_LOG_SetLevelConf(&log_conf);

	s32Ret = SAMPLE_COMM_VI_ParseIni(&stIniCfg);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("Parse ini fail\n");
		return s32Ret;
	}
	SAMPLE_PRT("Parse complete\n");

	CVI_VI_SetDevNum(stIniCfg.devNum);

	s32Ret = SAMPLE_COMM_VI_IniToViCfg(&stIniCfg, &stViConfig);
	if (s32Ret != CVI_SUCCESS)
		return s32Ret;

	memcpy(&g_stViConfig, &stViConfig, sizeof(SAMPLE_VI_CONFIG_S));

	s32Ret = SAMPLE_COMM_VI_GetSizeBySensor(stIniCfg.enSnsType[0], &enPicSize);
	if (s32Ret != CVI_SUCCESS)
		return s32Ret;

	s32Ret = SAMPLE_COMM_SYS_GetPicSize(enPicSize, &g_stSensorSize);
	if (s32Ret != CVI_SUCCESS)
		return s32Ret;

	memset(&stVbConf, 0, sizeof(stVbConf));
	u32ViBlk = vb_pool_blk_size(g_stSensorSize.u32Width, g_stSensorSize.u32Height,
				    stViConfig.astViInfo[0].stChnInfo.enPixFormat);
	u32ViRotBlk = vb_pool_blk_size(g_stSensorSize.u32Height, g_stSensorSize.u32Width,
				       stViConfig.astViInfo[0].stChnInfo.enPixFormat);
	u32ViBlk = u32ViBlk > u32ViRotBlk ? u32ViBlk : u32ViRotBlk;

	u32VpssBlk = vb_pool_blk_size(VPSS_OUT_W, VPSS_OUT_H, SAMPLE_PIXEL_FORMAT);
	u32VpssRotBlk = vb_pool_blk_size(VPSS_OUT_H, VPSS_OUT_W, SAMPLE_PIXEL_FORMAT);
	u32VpssBlk = u32VpssBlk > u32VpssRotBlk ? u32VpssBlk : u32VpssRotBlk;

	stVbConf.u32MaxPoolCnt = 2;
	stVbConf.astCommPool[0].u32BlkSize = u32ViBlk;
	stVbConf.astCommPool[0].u32BlkCnt = 5;
	stVbConf.astCommPool[0].enRemapMode = VB_REMAP_MODE_CACHED;
	stVbConf.astCommPool[1].u32BlkSize = u32VpssBlk;
	stVbConf.astCommPool[1].u32BlkCnt = 4;
	stVbConf.astCommPool[1].enRemapMode = VB_REMAP_MODE_CACHED;

	SAMPLE_PRT("VB pool[0] sensor %ux%u size=%u cnt=%u\n",
		   g_stSensorSize.u32Width, g_stSensorSize.u32Height,
		   stVbConf.astCommPool[0].u32BlkSize,
		   stVbConf.astCommPool[0].u32BlkCnt);
	SAMPLE_PRT("VB pool[1] vpss %dx%d size=%u cnt=%u\n",
		   VPSS_OUT_W, VPSS_OUT_H,
		   stVbConf.astCommPool[1].u32BlkSize,
		   stVbConf.astCommPool[1].u32BlkCnt);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = sys_handle_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	s32Ret = SAMPLE_COMM_SYS_Init(&stVbConf);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("system init failed: %#x\n", s32Ret);
		return s32Ret;
	}

	s32Ret = SAMPLE_PLAT_VI_INIT(&stViConfig);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vi init failed: %#x\n", s32Ret);
		SAMPLE_PRT("IMX678 I2C probe failed. After a VPSS kernel Oops, run:\n");
		SAMPLE_PRT("  zonhor-cam-recover reload   # or reboot\n");
		SAMPLE_PRT("  i2ctransfer -y 3 w2@0x1a 0x30 0x22 r1   # expect 0x01\n");
		/* SAMPLE_PLAT_VI_INIT already tears down MMF on error. */
		return s32Ret;
	}

	memset(&stVpssGrpAttr, 0, sizeof(stVpssGrpAttr));
	memset(astVpssChnAttr, 0, sizeof(astVpssChnAttr));

	stVpssGrpAttr.stFrameRate.s32SrcFrameRate = -1;
	stVpssGrpAttr.stFrameRate.s32DstFrameRate = -1;
	stVpssGrpAttr.enPixelFormat = SAMPLE_PIXEL_FORMAT;
	stVpssGrpAttr.u32MaxW = g_stSensorSize.u32Width;
	stVpssGrpAttr.u32MaxH = g_stSensorSize.u32Height;
	stVpssGrpAttr.u8VpssDev = 0;

	astVpssChnAttr[VpssChn].u32Width = VPSS_OUT_W;
	astVpssChnAttr[VpssChn].u32Height = VPSS_OUT_H;
	astVpssChnAttr[VpssChn].enVideoFormat = VIDEO_FORMAT_LINEAR;
	astVpssChnAttr[VpssChn].enPixelFormat = SAMPLE_PIXEL_FORMAT;
	astVpssChnAttr[VpssChn].stFrameRate.s32SrcFrameRate = -1;
	astVpssChnAttr[VpssChn].stFrameRate.s32DstFrameRate = -1;
	astVpssChnAttr[VpssChn].u32Depth = 1;
	astVpssChnAttr[VpssChn].bMirror = mirror;
	astVpssChnAttr[VpssChn].bFlip = flip;
	astVpssChnAttr[VpssChn].stAspectRatio.enMode = ASPECT_RATIO_AUTO;
	astVpssChnAttr[VpssChn].stAspectRatio.bEnableBgColor = CVI_TRUE;
	astVpssChnAttr[VpssChn].stAspectRatio.u32BgColor = COLOR_RGB_BLACK;
	astVpssChnAttr[VpssChn].stNormalize.bEnable = CVI_FALSE;

	abChnEnable[VpssChn] = CVI_TRUE;
	memcpy(g_vpss_chn_enabled, abChnEnable, sizeof(g_vpss_chn_enabled));
	s32Ret = SAMPLE_COMM_VPSS_Init(VpssGrp, abChnEnable, &stVpssGrpAttr, astVpssChnAttr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vpss init failed: %#x\n", s32Ret);
		return s32Ret;
	}

	s32Ret = SAMPLE_COMM_VPSS_Start(VpssGrp, abChnEnable, &stVpssGrpAttr, astVpssChnAttr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vpss start failed: %#x\n", s32Ret);
		return s32Ret;
	}
	g_vpss_started = CVI_TRUE;

	s32Ret = CVI_VPSS_SetChnRotation(VpssGrp, VpssChn, ROTATION_90);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vpss set rotation failed: %#x\n", s32Ret);
		return s32Ret;
	}

	s32Ret = SAMPLE_COMM_VI_Bind_VPSS(0, 0, VpssGrp);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vi bind vpss failed: %#x\n", s32Ret);
		return s32Ret;
	}
	g_vpss_bound = CVI_TRUE;

	SAMPLE_PRT("Pipeline ready: sensor %ux%u -> VPSS %dx%d ROT90 (disp ~%dx%d) -> LCD %dx%d\n",
		   g_stSensorSize.u32Width, g_stSensorSize.u32Height,
		   VPSS_OUT_W, VPSS_OUT_H, VPSS_OUT_H, VPSS_OUT_W,
		   FB_LCD_WIDTH, FB_LCD_HEIGHT);

	return CVI_SUCCESS;
}

static void sys_mm_deinit(void)
{
	VPSS_GRP VpssGrp = 0;
	VPSS_CHN VpssChn = VPSS_CHN0;

	if (g_frame_held) {
		CVI_VPSS_ReleaseChnFrame(VpssGrp, VpssChn, &g_held_frame);
		g_frame_held = CVI_FALSE;
	}

	if (g_vpss_bound) {
		SAMPLE_COMM_VI_UnBind_VPSS(0, 0, VpssGrp);
		g_vpss_bound = CVI_FALSE;
	}
	if (g_vpss_started) {
		SAMPLE_COMM_VPSS_Stop(VpssGrp, g_vpss_chn_enabled);
		g_vpss_started = CVI_FALSE;
		memset(g_vpss_chn_enabled, 0, sizeof(g_vpss_chn_enabled));
	}

	SAMPLE_COMM_VI_DestroyIsp(&g_stViConfig);
	SAMPLE_COMM_VI_DestroyVi(&g_stViConfig);
	SAMPLE_COMM_SYS_Exit();
}

static CVI_S32 map_vpss_frame(VIDEO_FRAME_INFO_S *pstFrame, void **ppVir,
			      size_t *pSize)
{
	size_t image_size;
	CVI_U32 plane_offset = 0;
	int i;

	image_size = pstFrame->stVFrame.u32Length[0] + pstFrame->stVFrame.u32Length[1]
		     + pstFrame->stVFrame.u32Length[2];
	if (image_size == 0)
		return CVI_FAILURE;

	*ppVir = CVI_SYS_Mmap(pstFrame->stVFrame.u64PhyAddr[0], image_size);
	if (*ppVir == NULL)
		return CVI_FAILURE;

	CVI_SYS_IonInvalidateCache(pstFrame->stVFrame.u64PhyAddr[0], *ppVir, image_size);

	for (i = 0; i < 3; i++) {
		if (pstFrame->stVFrame.u32Length[i] != 0) {
			pstFrame->stVFrame.pu8VirAddr[i] = (CVI_U8 *)*ppVir + plane_offset;
			plane_offset += pstFrame->stVFrame.u32Length[i];
		}
	}

	*pSize = image_size;
	return CVI_SUCCESS;
}

static void unmap_vpss_frame(void *pVir, size_t size)
{
	if (pVir)
		CVI_SYS_Munmap(pVir, size);
}

static void print_usage(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("  -m    Enable VPSS horizontal mirror\n");
	printf("  -f    Enable VPSS vertical flip\n");
	printf("  -h    Show this help\n");
	printf("\nRequires /mnt/data/sensor_cfg.ini (see readme.md).\n");
	printf("Stop other /dev/fb0 users (e.g. screen_demo.py) before running.\n");
}

int main(int argc, char **argv)
{
	CVI_S32 s32Ret;
	FB_LCD_S fb_lcd;
	uint16_t *rgb_buf = NULL;
	VIDEO_FRAME_INFO_S stFrame;
	VPSS_GRP VpssGrp = 0;
	VPSS_CHN VpssChn = VPSS_CHN0;
	CVI_BOOL mirror = CVI_FALSE;
	CVI_BOOL flip = CVI_FALSE;
	int opt;
	int frame_count = 0;
	int fps_count = 0;
	struct timespec fps_start, frame_start, frame_end;
	CVI_BOOL first_frame_logged = CVI_FALSE;

	while ((opt = getopt(argc, argv, "mfh")) != -1) {
		switch (opt) {
		case 'm':
			mirror = CVI_TRUE;
			break;
		case 'f':
			flip = CVI_TRUE;
			break;
		case 'h':
		default:
			print_usage(argv[0]);
			return opt == 'h' ? 0 : 1;
		}
	}

	setbuf(stdout, NULL);

	rgb_buf = calloc((size_t)VPSS_RGB_PIXELS, sizeof(uint16_t));
	if (!rgb_buf) {
		SAMPLE_PRT("calloc rgb buffer failed\n");
		return 1;
	}

	s32Ret = sys_mm_init(mirror, flip);
	if (s32Ret != CVI_SUCCESS) {
		free(rgb_buf);
		return 1;
	}

	if (fb_lcd_open(&fb_lcd) != 0) {
		sys_mm_deinit();
		free(rgb_buf);
		return 1;
	}
	fb_lcd_clear(&fb_lcd, 0x0000);

	usleep(500 * 1000);
	clock_gettime(CLOCK_MONOTONIC, &fps_start);

	SAMPLE_PRT("Preview started. Press Ctrl+C to exit.\n");

	while (g_running) {
		void *vir_addr = NULL;
		size_t map_size = 0;

		clock_gettime(CLOCK_MONOTONIC, &frame_start);

		s32Ret = CVI_VPSS_GetChnFrame(VpssGrp, VpssChn, &stFrame, 1000);
		if (s32Ret != CVI_SUCCESS)
			continue;

		g_held_frame = stFrame;
		g_frame_held = CVI_TRUE;

		if (!first_frame_logged) {
			SAMPLE_PRT("VPSS frame %ux%u stride Y=%u UV=%u fmt=%d\n",
				   stFrame.stVFrame.u32Width, stFrame.stVFrame.u32Height,
				   stFrame.stVFrame.u32Stride[0],
				   stFrame.stVFrame.u32Stride[1],
				   stFrame.stVFrame.enPixelFormat);
			first_frame_logged = CVI_TRUE;
		}

		if (map_vpss_frame(&stFrame, &vir_addr, &map_size) == CVI_SUCCESS) {
			CVI_U32 fw = stFrame.stVFrame.u32Width;
			CVI_U32 fh = stFrame.stVFrame.u32Height;

			if (fw > 0 && fh > 0 && fw * fh <= VPSS_RGB_PIXELS &&
			    nv21_frame_to_rgb565(&stFrame.stVFrame, rgb_buf) == 0) {
				fb_lcd_draw_rgb565(&fb_lcd, rgb_buf, (int)fw, (int)fh);
			}
			unmap_vpss_frame(vir_addr, map_size);
		}

		CVI_VPSS_ReleaseChnFrame(VpssGrp, VpssChn, &stFrame);
		g_frame_held = CVI_FALSE;

		frame_count++;
		fps_count++;
		clock_gettime(CLOCK_MONOTONIC, &frame_end);

		{
			double elapsed = (frame_end.tv_sec - fps_start.tv_sec)
				+ (frame_end.tv_nsec - fps_start.tv_nsec) / 1e9;
			double frame_ms = (frame_end.tv_sec - frame_start.tv_sec) * 1000.0
				+ (frame_end.tv_nsec - frame_start.tv_nsec) / 1e6;

			if (elapsed >= 1.0) {
				SAMPLE_PRT("FPS: %.1f  last frame: %.1f ms\n",
					   fps_count / elapsed, frame_ms);
				fps_count = 0;
				fps_start = frame_end;
			}
		}
	}

	SAMPLE_PRT("Stopped after %d frames\n", frame_count);

	fb_lcd_close(&fb_lcd);
	sys_mm_deinit();
	free(rgb_buf);

	return 0;
}
