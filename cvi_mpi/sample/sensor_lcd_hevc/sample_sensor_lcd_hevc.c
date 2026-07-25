#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <pthread.h>
#include <time.h>

#include "cvi_buffer.h"
#include "cvi_comm_isp.h"
#include "cvi_comm_sns.h"
#include "cvi_vb.h"
#include "cvi_vpss.h"
#include "sample_comm.h"
#include "fb_lcd.h"
#include "rgb888_rgb565.h"
#include "perf_stats.h"
#include "sys_status.h"
#include "hevc_recorder.h"
#include "touch_input.h"
#include "ui_overlay.h"

#define VPSS_ALIGN 64
#define VPSS_ALIGN_UP(x) ((((x) + VPSS_ALIGN - 1) / VPSS_ALIGN) * VPSS_ALIGN)
/* HEVC requires 8-pixel aligned picture size (see wave4 confWin). */
#define VENC_ALIGN 8
#define VENC_ALIGN_DOWN(x) ((x) & ~((CVI_U32)VENC_ALIGN - 1))
#define VENC_ALIGN_UP(x) VENC_ALIGN_DOWN((x) + VENC_ALIGN - 1)

/* 16:9 pre-rotation tiers (portrait 9:16 after ROT90); largest 9:16 first. */
typedef struct {
	CVI_U32 pre_w;
	CVI_U32 pre_h;
	const char *name;
} ENC_STD_RES_S;

static const ENC_STD_RES_S g_5m_enc_tiers[] = {
	/*
	 * Default: max **exact 9:16** that passes VPSS+VENC (pre_w must be <2880).
	 * 2880x1620 is 9:16 but broken; 2864x1611 exact 9:16 but VPSS init fails.
	 */
	{ 2848, 1602, "9:16-max" }, /* portrait 1602x2848, ~4.56MP */
	{ 2816, 1584, "QHD+" },      /* portrait 1584x2816, ~4.46MP */
	{ 2560, 1440, "2K/QHD" },    /* portrait 1440x2560, ~3.69MP */
	{ 1920, 1080, "1080p" },     /* portrait 1080x1920, fallback */
};

static void enc_pick_5m_tier(CVI_U32 sensor_w, CVI_U32 sensor_h,
			     CVI_U32 *pre_w, CVI_U32 *pre_h, const char **name)
{
	size_t i;

	for (i = 0; i < sizeof(g_5m_enc_tiers) / sizeof(g_5m_enc_tiers[0]); i++) {
		if (g_5m_enc_tiers[i].pre_w <= sensor_w &&
		    g_5m_enc_tiers[i].pre_h <= sensor_h) {
			*pre_w = g_5m_enc_tiers[i].pre_w;
			*pre_h = g_5m_enc_tiers[i].pre_h;
			if (name)
				*name = g_5m_enc_tiers[i].name;
			return;
		}
	}
	*pre_w = 1920;
	*pre_h = 1080;
	if (name)
		*name = "1080p";
}

/*
 * GDC rotation only supports NV12/NV21/YUV400 — not RGB888.
 * Grp0: Chn0 preview (fast path), Chn1 encode (on demand).
 * While recording, preview chn is disabled so encode gets full Grp0 bandwidth.
 */
#define VPSS_PRE_W 320
#define VPSS_PRE_H VPSS_ALIGN_UP(172)
#define VPSS_DISP_W VPSS_PRE_H
#define VPSS_DISP_H VPSS_PRE_W
#define VPSS_CSC_FMT PIXEL_FORMAT_RGB_888

#define VPSS_GRP_MAIN 0
#define VPSS_GRP_CSC  1
#define VPSS_CHN_PREV VPSS_CHN0
#define VPSS_CHN_ENC  VPSS_CHN1

/*
 * Portrait encode: VPSS scales to landscape then ROT90.
 * Channel attr is pre-rotation size; VENC uses post-rotation size.
 * Defaults follow active sensor size (not hardcoded 1080p).
 */
#define ENC_PIX_FMT       PIXEL_FORMAT_NV12

#define VPSS_RGB_PIXELS (VPSS_DISP_W * VPSS_DISP_H)

/* Recording: LCD shows last frame + REC HUD; preview VPSS paused for encode FPS. */
#define REC_HUD_REFRESH_MS 500
/* 1080p: defer encode VB until record (preview matches sensor_lcd 3-pool layout). */
#define ENC_VB_BLK_CNT_DEFER  2
#define ENC_VB_BLK_CNT_5MP    4

static SAMPLE_VI_CONFIG_S g_stViConfig;
static SIZE_S g_stSensorSize;
static CVI_S32 g_sensor_fps = HEVC_REC_DEFAULT_FPS;
static volatile sig_atomic_t g_running = 1;
static CVI_BOOL g_sys_inited = CVI_FALSE;
static CVI_BOOL g_vi_inited = CVI_FALSE;
static CVI_BOOL g_vpss0_started = CVI_FALSE;
static CVI_BOOL g_vpss1_started = CVI_FALSE;
static CVI_BOOL g_vi_vpss_bound = CVI_FALSE;
static CVI_BOOL g_vpss_vpss_bound = CVI_FALSE;
static CVI_BOOL g_preview_suspended = CVI_FALSE;
static CVI_BOOL g_vpss0_chn_enabled[VPSS_MAX_PHY_CHN_NUM] = {0};
static CVI_BOOL g_vpss1_chn_enabled[VPSS_MAX_PHY_CHN_NUM] = {0};
static CVI_BOOL g_frame_held = CVI_FALSE;
static VIDEO_FRAME_INFO_S g_held_frame;

