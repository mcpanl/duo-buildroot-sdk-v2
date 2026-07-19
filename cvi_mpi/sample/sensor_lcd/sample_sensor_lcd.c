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
#include "rgb888_rgb565.h"
#include "perf_stats.h"
#include "sys_status.h"

#define VPSS_ALIGN 64
#define VPSS_ALIGN_UP(x) ((((x) + VPSS_ALIGN - 1) / VPSS_ALIGN) * VPSS_ALIGN)

/*
 * GDC rotation only supports NV12/NV21/YUV400 — not RGB888.
 * Cascade:
 *   Grp0: scale + ROTATION_90 in NV21 (320x192 -> 192x320)
 *   Grp1: HW CSC NV21 -> Packed RGB888 (192x320, no rotation)
 */
#define VPSS_PRE_W 320
#define VPSS_PRE_H VPSS_ALIGN_UP(172)
#define VPSS_DISP_W VPSS_PRE_H
#define VPSS_DISP_H VPSS_PRE_W
#define VPSS_CSC_FMT PIXEL_FORMAT_RGB_888

#define VPSS_GRP_ROT 0
#define VPSS_GRP_CSC 1

/* Max RGB565 scratch (post-rotate display buffer). */
#define VPSS_RGB_PIXELS (VPSS_DISP_W * VPSS_DISP_H)

static SAMPLE_VI_CONFIG_S g_stViConfig;
static SIZE_S g_stSensorSize;
static volatile sig_atomic_t g_running = 1;
static CVI_BOOL g_sys_inited = CVI_FALSE;
static CVI_BOOL g_vi_inited = CVI_FALSE;
static CVI_BOOL g_vpss0_started = CVI_FALSE;
static CVI_BOOL g_vpss1_started = CVI_FALSE;
static CVI_BOOL g_vi_vpss_bound = CVI_FALSE;
static CVI_BOOL g_vpss_vpss_bound = CVI_FALSE;
static CVI_BOOL g_vpss0_chn_enabled[VPSS_MAX_PHY_CHN_NUM] = {0};
static CVI_BOOL g_vpss1_chn_enabled[VPSS_MAX_PHY_CHN_NUM] = {0};
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

/* Best-effort destroy for leftover groups (e.g. previous failed init). */
static void vpss_try_destroy(VPSS_GRP grp)
{
	CVI_S32 j;

	for (j = 0; j < VPSS_MAX_PHY_CHN_NUM; j++)
		CVI_VPSS_DisableChn(grp, j);
	CVI_VPSS_StopGrp(grp);
	CVI_VPSS_DestroyGrp(grp);
}

static void sys_mm_deinit(void);

