/*
 * Pipeline color-stage benchmark: VI / VPSS-YUV / VPSS-RGB888 side-by-side.
 *
 * Uses naive conversions only (no SG2000 B-G-R workaround):
 *   NV21  -> RGB565 via BT.601, NV21 V-U chroma order
 *   RGB888 byte0=R, byte1=G, byte2=B per PIXEL_FORMAT_RGB_888
 *
 * Screen layout (top -> bottom, 172x320):
 *   [red marker]    VI NV21 output
 *   [green marker]  VPSS0 NV21 (scale+ROT90)
 *   [blue marker]   VPSS1 RGB888 (HW CSC)
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include "cvi_buffer.h"
#include "cvi_comm_isp.h"
#include "cvi_comm_sns.h"
#include "cvi_vpss.h"
#include "sample_comm.h"
#include "bench_convert.h"
#include "bench_fb.h"
#include "../sensor_lcd/fb_lcd.h"

#define VPSS_ALIGN 64
#define VPSS_ALIGN_UP(x) ((((x) + VPSS_ALIGN - 1) / VPSS_ALIGN) * VPSS_ALIGN)

#define VPSS_PRE_W 320
#define VPSS_PRE_H VPSS_ALIGN_UP(172)
#define VPSS_DISP_W VPSS_PRE_H
#define VPSS_DISP_H VPSS_PRE_W
#define VPSS_CSC_FMT PIXEL_FORMAT_RGB_888

#define VPSS_GRP_ROT 0
#define VPSS_GRP_CSC 1

#define PANEL_PIXELS (FB_LCD_WIDTH * FB_LCD_HEIGHT)

static SAMPLE_VI_CONFIG_S g_stViConfig;
static SIZE_S g_stSensorSize;
static VI_PIPE g_vi_pipe;
static VI_CHN g_vi_chn;

static volatile sig_atomic_t g_running = 1;
static CVI_BOOL g_sys_inited = CVI_FALSE;
static CVI_BOOL g_vi_inited = CVI_FALSE;
static CVI_BOOL g_vpss0_started = CVI_FALSE;
static CVI_BOOL g_vpss1_started = CVI_FALSE;
static CVI_BOOL g_vi_vpss_bound = CVI_FALSE;
static CVI_BOOL g_vpss_vpss_bound = CVI_FALSE;
static CVI_BOOL g_vpss0_chn_enabled[VPSS_MAX_PHY_CHN_NUM] = {0};
static CVI_BOOL g_vpss1_chn_enabled[VPSS_MAX_PHY_CHN_NUM] = {0};

static void on_signal(int sig)
{
	(void)sig;
	g_running = 0;
}

static CVI_U32 vb_pool_blk_size(CVI_U32 w, CVI_U32 h, PIXEL_FORMAT_E fmt)
{
	return COMMON_GetPicBufferSize(w, h, fmt, DATA_BITWIDTH_8,
				       COMPRESS_MODE_NONE, DEFAULT_ALIGN);
}

static void vpss_try_destroy(VPSS_GRP grp)
{
	CVI_S32 j;

	for (j = 0; j < VPSS_MAX_PHY_CHN_NUM; j++)
		CVI_VPSS_DisableChn(grp, j);
	CVI_VPSS_StopGrp(grp);
	CVI_VPSS_DestroyGrp(grp);
}

static CVI_S32 vi_set_chn_depth(CVI_U32 depth)
{
	VI_CHN_ATTR_S stChnAttr;
	CVI_S32 ret;

	ret = CVI_VI_GetChnAttr(g_vi_pipe, g_vi_chn, &stChnAttr);
	if (ret != CVI_SUCCESS)
		return ret;

	stChnAttr.u32Depth = depth;
	return CVI_VI_SetChnAttr(g_vi_pipe, g_vi_chn, &stChnAttr);
}

static void sys_mm_deinit(void);

static CVI_S32 sys_mm_init(void)
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
	SAMPLE_SNS_MODE_INFO_S stModeInfo;

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

	s32Ret = SAMPLE_COMM_SNS_QueryActiveMode(&stIniCfg, &stModeInfo);
	if (s32Ret != CVI_SUCCESS)
		return s32Ret;
	g_stSensorSize = stModeInfo.stSize;
	SAMPLE_PRT("Sensor mode=%s size=%ux%u raw=%ubit snsMode=%u bin=%s\n",
		   stModeInfo.pszModeName,
		   stModeInfo.stSize.u32Width, stModeInfo.stSize.u32Height,
		   stModeInfo.u8RawBitDepth, stModeInfo.u8SnsMode,
		   stModeInfo.pszIspBinPath ? stModeInfo.pszIspBinPath : "(default)");

	CVI_VI_SetDevNum(stIniCfg.devNum);

	s32Ret = SAMPLE_COMM_VI_IniToViCfg(&stIniCfg, &stViConfig);
	if (s32Ret != CVI_SUCCESS)
		return s32Ret;

	memcpy(&g_stViConfig, &stViConfig, sizeof(SAMPLE_VI_CONFIG_S));
	g_vi_pipe = stViConfig.astViInfo[0].stPipeInfo.aPipe[0];
	g_vi_chn = stViConfig.astViInfo[0].stChnInfo.ViChn;

	memset(&stVbConf, 0, sizeof(stVbConf));
	u32ViBlk = vb_pool_blk_size(g_stSensorSize.u32Width, g_stSensorSize.u32Height,
				    stViConfig.astViInfo[0].stChnInfo.enPixFormat);
	u32ViRotBlk = vb_pool_blk_size(g_stSensorSize.u32Height, g_stSensorSize.u32Width,
				       stViConfig.astViInfo[0].stChnInfo.enPixFormat);
	u32ViBlk = u32ViBlk > u32ViRotBlk ? u32ViBlk : u32ViRotBlk;

	u32Nv21Blk = vb_pool_blk_size(VPSS_PRE_W, VPSS_PRE_H, SAMPLE_PIXEL_FORMAT);
	u32Nv21RotBlk = vb_pool_blk_size(VPSS_DISP_W, VPSS_DISP_H, SAMPLE_PIXEL_FORMAT);
	u32Nv21Blk = u32Nv21Blk > u32Nv21RotBlk ? u32Nv21Blk : u32Nv21RotBlk;
	u32RgbBlk = vb_pool_blk_size(VPSS_DISP_W, VPSS_DISP_H, VPSS_CSC_FMT);

	stVbConf.u32MaxPoolCnt = 3;
	stVbConf.astCommPool[0].u32BlkSize = u32ViBlk;
	stVbConf.astCommPool[0].u32BlkCnt = 6;
	stVbConf.astCommPool[0].enRemapMode = VB_REMAP_MODE_CACHED;
	stVbConf.astCommPool[1].u32BlkSize = u32Nv21Blk;
	stVbConf.astCommPool[1].u32BlkCnt = 6;
	stVbConf.astCommPool[1].enRemapMode = VB_REMAP_MODE_CACHED;
	stVbConf.astCommPool[2].u32BlkSize = u32RgbBlk;
	stVbConf.astCommPool[2].u32BlkCnt = 4;
	stVbConf.astCommPool[2].enRemapMode = VB_REMAP_MODE_CACHED;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	s32Ret = SAMPLE_COMM_SYS_Init(&stVbConf);
	if (s32Ret != CVI_SUCCESS)
		return s32Ret;
	g_sys_inited = CVI_TRUE;

	s32Ret = SAMPLE_PLAT_VI_INIT(&stViConfig);
	if (s32Ret != CVI_SUCCESS) {
		sys_mm_deinit();
		return s32Ret;
	}
	g_vi_inited = CVI_TRUE;

	s32Ret = vi_set_chn_depth(1);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("VI SetChn depth failed: %#x\n", s32Ret);
		sys_mm_deinit();
		return s32Ret;
	}

	vpss_try_destroy(VPSS_GRP_ROT);
	vpss_try_destroy(VPSS_GRP_CSC);

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
	astVpssChnAttr[VpssChn].u32Depth = 1;
	astVpssChnAttr[VpssChn].bMirror = CVI_FALSE;
	astVpssChnAttr[VpssChn].bFlip = CVI_FALSE;
	astVpssChnAttr[VpssChn].stAspectRatio.enMode = ASPECT_RATIO_AUTO;
	astVpssChnAttr[VpssChn].stAspectRatio.bEnableBgColor = CVI_TRUE;
	astVpssChnAttr[VpssChn].stAspectRatio.u32BgColor = COLOR_RGB_BLACK;
	astVpssChnAttr[VpssChn].stNormalize.bEnable = CVI_FALSE;

	abChnEnable[VpssChn] = CVI_TRUE;
	memcpy(g_vpss0_chn_enabled, abChnEnable, sizeof(g_vpss0_chn_enabled));
	s32Ret = SAMPLE_COMM_VPSS_Init(VPSS_GRP_ROT, abChnEnable, &stVpssGrpAttr, astVpssChnAttr);
	if (s32Ret != CVI_SUCCESS)
		goto fail;
	g_vpss0_started = CVI_TRUE;

	s32Ret = SAMPLE_COMM_VPSS_Start(VPSS_GRP_ROT, abChnEnable, &stVpssGrpAttr, astVpssChnAttr);
	if (s32Ret != CVI_SUCCESS)
		goto fail;

	s32Ret = CVI_VPSS_SetChnRotation(VPSS_GRP_ROT, VpssChn, ROTATION_90);
	if (s32Ret != CVI_SUCCESS)
		goto fail;

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
	if (s32Ret != CVI_SUCCESS)
		goto fail;
	g_vpss1_started = CVI_TRUE;

	s32Ret = SAMPLE_COMM_VPSS_Start(VPSS_GRP_CSC, abChnEnable, &stVpssGrpAttr, astVpssChnAttr);
	if (s32Ret != CVI_SUCCESS)
		goto fail;

	s32Ret = SAMPLE_COMM_VI_Bind_VPSS(0, 0, VPSS_GRP_ROT);
	if (s32Ret != CVI_SUCCESS)
		goto fail;
	g_vi_vpss_bound = CVI_TRUE;

	s32Ret = SAMPLE_COMM_VPSS_Bind_VPSS(VPSS_GRP_ROT, VpssChn, VPSS_GRP_CSC);
	if (s32Ret != CVI_SUCCESS)
		goto fail;
	g_vpss_vpss_bound = CVI_TRUE;

	SAMPLE_PRT("Color bench pipeline ready\n");
	SAMPLE_PRT("  VI:     %ux%u fmt=%d (NV21=%d)\n",
		   g_stSensorSize.u32Width, g_stSensorSize.u32Height,
		   SAMPLE_PIXEL_FORMAT, PIXEL_FORMAT_NV21);
	SAMPLE_PRT("  VPSS0:  %dx%d -> ROT90 -> %dx%d NV21\n",
		   VPSS_PRE_W, VPSS_PRE_H, VPSS_DISP_W, VPSS_DISP_H);
	SAMPLE_PRT("  VPSS1:  %dx%d RGB888\n", VPSS_DISP_W, VPSS_DISP_H);
	SAMPLE_PRT("  LCD:    top=VI  mid=VPSS-YUV  bottom=VPSS-RGB (naive CSC)\n");

	return CVI_SUCCESS;

fail:
	sys_mm_deinit();
	return s32Ret;
}

static void sys_mm_deinit(void)
{
	VPSS_CHN VpssChn = VPSS_CHN0;

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
	} else {
		vpss_try_destroy(VPSS_GRP_CSC);
	}
	if (g_vpss0_started) {
		SAMPLE_COMM_VPSS_Stop(VPSS_GRP_ROT, g_vpss0_chn_enabled);
		g_vpss0_started = CVI_FALSE;
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

static CVI_S32 map_frame(VIDEO_FRAME_INFO_S *pstFrame, void **ppVir, size_t *pSize)
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

static void unmap_frame(void *pVir, size_t size)
{
	if (pVir)
		CVI_SYS_Munmap(pVir, size);
}

static void band_y_range(int band, int *y0, int *h)
{
	if (band == 0) {
		*y0 = 0;
		*h = BENCH_BAND_H;
	} else if (band == 1) {
		*y0 = BENCH_BAND_H;
		*h = BENCH_BAND_H;
	} else {
		*y0 = BENCH_BAND_H * 2;
		*h = BENCH_BAND_LAST_H;
	}
}

static void mark_band_top(uint16_t *composite, int band, uint16_t color)
{
	int y0, h, x;
	int y;

	band_y_range(band, &y0, &h);
	(void)h;
	for (y = y0; y < y0 + 2 && y < FB_LCD_HEIGHT; y++) {
		uint16_t *row = composite + y * FB_LCD_WIDTH;

		for (x = 0; x < FB_LCD_WIDTH; x++)
			row[x] = color;
	}
}

static void fill_band_black(uint16_t *composite, int band)
{
	int y0, h, y;

	band_y_range(band, &y0, &h);
	for (y = y0; y < y0 + h; y++)
		memset(composite + y * FB_LCD_WIDTH, 0, FB_LCD_WIDTH * sizeof(uint16_t));
}

static void log_frame_once(const char *tag, const VIDEO_FRAME_S *vf, CVI_BOOL *logged)
{
	if (*logged)
		return;
	SAMPLE_PRT("%s: %ux%u stride[0]=%u stride[1]=%u fmt=%d\n",
		   tag, vf->u32Width, vf->u32Height,
		   vf->u32Stride[0], vf->u32Stride[1], vf->enPixelFormat);
	*logged = CVI_TRUE;
}

int main(int argc, char **argv)
{
	CVI_S32 s32Ret;
	FB_LCD_S fb_lcd;
	uint16_t *composite = NULL;
	uint16_t *band_buf = NULL;
	VIDEO_FRAME_INFO_S vi_frame, yuv_frame, rgb_frame;
	VPSS_CHN vpss_chn = VPSS_CHN0;
	void *vi_vir = NULL, *yuv_vir = NULL, *rgb_vir = NULL;
	size_t vi_map = 0, yuv_map = 0, rgb_map = 0;
	CVI_BOOL got_vi = CVI_FALSE, got_yuv = CVI_FALSE, got_rgb = CVI_FALSE;
	CVI_BOOL log_vi = CVI_FALSE, log_yuv = CVI_FALSE, log_rgb = CVI_FALSE;
	int frame_count = 0;
	int band_w = FB_LCD_WIDTH;
	int band_h0 = BENCH_BAND_H;
	int band_h1 = BENCH_BAND_H;
	int band_h2 = BENCH_BAND_LAST_H;

	(void)argc;
	(void)argv;
	setbuf(stdout, NULL);

	composite = calloc((size_t)PANEL_PIXELS, sizeof(uint16_t));
	band_buf = calloc((size_t)FB_LCD_WIDTH * BENCH_BAND_H, sizeof(uint16_t));
	if (!composite || !band_buf) {
		SAMPLE_PRT("alloc failed\n");
		free(composite);
		free(band_buf);
		return 1;
	}

	s32Ret = sys_mm_init();
	if (s32Ret != CVI_SUCCESS) {
		free(composite);
		free(band_buf);
		return 1;
	}

	if (fb_lcd_open(&fb_lcd) != 0) {
		sys_mm_deinit();
		free(composite);
		free(band_buf);
		return 1;
	}

	usleep(400 * 1000);
	SAMPLE_PRT("Running. Top=VI(red bar) Mid=VPSS-YUV(green) Bottom=VPSS-RGB(blue)\n");
	SAMPLE_PRT("Naive YUV/RGB conversion — no color workaround. Ctrl+C to exit.\n");

	while (g_running) {
		memset(composite, 0, (size_t)PANEL_PIXELS * sizeof(uint16_t));

		s32Ret = CVI_VI_GetChnFrame(g_vi_pipe, g_vi_chn, &vi_frame, 500);
		if (s32Ret == CVI_SUCCESS)
			got_vi = CVI_TRUE;

		s32Ret = CVI_VPSS_GetChnFrame(VPSS_GRP_ROT, vpss_chn, &yuv_frame, 500);
		if (s32Ret == CVI_SUCCESS)
			got_yuv = CVI_TRUE;

		s32Ret = CVI_VPSS_GetChnFrame(VPSS_GRP_CSC, vpss_chn, &rgb_frame, 500);
		if (s32Ret == CVI_SUCCESS)
			got_rgb = CVI_TRUE;

		if (got_vi) {
			if (map_frame(&vi_frame, &vi_vir, &vi_map) == CVI_SUCCESS) {
				log_frame_once("VI", &vi_frame.stVFrame, &log_vi);
				fill_band_black(composite, 0);
				if (bench_nv21_scale_to_rgb565(&vi_frame.stVFrame, band_buf,
							       band_w, band_h0) == 0) {
					memcpy(composite, band_buf,
					       (size_t)band_w * band_h0 * sizeof(uint16_t));
					mark_band_top(composite, 0, BENCH_MARK_VI);
				}
				unmap_frame(vi_vir, vi_map);
				vi_vir = NULL;
			}
			CVI_VI_ReleaseChnFrame(g_vi_pipe, g_vi_chn, &vi_frame);
			got_vi = CVI_FALSE;
		}

		if (got_yuv) {
			if (map_frame(&yuv_frame, &yuv_vir, &yuv_map) == CVI_SUCCESS) {
				log_frame_once("VPSS-YUV", &yuv_frame.stVFrame, &log_yuv);
				fill_band_black(composite, 1);
				if (bench_nv21_scale_to_rgb565(&yuv_frame.stVFrame, band_buf,
							       band_w, band_h1) == 0) {
					memcpy(composite + BENCH_BAND_H * FB_LCD_WIDTH, band_buf,
					       (size_t)band_w * band_h1 * sizeof(uint16_t));
					mark_band_top(composite, 1, BENCH_MARK_YUV);
				}
				unmap_frame(yuv_vir, yuv_map);
				yuv_vir = NULL;
			}
			CVI_VPSS_ReleaseChnFrame(VPSS_GRP_ROT, vpss_chn, &yuv_frame);
			got_yuv = CVI_FALSE;
		}

		if (got_rgb) {
			if (map_frame(&rgb_frame, &rgb_vir, &rgb_map) == CVI_SUCCESS) {
				log_frame_once("VPSS-RGB", &rgb_frame.stVFrame, &log_rgb);
				fill_band_black(composite, 2);
				if (bench_rgb888_scale_to_rgb565(&rgb_frame.stVFrame, band_buf,
								 band_w, band_h2) == 0) {
					memcpy(composite + BENCH_BAND_H * 2 * FB_LCD_WIDTH,
					       band_buf,
					       (size_t)band_w * band_h2 * sizeof(uint16_t));
					mark_band_top(composite, 2, BENCH_MARK_RGB);
				}
				unmap_frame(rgb_vir, rgb_map);
				rgb_vir = NULL;
			}
			CVI_VPSS_ReleaseChnFrame(VPSS_GRP_CSC, vpss_chn, &rgb_frame);
			got_rgb = CVI_FALSE;
		}

		bench_fb_draw_composite(&fb_lcd, composite, FB_LCD_WIDTH, FB_LCD_HEIGHT);
		frame_count++;
	}

	SAMPLE_PRT("Stopped after %d frames\n", frame_count);

	fb_lcd_clear(&fb_lcd, 0x0000);
	fb_lcd_close(&fb_lcd);
	sys_mm_deinit();
	free(composite);
	free(band_buf);
	return 0;
}