static VPSS_CHN_ATTR_S g_enc_chn_attr;
static CVI_U32 g_enc_pre_w; /* VPSS chn size before ROT90 */
static CVI_U32 g_enc_pre_h;
static CVI_U32 g_enc_w;     /* VENC size after ROT90 */
static CVI_U32 g_enc_h;
static CVI_BOOL g_mirror;
static CVI_BOOL g_flip;
static CVI_BOOL g_enc_rot90; /* false for 5MP: VPSS ROT90 at full res breaks VENC bind */
static CVI_U32 g_enc_override_w; /* 0 = auto tier pick (5MP path) */
static CVI_U32 g_enc_override_h;
static CVI_U32 g_enc_vb_blk_size;
static CVI_BOOL g_enc_vb_defer; /* CVI_TRUE: no comm pool[3] at preview init */
static VB_POOL g_enc_dyn_pool = VB_INVALID_POOLID;

static HEVC_RECORDER_S g_recorder;
static UI_OVERLAY_S g_ui;
static TOUCH_INPUT_S g_touch;
static pthread_t g_touch_tid;
static volatile int g_touch_running;

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

static void vpss_try_destroy(VPSS_GRP grp)
{
	CVI_S32 j;

	for (j = 0; j < VPSS_MAX_PHY_CHN_NUM; j++)
		CVI_VPSS_DisableChn(grp, j);
	CVI_VPSS_StopGrp(grp);
	CVI_VPSS_DestroyGrp(grp);
}

static void sys_mm_deinit(void);

static CVI_S32 enc_vb_pool_ensure(void)
{
	VB_POOL_CONFIG_S cfg;

	if (!g_enc_vb_defer)
		return CVI_SUCCESS;
	if (g_enc_dyn_pool != VB_INVALID_POOLID)
		return CVI_SUCCESS;
	if (g_enc_vb_blk_size == 0)
		return CVI_FAILURE;

	memset(&cfg, 0, sizeof(cfg));
	cfg.u32BlkSize = g_enc_vb_blk_size;
	cfg.u32BlkCnt = ENC_VB_BLK_CNT_DEFER;
	cfg.enRemapMode = VB_REMAP_MODE_CACHED;
	g_enc_dyn_pool = CVI_VB_CreatePool(&cfg);
	if (g_enc_dyn_pool == VB_INVALID_POOLID) {
		SAMPLE_PRT("encode VB CreatePool failed size=%u\n", g_enc_vb_blk_size);
		return CVI_FAILURE;
	}
	SAMPLE_PRT("encode VB pool created: id=%u size=%u cnt=%u\n",
		   g_enc_dyn_pool, g_enc_vb_blk_size, ENC_VB_BLK_CNT_DEFER);
	return CVI_SUCCESS;
}

static void enc_vb_pool_release(void)
{
	if (g_enc_dyn_pool != VB_INVALID_POOLID) {
		CVI_VB_DestroyPool(g_enc_dyn_pool);
		g_enc_dyn_pool = VB_INVALID_POOLID;
	}
}

static CVI_S32 vpss_preview_suspend(void)
{
	CVI_S32 s32Ret;

	if (g_preview_suspended)
		return CVI_SUCCESS;

	if (g_frame_held) {
		CVI_VPSS_ReleaseChnFrame(VPSS_GRP_CSC, VPSS_CHN0, &g_held_frame);
		g_frame_held = CVI_FALSE;
	}
	if (g_vpss_vpss_bound) {
		SAMPLE_COMM_VPSS_UnBind_VPSS(VPSS_GRP_MAIN, VPSS_CHN_PREV, VPSS_GRP_CSC);
		g_vpss_vpss_bound = CVI_FALSE;
	}
	s32Ret = CVI_VPSS_DisableChn(VPSS_GRP_MAIN, VPSS_CHN_PREV);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("DisableChn preview failed: %#x\n", s32Ret);
		return s32Ret;
	}
	g_vpss0_chn_enabled[VPSS_CHN_PREV] = CVI_FALSE;
	g_preview_suspended = CVI_TRUE;
	SAMPLE_PRT("Preview VPSS paused for recording\n");
	return CVI_SUCCESS;
}

static CVI_S32 vpss_preview_resume(void)
{
	CVI_S32 s32Ret;

	if (!g_preview_suspended)
		return CVI_SUCCESS;

	s32Ret = CVI_VPSS_EnableChn(VPSS_GRP_MAIN, VPSS_CHN_PREV);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("EnableChn preview failed: %#x\n", s32Ret);
		return s32Ret;
	}
	g_vpss0_chn_enabled[VPSS_CHN_PREV] = CVI_TRUE;

	if (!g_vpss_vpss_bound) {
		s32Ret = SAMPLE_COMM_VPSS_Bind_VPSS(VPSS_GRP_MAIN, VPSS_CHN_PREV,
						    VPSS_GRP_CSC);
		if (s32Ret != CVI_SUCCESS) {
			SAMPLE_PRT("Rebind preview VPSS failed: %#x\n", s32Ret);
			return s32Ret;
		}
		g_vpss_vpss_bound = CVI_TRUE;
	}
	g_preview_suspended = CVI_FALSE;
	SAMPLE_PRT("Preview VPSS resumed\n");
	return CVI_SUCCESS;
}