static CVI_S32 sys_mm_init(CVI_BOOL mirror, CVI_BOOL flip)
{
	MMF_VERSION_S stVersion;
	SAMPLE_INI_CFG_S stIniCfg;
	SAMPLE_VI_CONFIG_S stViConfig;
	CVI_S32 s32Ret;
	LOG_LEVEL_CONF_S log_conf;
	VB_CONFIG_S stVbConf;
	CVI_U32 u32ViBlk, u32ViRotBlk, u32Nv21Blk, u32Nv21RotBlk, u32RgbBlk;
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

	{
		SAMPLE_SNS_MODE_INFO_S stModeInfo;

		s32Ret = SAMPLE_COMM_SNS_QueryActiveMode(&stIniCfg, &stModeInfo);
		if (s32Ret != CVI_SUCCESS)
			return s32Ret;
		g_stSensorSize = stModeInfo.stSize;
		SAMPLE_PRT("Sensor mode=%s size=%ux%u raw=%ubit snsMode=%u bin=%s\n",
			   stModeInfo.pszModeName,
			   stModeInfo.stSize.u32Width, stModeInfo.stSize.u32Height,
			   stModeInfo.u8RawBitDepth, stModeInfo.u8SnsMode,
			   stModeInfo.pszIspBinPath ? stModeInfo.pszIspBinPath : "(default)");
	}

	CVI_VI_SetDevNum(stIniCfg.devNum);

	s32Ret = SAMPLE_COMM_VI_IniToViCfg(&stIniCfg, &stViConfig);
	if (s32Ret != CVI_SUCCESS)
		return s32Ret;

	memcpy(&g_stViConfig, &stViConfig, sizeof(SAMPLE_VI_CONFIG_S));

	memset(&stVbConf, 0, sizeof(stVbConf));
	u32ViBlk = vb_pool_blk_size(g_stSensorSize.u32Width, g_stSensorSize.u32Height,
				    stViConfig.astViInfo[0].stChnInfo.enPixFormat);
	u32ViRotBlk = vb_pool_blk_size(g_stSensorSize.u32Height, g_stSensorSize.u32Width,
				       stViConfig.astViInfo[0].stChnInfo.enPixFormat);
	u32ViBlk = u32ViBlk > u32ViRotBlk ? u32ViBlk : u32ViRotBlk;

	/* Grp0 NV21 (pre- and post-rotate). */
	u32Nv21Blk = vb_pool_blk_size(VPSS_PRE_W, VPSS_PRE_H, SAMPLE_PIXEL_FORMAT);
	u32Nv21RotBlk = vb_pool_blk_size(VPSS_DISP_W, VPSS_DISP_H, SAMPLE_PIXEL_FORMAT);
	u32Nv21Blk = u32Nv21Blk > u32Nv21RotBlk ? u32Nv21Blk : u32Nv21RotBlk;

	/* Grp1 Packed RGB888 (display size). */
	u32RgbBlk = vb_pool_blk_size(VPSS_DISP_W, VPSS_DISP_H, VPSS_CSC_FMT);

	stVbConf.u32MaxPoolCnt = 3;
	stVbConf.astCommPool[0].u32BlkSize = u32ViBlk;
	stVbConf.astCommPool[0].u32BlkCnt = 5;
	stVbConf.astCommPool[0].enRemapMode = VB_REMAP_MODE_CACHED;
	stVbConf.astCommPool[1].u32BlkSize = u32Nv21Blk;
	stVbConf.astCommPool[1].u32BlkCnt = 5;
	stVbConf.astCommPool[1].enRemapMode = VB_REMAP_MODE_CACHED;
	stVbConf.astCommPool[2].u32BlkSize = u32RgbBlk;
	stVbConf.astCommPool[2].u32BlkCnt = 4;
	stVbConf.astCommPool[2].enRemapMode = VB_REMAP_MODE_CACHED;

	SAMPLE_PRT("VB pool[0] sensor %ux%u size=%u cnt=%u\n",
		   g_stSensorSize.u32Width, g_stSensorSize.u32Height,
		   stVbConf.astCommPool[0].u32BlkSize,
		   stVbConf.astCommPool[0].u32BlkCnt);
	SAMPLE_PRT("VB pool[1] vpss NV21 %dx%d size=%u cnt=%u\n",
		   VPSS_PRE_W, VPSS_PRE_H,
		   stVbConf.astCommPool[1].u32BlkSize,
		   stVbConf.astCommPool[1].u32BlkCnt);
	SAMPLE_PRT("VB pool[2] vpss RGB888 %dx%d size=%u cnt=%u\n",
		   VPSS_DISP_W, VPSS_DISP_H,
		   stVbConf.astCommPool[2].u32BlkSize,
		   stVbConf.astCommPool[2].u32BlkCnt);

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
	g_sys_inited = CVI_TRUE;

	s32Ret = SAMPLE_PLAT_VI_INIT(&stViConfig);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vi init failed: %#x\n", s32Ret);
		SAMPLE_PRT("IMX678 I2C probe failed. After a VPSS kernel Oops, run:\n");
		SAMPLE_PRT("  zonhor-cam-recover reload   # or reboot\n");
		SAMPLE_PRT("  i2ctransfer -y 3 w2@0x1a 0x30 0x22 r1   # expect 0x01\n");
		/* SAMPLE_PLAT_VI_INIT already tears down MMF on error. */
		g_sys_inited = CVI_FALSE;
		return s32Ret;
	}
	g_vi_inited = CVI_TRUE;

	/* Clear leftovers from previous failed runs (Grp occupied / 0xc0068004). */
	vpss_try_destroy(VPSS_GRP_ROT);
	vpss_try_destroy(VPSS_GRP_CSC);

	/* ---- Grp0: scale + ROT90 in NV21 (GDC-compatible) ---- */
	memset(&stVpssGrpAttr, 0, sizeof(stVpssGrpAttr));
	memset(astVpssChnAttr, 0, sizeof(astVpssChnAttr));
	memset(abChnEnable, 0, sizeof(abChnEnable));

	stVpssGrpAttr.stFrameRate.s32SrcFrameRate = -1;
	stVpssGrpAttr.stFrameRate.s32DstFrameRate = -1;
	stVpssGrpAttr.enPixelFormat = SAMPLE_PIXEL_FORMAT;
	stVpssGrpAttr.u32MaxW = g_stSensorSize.u32Width;
	stVpssGrpAttr.u32MaxH = g_stSensorSize.u32Height;
	stVpssGrpAttr.u8VpssDev = 0;

	astVpssChnAttr[VpssChn].u32Width = VPSS_PRE_W;
	astVpssChnAttr[VpssChn].u32Height = VPSS_PRE_H;
	astVpssChnAttr[VpssChn].enVideoFormat = VIDEO_FORMAT_LINEAR;
	astVpssChnAttr[VpssChn].enPixelFormat = SAMPLE_PIXEL_FORMAT;
	astVpssChnAttr[VpssChn].stFrameRate.s32SrcFrameRate = -1;
	astVpssChnAttr[VpssChn].stFrameRate.s32DstFrameRate = -1;
	astVpssChnAttr[VpssChn].u32Depth = 1; /* bound to Grp1 + YUV snapshot */
	astVpssChnAttr[VpssChn].bMirror = mirror;
	astVpssChnAttr[VpssChn].bFlip = flip;
	astVpssChnAttr[VpssChn].stAspectRatio.enMode = ASPECT_RATIO_AUTO;
	astVpssChnAttr[VpssChn].stAspectRatio.bEnableBgColor = CVI_TRUE;
	astVpssChnAttr[VpssChn].stAspectRatio.u32BgColor = COLOR_RGB_BLACK;
	astVpssChnAttr[VpssChn].stNormalize.bEnable = CVI_FALSE;

	abChnEnable[VpssChn] = CVI_TRUE;
	memcpy(g_vpss0_chn_enabled, abChnEnable, sizeof(g_vpss0_chn_enabled));
	s32Ret = SAMPLE_COMM_VPSS_Init(VPSS_GRP_ROT, abChnEnable, &stVpssGrpAttr, astVpssChnAttr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vpss grp0 init failed: %#x\n", s32Ret);
		goto fail;
	}
	g_vpss0_started = CVI_TRUE; /* CreateGrp done; Stop destroys even if Start fails */

	s32Ret = SAMPLE_COMM_VPSS_Start(VPSS_GRP_ROT, abChnEnable, &stVpssGrpAttr, astVpssChnAttr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vpss grp0 start failed: %#x\n", s32Ret);
		goto fail;
	}

	s32Ret = CVI_VPSS_SetChnRotation(VPSS_GRP_ROT, VpssChn, ROTATION_90);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vpss grp0 set rotation failed: %#x\n", s32Ret);
		goto fail;
	}

	/* ---- Grp1: HW CSC NV21 -> RGB888 (no rotation) ---- */
	memset(&stVpssGrpAttr, 0, sizeof(stVpssGrpAttr));
	memset(astVpssChnAttr, 0, sizeof(astVpssChnAttr));
	memset(abChnEnable, 0, sizeof(abChnEnable));

	stVpssGrpAttr.stFrameRate.s32SrcFrameRate = -1;
	stVpssGrpAttr.stFrameRate.s32DstFrameRate = -1;
	stVpssGrpAttr.enPixelFormat = SAMPLE_PIXEL_FORMAT;
	stVpssGrpAttr.u32MaxW = VPSS_DISP_W;
	stVpssGrpAttr.u32MaxH = VPSS_DISP_H;
	stVpssGrpAttr.u8VpssDev = 0;

	astVpssChnAttr[VpssChn].u32Width = VPSS_DISP_W;
	astVpssChnAttr[VpssChn].u32Height = VPSS_DISP_H;
	astVpssChnAttr[VpssChn].enVideoFormat = VIDEO_FORMAT_LINEAR;
	astVpssChnAttr[VpssChn].enPixelFormat = VPSS_CSC_FMT;
	astVpssChnAttr[VpssChn].stFrameRate.s32SrcFrameRate = -1;
	astVpssChnAttr[VpssChn].stFrameRate.s32DstFrameRate = -1;
	astVpssChnAttr[VpssChn].u32Depth = 1;
	astVpssChnAttr[VpssChn].bMirror = CVI_FALSE;
	astVpssChnAttr[VpssChn].bFlip = CVI_FALSE;
	astVpssChnAttr[VpssChn].stAspectRatio.enMode = ASPECT_RATIO_NONE;
	astVpssChnAttr[VpssChn].stAspectRatio.bEnableBgColor = CVI_FALSE;
	astVpssChnAttr[VpssChn].stNormalize.bEnable = CVI_FALSE;

	abChnEnable[VpssChn] = CVI_TRUE;
	memcpy(g_vpss1_chn_enabled, abChnEnable, sizeof(g_vpss1_chn_enabled));
	s32Ret = SAMPLE_COMM_VPSS_Init(VPSS_GRP_CSC, abChnEnable, &stVpssGrpAttr, astVpssChnAttr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vpss grp1 init failed: %#x\n", s32Ret);
		goto fail;
	}
	g_vpss1_started = CVI_TRUE;

	s32Ret = SAMPLE_COMM_VPSS_Start(VPSS_GRP_CSC, abChnEnable, &stVpssGrpAttr, astVpssChnAttr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vpss grp1 start failed: %#x\n", s32Ret);
		goto fail;
	}

	s32Ret = SAMPLE_COMM_VI_Bind_VPSS(0, 0, VPSS_GRP_ROT);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vi bind vpss0 failed: %#x\n", s32Ret);
		goto fail;
	}
	g_vi_vpss_bound = CVI_TRUE;

	s32Ret = SAMPLE_COMM_VPSS_Bind_VPSS(VPSS_GRP_ROT, VpssChn, VPSS_GRP_CSC);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vpss0 bind vpss1 failed: %#x\n", s32Ret);
		goto fail;
	}
	g_vpss_vpss_bound = CVI_TRUE;

	SAMPLE_PRT("Pipeline ready: sensor %ux%u -> VPSS0 %dx%d ROT90 NV21 -> VPSS1 %dx%d RGB888 -> LCD %dx%d\n",
		   g_stSensorSize.u32Width, g_stSensorSize.u32Height,
		   VPSS_PRE_W, VPSS_PRE_H, VPSS_DISP_W, VPSS_DISP_H,
		   FB_LCD_WIDTH, FB_LCD_HEIGHT);

	return CVI_SUCCESS;