static CVI_S32 vpss_enable_enc_chn(void *ctx)
{
	CVI_S32 s32Ret;
	(void)ctx;

	s32Ret = enc_vb_pool_ensure();
	if (s32Ret != CVI_SUCCESS)
		return s32Ret;

	s32Ret = vpss_preview_suspend();
	if (s32Ret != CVI_SUCCESS)
		return s32Ret;

	s32Ret = CVI_VPSS_SetChnAttr(VPSS_GRP_MAIN, VPSS_CHN_ENC, &g_enc_chn_attr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("SetChnAttr enc failed: %#x\n", s32Ret);
		goto fail_resume;
	}
	if (g_enc_rot90) {
		s32Ret = CVI_VPSS_SetChnRotation(VPSS_GRP_MAIN, VPSS_CHN_ENC, ROTATION_90);
		if (s32Ret != CVI_SUCCESS) {
			SAMPLE_PRT("SetChnRotation enc failed: %#x\n", s32Ret);
			goto fail_resume;
		}
	} else {
		s32Ret = CVI_VPSS_SetChnRotation(VPSS_GRP_MAIN, VPSS_CHN_ENC, ROTATION_0);
		if (s32Ret != CVI_SUCCESS) {
			SAMPLE_PRT("SetChnRotation enc failed: %#x\n", s32Ret);
			goto fail_resume;
		}
	}
	s32Ret = CVI_VPSS_EnableChn(VPSS_GRP_MAIN, VPSS_CHN_ENC);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("EnableChn enc failed: %#x\n", s32Ret);
		goto fail_resume;
	}
	g_vpss0_chn_enabled[VPSS_CHN_ENC] = CVI_TRUE;
	if (g_enc_rot90)
		SAMPLE_PRT("VPSS Grp0 Chn1 encode: %ux%u ROT90 -> %ux%u NV12\n",
			   g_enc_pre_w, g_enc_pre_h, g_enc_w, g_enc_h);
	else
		SAMPLE_PRT("VPSS Grp0 Chn1 encode: %ux%u landscape NV12\n",
			   g_enc_w, g_enc_h);
	return CVI_SUCCESS;

fail_resume:
	vpss_preview_resume();
	return s32Ret;
}

static CVI_S32 vpss_disable_enc_chn(void *ctx)
{
	CVI_S32 s32Ret;
	(void)ctx;

	s32Ret = CVI_VPSS_DisableChn(VPSS_GRP_MAIN, VPSS_CHN_ENC);
	if (s32Ret != CVI_SUCCESS)
		SAMPLE_PRT("DisableChn enc failed: %#x\n", s32Ret);
	g_vpss0_chn_enabled[VPSS_CHN_ENC] = CVI_FALSE;
	enc_vb_pool_release();
	vpss_preview_resume();
	SAMPLE_PRT("VPSS Grp0 Chn1 (encode) disabled\n");
	return CVI_SUCCESS;
}