fail:
	sys_mm_deinit();
	return s32Ret;
}

static void sys_mm_deinit(void)
{
	VPSS_CHN VpssChn = VPSS_CHN0;

	if (g_frame_held) {
		CVI_VPSS_ReleaseChnFrame(VPSS_GRP_CSC, VpssChn, &g_held_frame);
		g_frame_held = CVI_FALSE;
	}

	if (g_vpss_vpss_bound) {
		SAMPLE_COMM_VPSS_UnBind_VPSS(VPSS_GRP_ROT, VpssChn, VPSS_GRP_CSC);
		g_vpss_vpss_bound = CVI_FALSE;
	}
	if (g_vi_vpss_bound) {
		SAMPLE_COMM_VI_UnBind_VPSS(0, 0, VPSS_GRP_ROT);
		g_vi_vpss_bound = CVI_FALSE;
	}
	if (g_vpss1_started) {
		SAMPLE_COMM_VPSS_Stop(VPSS_GRP_CSC, g_vpss1_chn_enabled);
		g_vpss1_started = CVI_FALSE;
		memset(g_vpss1_chn_enabled, 0, sizeof(g_vpss1_chn_enabled));
	} else {
		vpss_try_destroy(VPSS_GRP_CSC);
	}
	if (g_vpss0_started) {
		SAMPLE_COMM_VPSS_Stop(VPSS_GRP_ROT, g_vpss0_chn_enabled);
		g_vpss0_started = CVI_FALSE;
		memset(g_vpss0_chn_enabled, 0, sizeof(g_vpss0_chn_enabled));
	} else {
		vpss_try_destroy(VPSS_GRP_ROT);
	}

	if (g_vi_inited) {
		SAMPLE_COMM_VI_DestroyIsp(&g_stViConfig);
		SAMPLE_COMM_VI_DestroyVi(&g_stViConfig);
		g_vi_inited = CVI_FALSE;
	}
	if (g_sys_inited) {
		SAMPLE_COMM_SYS_Exit();
		g_sys_inited = CVI_FALSE;
	}
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
	VPSS_CHN VpssChn = VPSS_CHN0;
	CVI_BOOL mirror = CVI_FALSE;
	CVI_BOOL flip = CVI_FALSE;
	int opt;
	int frame_count = 0;
	struct timespec perf_report_start;
	struct timespec frame_loop_start, frame_loop_end;
	struct timespec step_start, step_end;
	SYS_STATUS_S sys_status;
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

	if (sys_status_start() != 0)
		SAMPLE_PRT("warn: status thread failed, HUD may be stale\n");

	usleep(500 * 1000);
	perf_timespec_now(&perf_report_start);

	SAMPLE_PRT("Preview started. Press Ctrl+C to exit.\n");
	SAMPLE_PRT("Perf stats every %ds (temporary instrumentation).\n",
		   PERF_REPORT_INTERVAL_SEC);

	while (g_running) {
		void *vir_addr = NULL;
		size_t map_size = 0;

		perf_timespec_now(&frame_loop_start);

		perf_timespec_now(&step_start);
		s32Ret = CVI_VPSS_GetChnFrame(VPSS_GRP_CSC, VpssChn, &stFrame, 1000);
		perf_timespec_now(&step_end);
		if (s32Ret != CVI_SUCCESS)
			continue;
		perf_record(PERF_VPSS_GET_FRAME, perf_elapsed_ns(&step_start, &step_end));

		g_held_frame = stFrame;
		g_frame_held = CVI_TRUE;

		if (!first_frame_logged) {
			SAMPLE_PRT("VPSS frame %ux%u stride=%u fmt=%d (expect RGB_888=%d)\n",
				   stFrame.stVFrame.u32Width, stFrame.stVFrame.u32Height,
				   stFrame.stVFrame.u32Stride[0],
				   stFrame.stVFrame.enPixelFormat,
				   PIXEL_FORMAT_RGB_888);
			first_frame_logged = CVI_TRUE;
		}

		perf_timespec_now(&step_start);
		if (map_vpss_frame(&stFrame, &vir_addr, &map_size) == CVI_SUCCESS) {
			perf_timespec_now(&step_end);
			perf_record(PERF_VPSS_MAP, perf_elapsed_ns(&step_start, &step_end));

			CVI_U32 fw = stFrame.stVFrame.u32Width;
			CVI_U32 fh = stFrame.stVFrame.u32Height;

			if (fw > 0 && fh > 0 && fw * fh <= VPSS_RGB_PIXELS) {
				perf_timespec_now(&step_start);
				if (rgb888_frame_to_rgb565(&stFrame.stVFrame, rgb_buf) == 0) {
					perf_timespec_now(&step_end);
					perf_record(PERF_RGB888_RGB565,
						    perf_elapsed_ns(&step_start, &step_end));

					fb_lcd_draw_rgb565(&fb_lcd, rgb_buf, (int)fw, (int)fh);
					sys_status_get(&sys_status);
					fb_lcd_draw_status_hud(&fb_lcd,
								 sys_status.battery_valid,
								 sys_status.battery_pct,
								 sys_status.temp_valid,
								 sys_status.temp_c);
				}
			}

			perf_timespec_now(&step_start);
			unmap_vpss_frame(vir_addr, map_size);
			perf_timespec_now(&step_end);
			perf_record(PERF_VPSS_UNMAP, perf_elapsed_ns(&step_start, &step_end));
		}

		perf_timespec_now(&step_start);
		CVI_VPSS_ReleaseChnFrame(VPSS_GRP_CSC, VpssChn, &stFrame);
		perf_timespec_now(&step_end);
		perf_record(PERF_VPSS_RELEASE, perf_elapsed_ns(&step_start, &step_end));
		g_frame_held = CVI_FALSE;

		frame_count++;
		perf_timespec_now(&frame_loop_end);
		perf_record(PERF_FRAME_TOTAL,
			    perf_elapsed_ns(&frame_loop_start, &frame_loop_end));

		if (perf_report_due(&perf_report_start, PERF_REPORT_INTERVAL_SEC)) {
			perf_print_report();
			perf_reset_window();
		}
	}

	SAMPLE_PRT("Stopped after %d frames\n", frame_count);

	sys_status_stop();
	fb_lcd_clear(&fb_lcd, 0x0000);
	fb_lcd_close(&fb_lcd);
	sys_mm_deinit();
	free(rgb_buf);

	return 0;
}