static CVI_S32 sys_mm_init(CVI_BOOL mirror, CVI_BOOL flip)
{
	MMF_VERSION_S stVersion;
	SAMPLE_INI_CFG_S stIniCfg;
	SAMPLE_VI_CONFIG_S stViConfig;
	CVI_S32 s32Ret;
	LOG_LEVEL_CONF_S log_conf;
	VB_CONFIG_S stVbConf;
	CVI_U32 u32ViBlk, u32ViRotBlk, u32Nv21Blk, u32Nv21RotBlk, u32RgbBlk, u32EncBlk;
	VPSS_GRP_ATTR_S stVpssGrpAttr;
	VPSS_CHN_ATTR_S astVpssChnAttr[VPSS_MAX_PHY_CHN_NUM];
	CVI_BOOL abChnEnable[VPSS_MAX_PHY_CHN_NUM] = {0};
	struct sigaction sa;
	SAMPLE_SNS_MODE_INFO_S stModeInfo;

	g_mirror = mirror;
	g_flip = flip;

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

	s32Ret = SAMPLE_COMM_SNS_QueryActiveMode(&stIniCfg, &stModeInfo);
	if (s32Ret != CVI_SUCCESS)
		return s32Ret;
	g_stSensorSize = stModeInfo.stSize;
	if (stModeInfo.f32Fps > 0.01f)
		g_sensor_fps = (CVI_S32)(stModeInfo.f32Fps + 0.5f);
	else
		g_sensor_fps = HEVC_REC_DEFAULT_FPS;
	SAMPLE_PRT("Sensor mode=%s size=%ux%u fps=%d raw=%ubit snsMode=%u bin=%s\n",
		   stModeInfo.pszModeName,
		   stModeInfo.stSize.u32Width, stModeInfo.stSize.u32Height,
		   g_sensor_fps,
		   stModeInfo.u8RawBitDepth, stModeInfo.u8SnsMode,
		   stModeInfo.pszIspBinPath ? stModeInfo.pszIspBinPath : "(default)");

	CVI_VI_SetDevNum(stIniCfg.devNum);

	s32Ret = SAMPLE_COMM_VI_IniToViCfg(&stIniCfg, &stViConfig);
	if (s32Ret != CVI_SUCCESS)
		return s32Ret;

	memcpy(&g_stViConfig, &stViConfig, sizeof(SAMPLE_VI_CONFIG_S));

	/*
	 * Portrait encode via ROT90 works for 1080p. At 5MP (2880x1620) full-res
	 * VPSS rotation + VENC bind produces corrupt frames on SG2000; use a
	 * portrait downscale that keeps 8px-aligned VENC size instead.
	 */
	if (g_stSensorSize.u32Width > 1920 || g_stSensorSize.u32Height > 1080) {
		const char *tier_name = NULL;

		/*
		 * 5MP: full-res VPSS ROT90 + VENC is broken on SG2000.
		 * Pick the largest standard 16:9 tier that fits the sensor.
		 */
		g_enc_rot90 = CVI_TRUE;
		if (g_enc_override_w > 0 && g_enc_override_h > 0) {
			g_enc_pre_w = g_enc_override_w;
			g_enc_pre_h = g_enc_override_h;
			tier_name = "override";
		} else {
			enc_pick_5m_tier(g_stSensorSize.u32Width, g_stSensorSize.u32Height,
					 &g_enc_pre_w, &g_enc_pre_h, &tier_name);
		}
		g_enc_w = g_enc_pre_h;
		g_enc_h = g_enc_pre_w;
		SAMPLE_PRT("5MP encode [%s]: downscale %ux%u -> %ux%u ROT90 -> %ux%u portrait\n",
			   tier_name ? tier_name : "?",
			   g_stSensorSize.u32Width, g_stSensorSize.u32Height,
			   g_enc_pre_w, g_enc_pre_h, g_enc_w, g_enc_h);
	} else {
		g_enc_rot90 = CVI_TRUE;
		g_enc_pre_w = VENC_ALIGN_DOWN(g_stSensorSize.u32Width);
		g_enc_pre_h = VENC_ALIGN_DOWN(g_stSensorSize.u32Height);
		if (g_enc_pre_w == 0)
			g_enc_pre_w = g_stSensorSize.u32Width;
		if (g_enc_pre_h == 0)
			g_enc_pre_h = g_stSensorSize.u32Height;
		g_enc_w = g_enc_pre_h;
		g_enc_h = g_enc_pre_w;
		SAMPLE_PRT("Sensor %ux%u, encode %ux%u ROT90 -> %ux%u (portrait)\n",
			   g_stSensorSize.u32Width, g_stSensorSize.u32Height,
			   g_enc_pre_w, g_enc_pre_h, g_enc_w, g_enc_h);
	}

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
	/* Encode VB must cover pre- and post-rotation sizes (same pixel count for 90°). */
	u32EncBlk = vb_pool_blk_size(g_enc_pre_w, g_enc_pre_h, ENC_PIX_FMT);
	{
		CVI_U32 u32EncRot = vb_pool_blk_size(g_enc_w, g_enc_h, ENC_PIX_FMT);

		if (u32EncRot > u32EncBlk)
			u32EncBlk = u32EncRot;
	}

	g_enc_vb_blk_size = u32EncBlk;
	/* 1080p bin: skip encode pool at preview init to match sensor_lcd memory footprint. */
	g_enc_vb_defer = (g_stSensorSize.u32Width <= 1920 &&
			  g_stSensorSize.u32Height <= 1080) ? CVI_TRUE : CVI_FALSE;

	stVbConf.u32MaxPoolCnt = g_enc_vb_defer ? 3 : 4;
	stVbConf.astCommPool[0].u32BlkSize = u32ViBlk;
	stVbConf.astCommPool[0].u32BlkCnt = 5;
	stVbConf.astCommPool[0].enRemapMode = VB_REMAP_MODE_CACHED;
	stVbConf.astCommPool[1].u32BlkSize = u32Nv21Blk;
	stVbConf.astCommPool[1].u32BlkCnt = 5;
	stVbConf.astCommPool[1].enRemapMode = VB_REMAP_MODE_CACHED;
	stVbConf.astCommPool[2].u32BlkSize = u32RgbBlk;
	stVbConf.astCommPool[2].u32BlkCnt = 4;
	stVbConf.astCommPool[2].enRemapMode = VB_REMAP_MODE_CACHED;
	if (!g_enc_vb_defer) {
		stVbConf.astCommPool[3].u32BlkSize = u32EncBlk;
		stVbConf.astCommPool[3].u32BlkCnt = ENC_VB_BLK_CNT_5MP;
		stVbConf.astCommPool[3].enRemapMode = VB_REMAP_MODE_CACHED;
	}

	SAMPLE_PRT("VB pool[0] sensor size=%u cnt=5\n", u32ViBlk);
	SAMPLE_PRT("VB pool[1] preview NV21 size=%u cnt=5\n", u32Nv21Blk);
	SAMPLE_PRT("VB pool[2] RGB888 size=%u cnt=4\n", u32RgbBlk);
	if (g_enc_vb_defer)
		SAMPLE_PRT("VB encode NV12 %ux%u->%ux%u size=%u deferred (on record)\n",
			   g_enc_pre_w, g_enc_pre_h, g_enc_w, g_enc_h, u32EncBlk);
	else
		SAMPLE_PRT("VB pool[3] encode NV12 %ux%u->%ux%u size=%u cnt=%u\n",
			   g_enc_pre_w, g_enc_pre_h, g_enc_w, g_enc_h, u32EncBlk,
			   ENC_VB_BLK_CNT_5MP);

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
		g_sys_inited = CVI_FALSE;
		return s32Ret;
	}
	g_vi_inited = CVI_TRUE;

	vpss_try_destroy(VPSS_GRP_MAIN);
	vpss_try_destroy(VPSS_GRP_CSC);

	/* ---- Grp0: preview only (same single-chn layout as sample_sensor_lcd) ---- */
	memset(&stVpssGrpAttr, 0, sizeof(stVpssGrpAttr));
	memset(astVpssChnAttr, 0, sizeof(astVpssChnAttr));
	memset(abChnEnable, 0, sizeof(abChnEnable));

	stVpssGrpAttr.stFrameRate.s32SrcFrameRate = -1;
	stVpssGrpAttr.stFrameRate.s32DstFrameRate = -1;
	stVpssGrpAttr.enPixelFormat = SAMPLE_PIXEL_FORMAT;
	stVpssGrpAttr.u32MaxW = g_stSensorSize.u32Width;
	stVpssGrpAttr.u32MaxH = g_stSensorSize.u32Height;
	stVpssGrpAttr.u8VpssDev = 0;

	/* Encode attrs for Grp0 Chn1 (enabled on record; preview chn paused meanwhile). */
	memset(&g_enc_chn_attr, 0, sizeof(g_enc_chn_attr));
	g_enc_chn_attr.u32Width = g_enc_pre_w;
	g_enc_chn_attr.u32Height = g_enc_pre_h;
	g_enc_chn_attr.enVideoFormat = VIDEO_FORMAT_LINEAR;
	g_enc_chn_attr.enPixelFormat = ENC_PIX_FMT; /* NV12: avoids UV-swap vs NV21 in VENC */
	g_enc_chn_attr.stFrameRate.s32SrcFrameRate = -1;
	g_enc_chn_attr.stFrameRate.s32DstFrameRate = -1;
	g_enc_chn_attr.u32Depth = 0; /* hardware bind to VENC */
	g_enc_chn_attr.bMirror = mirror;
	g_enc_chn_attr.bFlip = flip;
	g_enc_chn_attr.stAspectRatio.enMode = ASPECT_RATIO_NONE;
	g_enc_chn_attr.stAspectRatio.bEnableBgColor = CVI_FALSE;
	g_enc_chn_attr.stNormalize.bEnable = CVI_FALSE;

	/* Chn0 preview */
	astVpssChnAttr[VPSS_CHN_PREV].u32Width = VPSS_PRE_W;
	astVpssChnAttr[VPSS_CHN_PREV].u32Height = VPSS_PRE_H;
	astVpssChnAttr[VPSS_CHN_PREV].enVideoFormat = VIDEO_FORMAT_LINEAR;
	astVpssChnAttr[VPSS_CHN_PREV].enPixelFormat = SAMPLE_PIXEL_FORMAT;
	astVpssChnAttr[VPSS_CHN_PREV].stFrameRate.s32SrcFrameRate = -1;
	astVpssChnAttr[VPSS_CHN_PREV].stFrameRate.s32DstFrameRate = -1;
	astVpssChnAttr[VPSS_CHN_PREV].u32Depth = 1;
	astVpssChnAttr[VPSS_CHN_PREV].bMirror = mirror;
	astVpssChnAttr[VPSS_CHN_PREV].bFlip = flip;
	astVpssChnAttr[VPSS_CHN_PREV].stAspectRatio.enMode = ASPECT_RATIO_AUTO;
	astVpssChnAttr[VPSS_CHN_PREV].stAspectRatio.bEnableBgColor = CVI_TRUE;
	astVpssChnAttr[VPSS_CHN_PREV].stAspectRatio.u32BgColor = COLOR_RGB_BLACK;
	astVpssChnAttr[VPSS_CHN_PREV].stNormalize.bEnable = CVI_FALSE;

	abChnEnable[VPSS_CHN_PREV] = CVI_TRUE;
	memcpy(g_vpss0_chn_enabled, abChnEnable, sizeof(g_vpss0_chn_enabled));

	s32Ret = SAMPLE_COMM_VPSS_Init(VPSS_GRP_MAIN, abChnEnable, &stVpssGrpAttr,
				       astVpssChnAttr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vpss grp0 init failed: %#x\n", s32Ret);
		goto fail;
	}
	g_vpss0_started = CVI_TRUE;

	s32Ret = SAMPLE_COMM_VPSS_Start(VPSS_GRP_MAIN, abChnEnable, &stVpssGrpAttr,
					astVpssChnAttr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vpss grp0 start failed: %#x\n", s32Ret);
		goto fail;
	}

	s32Ret = CVI_VPSS_SetChnRotation(VPSS_GRP_MAIN, VPSS_CHN_PREV, ROTATION_90);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vpss grp0 chn0 rotation failed: %#x\n", s32Ret);
		goto fail;
	}

	/* ---- Grp1: HW CSC ---- */
	memset(&stVpssGrpAttr, 0, sizeof(stVpssGrpAttr));
	memset(astVpssChnAttr, 0, sizeof(astVpssChnAttr));
	memset(abChnEnable, 0, sizeof(abChnEnable));

	stVpssGrpAttr.stFrameRate.s32SrcFrameRate = -1;
	stVpssGrpAttr.stFrameRate.s32DstFrameRate = -1;
	stVpssGrpAttr.enPixelFormat = SAMPLE_PIXEL_FORMAT;
	stVpssGrpAttr.u32MaxW = VPSS_DISP_W;
	stVpssGrpAttr.u32MaxH = VPSS_DISP_H;
	stVpssGrpAttr.u8VpssDev = 0;

	astVpssChnAttr[VPSS_CHN0].u32Width = VPSS_DISP_W;
	astVpssChnAttr[VPSS_CHN0].u32Height = VPSS_DISP_H;
	astVpssChnAttr[VPSS_CHN0].enVideoFormat = VIDEO_FORMAT_LINEAR;
	astVpssChnAttr[VPSS_CHN0].enPixelFormat = VPSS_CSC_FMT;
	astVpssChnAttr[VPSS_CHN0].stFrameRate.s32SrcFrameRate = -1;
	astVpssChnAttr[VPSS_CHN0].stFrameRate.s32DstFrameRate = -1;
	astVpssChnAttr[VPSS_CHN0].u32Depth = 1;
	astVpssChnAttr[VPSS_CHN0].bMirror = CVI_FALSE;
	astVpssChnAttr[VPSS_CHN0].bFlip = CVI_FALSE;
	astVpssChnAttr[VPSS_CHN0].stAspectRatio.enMode = ASPECT_RATIO_NONE;
	astVpssChnAttr[VPSS_CHN0].stAspectRatio.bEnableBgColor = CVI_FALSE;
	astVpssChnAttr[VPSS_CHN0].stNormalize.bEnable = CVI_FALSE;

	abChnEnable[VPSS_CHN0] = CVI_TRUE;
	memcpy(g_vpss1_chn_enabled, abChnEnable, sizeof(g_vpss1_chn_enabled));

	s32Ret = SAMPLE_COMM_VPSS_Init(VPSS_GRP_CSC, abChnEnable, &stVpssGrpAttr,
				       astVpssChnAttr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vpss grp1 init failed: %#x\n", s32Ret);
		goto fail;
	}
	g_vpss1_started = CVI_TRUE;

	s32Ret = SAMPLE_COMM_VPSS_Start(VPSS_GRP_CSC, abChnEnable, &stVpssGrpAttr,
					astVpssChnAttr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vpss grp1 start failed: %#x\n", s32Ret);
		goto fail;
	}

	s32Ret = SAMPLE_COMM_VI_Bind_VPSS(0, 0, VPSS_GRP_MAIN);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vi bind vpss0 failed: %#x\n", s32Ret);
		goto fail;
	}
	g_vi_vpss_bound = CVI_TRUE;

	s32Ret = SAMPLE_COMM_VPSS_Bind_VPSS(VPSS_GRP_MAIN, VPSS_CHN_PREV, VPSS_GRP_CSC);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("vpss0 chn1 bind vpss1 failed: %#x\n", s32Ret);
		goto fail;
	}
	g_vpss_vpss_bound = CVI_TRUE;

	SAMPLE_PRT("Pipeline ready: sensor %ux%u\n",
		   g_stSensorSize.u32Width, g_stSensorSize.u32Height);
	SAMPLE_PRT("  Grp0 Chn0 %dx%d ROT90 NV21 -> Grp1 %dx%d RGB888 -> LCD %dx%d\n",
		   VPSS_PRE_W, VPSS_PRE_H, VPSS_DISP_W, VPSS_DISP_H,
		   FB_LCD_WIDTH, FB_LCD_HEIGHT);
	SAMPLE_PRT("  Grp0 Chn1 %ux%u%s NV12 -> VENC H265 %ux%u (on-demand; preview paused)\n",
		   g_enc_pre_w, g_enc_pre_h, g_enc_rot90 ? " ROT90" : "",
		   g_enc_w, g_enc_h);

	return CVI_SUCCESS;

fail:
	sys_mm_deinit();
	return s32Ret;
}

static void sys_mm_deinit(void)
{
	enc_vb_pool_release();

	if (g_frame_held) {
		CVI_VPSS_ReleaseChnFrame(VPSS_GRP_CSC, VPSS_CHN0, &g_held_frame);
		g_frame_held = CVI_FALSE;
	}

	if (g_vpss_vpss_bound) {
		SAMPLE_COMM_VPSS_UnBind_VPSS(VPSS_GRP_MAIN, VPSS_CHN_PREV, VPSS_GRP_CSC);
		g_vpss_vpss_bound = CVI_FALSE;
	}
	if (g_vi_vpss_bound) {
		SAMPLE_COMM_VI_UnBind_VPSS(0, 0, VPSS_GRP_MAIN);
		g_vi_vpss_bound = CVI_FALSE;
	}
	if (g_vpss0_chn_enabled[VPSS_CHN_ENC])
		CVI_VPSS_DisableChn(VPSS_GRP_MAIN, VPSS_CHN_ENC);
	if (g_preview_suspended)
		vpss_preview_resume();
	if (g_vpss1_started) {
		SAMPLE_COMM_VPSS_Stop(VPSS_GRP_CSC, g_vpss1_chn_enabled);
		g_vpss1_started = CVI_FALSE;
		memset(g_vpss1_chn_enabled, 0, sizeof(g_vpss1_chn_enabled));
	} else {
		vpss_try_destroy(VPSS_GRP_CSC);
	}
	if (g_vpss0_started) {
		SAMPLE_COMM_VPSS_Stop(VPSS_GRP_MAIN, g_vpss0_chn_enabled);
		g_vpss0_started = CVI_FALSE;
		memset(g_vpss0_chn_enabled, 0, sizeof(g_vpss0_chn_enabled));
	} else {
		vpss_try_destroy(VPSS_GRP_MAIN);
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

static void *touch_thread(void *arg)
{
	(void)arg;

	while (g_touch_running && g_running) {
		TOUCH_EVT_S evt;
		ui_cmd_e cmd;

		if (touch_input_poll(&g_touch, &evt) != TOUCH_EVT_NONE) {
			cmd = ui_overlay_handle_touch(&g_ui, &evt);
			if (cmd == UI_CMD_TOGGLE_RECORD) {
				if (hevc_recorder_is_recording(&g_recorder)) {
					hevc_recorder_stop(&g_recorder);
					ui_overlay_set_recording(&g_ui, false, 0);
				} else {
					if (hevc_recorder_start(&g_recorder) == 0)
						ui_overlay_set_recording(&g_ui, true, 0);
					else
						SAMPLE_PRT("start recording failed\n");
				}
			}
		}
		usleep(10 * 1000);
	}
	return NULL;
}

static void print_usage(const char *prog)
{
	printf("Usage: %s [options]\n", prog);
	printf("  -m          Enable VPSS horizontal mirror\n");
	printf("  -f          Enable VPSS vertical flip\n");
	printf("  -b <kbps>   H265 CBR bitrate (default %d)\n", HEVC_REC_DEFAULT_BITRATE);
	printf("  -o <dir>    Output directory (default %s, fallback %s)\n",
	       HEVC_REC_DEFAULT_DIR, HEVC_REC_FALLBACK_DIR);
	printf("  -r <sec>    Auto-record for N seconds then exit (headless test)\n");
	printf("  -e <WxH>    Force 5MP encode downscale (e.g. 2816x1584, test only)\n");
	printf("  -h          Show this help\n");
	printf("\nGestures:\n");
	printf("  Swipe up    Show half-screen menu (Record/Stop)\n");
	printf("  Swipe down / tap upper half   Hide menu\n");
	printf("\nRequires /mnt/data/sensor_cfg.ini (use imx678-mode 1080p|5m).\n");
	printf("Stop other /dev/fb0 users (e.g. screen_demo.py) before running.\n");
}

int main(int argc, char **argv)
{
	CVI_S32 s32Ret;
	FB_LCD_S fb_lcd;
	uint16_t *rgb_buf = NULL;
	VIDEO_FRAME_INFO_S stFrame;
	CVI_BOOL mirror = CVI_FALSE;
	CVI_BOOL flip = CVI_FALSE;
	int bitrate = HEVC_REC_DEFAULT_BITRATE;
	const char *out_dir = NULL;
	int auto_rec_sec = 0;
	int opt;
	int frame_count = 0;
	struct timespec rec_hud_last;
	struct timespec frame_loop_start, frame_loop_end;
	struct timespec step_start, step_end;
	struct timespec perf_report_start;
	struct timespec auto_rec_deadline;
	SYS_STATUS_S sys_status;
	CVI_BOOL first_frame_logged = CVI_FALSE;
	CVI_BOOL auto_rec_started = CVI_FALSE;
	HEVC_RECORDER_CFG_S rec_cfg;

	while ((opt = getopt(argc, argv, "mfb:o:r:e:h")) != -1) {
		switch (opt) {
		case 'm':
			mirror = CVI_TRUE;
			break;
		case 'f':
			flip = CVI_TRUE;
			break;
		case 'b':
			bitrate = atoi(optarg);
			if (bitrate <= 0)
				bitrate = HEVC_REC_DEFAULT_BITRATE;
			break;
		case 'o':
			out_dir = optarg;
			break;
		case 'r':
			auto_rec_sec = atoi(optarg);
			if (auto_rec_sec < 0)
				auto_rec_sec = 0;
			break;
		case 'e': {
			unsigned w = 0, h = 0;

			if (sscanf(optarg, "%ux%u", &w, &h) == 2 && w > 0 && h > 0) {
				g_enc_override_w = w;
				g_enc_override_h = h;
			} else {
				SAMPLE_PRT("invalid -e format, use WxH e.g. 2560x1440\n");
				return 1;
			}
			break;
		}
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

	ui_overlay_init(&g_ui);

	memset(&rec_cfg, 0, sizeof(rec_cfg));
	rec_cfg.width = g_enc_w;
	rec_cfg.height = g_enc_h;
	/* Scale default bitrate with encode pixels (ref: 1080x1920 portrait). */
	if (bitrate == HEVC_REC_DEFAULT_BITRATE) {
		CVI_U64 enc_pixels = (CVI_U64)g_enc_w * (CVI_U64)g_enc_h;
		CVI_U64 ref_pixels = 1080ULL * 1920ULL;

		rec_cfg.bitrate_kbps = (CVI_S32)((HEVC_REC_DEFAULT_BITRATE * enc_pixels) / ref_pixels);
		if (rec_cfg.bitrate_kbps < HEVC_REC_DEFAULT_BITRATE)
			rec_cfg.bitrate_kbps = HEVC_REC_DEFAULT_BITRATE;
	} else {
		rec_cfg.bitrate_kbps = bitrate;
	}
	rec_cfg.fps = g_sensor_fps > 0 ? g_sensor_fps : HEVC_REC_DEFAULT_FPS;
	rec_cfg.gop = rec_cfg.fps; /* keep ~1s GOP when fps changes */
	rec_cfg.vpss_grp = VPSS_GRP_MAIN;
	rec_cfg.vpss_chn = VPSS_CHN_ENC;
	rec_cfg.out_dir = out_dir;
	hevc_recorder_init(&g_recorder, &rec_cfg, vpss_enable_enc_chn,
			   vpss_disable_enc_chn, NULL);

	if (sys_status_start() != 0)
		SAMPLE_PRT("warn: status thread failed, HUD may be stale\n");

	if (touch_input_open(&g_touch, TOUCH_DEV_DEFAULT, fb_lcd.width, fb_lcd.height) == 0) {
		g_touch_running = 1;
		if (pthread_create(&g_touch_tid, NULL, touch_thread, NULL) != 0) {
			SAMPLE_PRT("warn: touch thread failed\n");
			g_touch_running = 0;
			touch_input_close(&g_touch);
		}
	} else {
		SAMPLE_PRT("warn: touch unavailable; menu gestures disabled\n");
	}

	usleep(500 * 1000);
	perf_timespec_now(&perf_report_start);
	perf_timespec_now(&rec_hud_last);

	if (auto_rec_sec > 0) {
		SAMPLE_PRT("Auto-record mode: %d seconds then exit.\n", auto_rec_sec);
		if (hevc_recorder_start(&g_recorder) == 0) {
			auto_rec_started = CVI_TRUE;
			ui_overlay_set_recording(&g_ui, true, 0);
			clock_gettime(CLOCK_MONOTONIC, &auto_rec_deadline);
			auto_rec_deadline.tv_sec += auto_rec_sec;
		} else {
			SAMPLE_PRT("auto-record start failed\n");
			g_running = 0;
		}
	} else {
		SAMPLE_PRT("Preview started. Swipe up for Record menu. Ctrl+C to exit.\n");
		SAMPLE_PRT("Perf stats every %ds (temporary instrumentation).\n",
			   PERF_REPORT_INTERVAL_SEC);
	}

	while (g_running) {
		void *vir_addr = NULL;
		size_t map_size = 0;
		bool recording;
		int elapsed;

		perf_timespec_now(&frame_loop_start);

		if (auto_rec_started) {
			struct timespec now;

			clock_gettime(CLOCK_MONOTONIC, &now);
			if (now.tv_sec > auto_rec_deadline.tv_sec ||
			    (now.tv_sec == auto_rec_deadline.tv_sec &&
			     now.tv_nsec >= auto_rec_deadline.tv_nsec)) {
				SAMPLE_PRT("Auto-record duration reached, stopping.\n");
				g_running = 0;
				break;
			}
		}

		recording = hevc_recorder_is_recording(&g_recorder);
		if (recording) {
			struct timespec hud_now;

			elapsed = hevc_recorder_elapsed_sec(&g_recorder);
			ui_overlay_set_recording(&g_ui, true, elapsed);
			clock_gettime(CLOCK_MONOTONIC, &hud_now);
			if (perf_elapsed_ns(&rec_hud_last, &hud_now) >=
			    (uint64_t)REC_HUD_REFRESH_MS * 1000000ULL) {
				sys_status_get(&sys_status);
				ui_overlay_draw(&g_ui, &fb_lcd,
						sys_status.battery_valid,
						sys_status.battery_pct,
						sys_status.temp_valid,
						sys_status.temp_c);
				rec_hud_last = hud_now;
			}
			usleep(20 * 1000);
			continue;
		}

		perf_timespec_now(&step_start);
		s32Ret = CVI_VPSS_GetChnFrame(VPSS_GRP_CSC, VPSS_CHN0, &stFrame, 1000);
		perf_timespec_now(&step_end);
		if (s32Ret != CVI_SUCCESS)
			continue;
		perf_record(PERF_VPSS_GET_FRAME, perf_elapsed_ns(&step_start, &step_end));

		g_held_frame = stFrame;
		g_frame_held = CVI_TRUE;

		if (!first_frame_logged) {
			SAMPLE_PRT("VPSS frame %ux%u stride=%u fmt=%d\n",
				   stFrame.stVFrame.u32Width, stFrame.stVFrame.u32Height,
				   stFrame.stVFrame.u32Stride[0],
				   stFrame.stVFrame.enPixelFormat);
			first_frame_logged = CVI_TRUE;
		}

		perf_timespec_now(&step_start);
		if (map_vpss_frame(&stFrame, &vir_addr, &map_size) == CVI_SUCCESS) {
			CVI_U32 fw = stFrame.stVFrame.u32Width;
			CVI_U32 fh = stFrame.stVFrame.u32Height;

			perf_timespec_now(&step_end);
			perf_record(PERF_VPSS_MAP, perf_elapsed_ns(&step_start, &step_end));

			if (fw > 0 && fh > 0 && fw * fh <= VPSS_RGB_PIXELS) {
				perf_timespec_now(&step_start);
				if (rgb888_frame_to_rgb565(&stFrame.stVFrame, rgb_buf) == 0) {
					perf_timespec_now(&step_end);
					perf_record(PERF_RGB888_RGB565,
						      perf_elapsed_ns(&step_start, &step_end));

					fb_lcd_draw_rgb565(&fb_lcd, rgb_buf, (int)fw, (int)fh);
					sys_status_get(&sys_status);
					ui_overlay_draw(&g_ui, &fb_lcd,
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
		CVI_VPSS_ReleaseChnFrame(VPSS_GRP_CSC, VPSS_CHN0, &stFrame);
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

	if (hevc_recorder_is_recording(&g_recorder))
		hevc_recorder_stop(&g_recorder);

	g_touch_running = 0;
	if (touch_input_available(&g_touch)) {
		pthread_join(g_touch_tid, NULL);
		touch_input_close(&g_touch);
	}

	sys_status_stop();
	fb_lcd_clear(&fb_lcd, 0x0000);
	fb_lcd_close(&fb_lcd);
	sys_mm_deinit();
	free(rgb_buf);

	return 0;
}
