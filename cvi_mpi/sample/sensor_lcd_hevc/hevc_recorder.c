#include "hevc_recorder.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "sample_comm.h"

#define VENC_CHN0 0

typedef struct {
	HEVC_RECORDER_S *rec;
	FILE *fp;
	char path[256];
	pthread_t tid;
	volatile int running;
	volatile int recording;
	struct timespec t_start;
	CVI_BOOL venc_started;
	CVI_BOOL venc_bound;
	CVI_BOOL chn_enabled;
	chnInputCfg ic;
} hevc_priv_t;

static hevc_priv_t g_priv;

static int ensure_dir(const char *dir)
{
	struct stat st;

	if (!dir)
		return -1;
	if (stat(dir, &st) == 0) {
		if (S_ISDIR(st.st_mode) && access(dir, W_OK) == 0)
			return 0;
		return -1;
	}
	if (mkdir(dir, 0755) == 0)
		return 0;
	return -1;
}

static const char *pick_out_dir(const char *preferred)
{
	if (preferred && ensure_dir(preferred) == 0)
		return preferred;
	if (ensure_dir(HEVC_REC_DEFAULT_DIR) == 0)
		return HEVC_REC_DEFAULT_DIR;
	if (ensure_dir(HEVC_REC_FALLBACK_DIR) == 0) {
		SAMPLE_PRT("warn: %s not writable, using %s\n",
			   preferred ? preferred : HEVC_REC_DEFAULT_DIR,
			   HEVC_REC_FALLBACK_DIR);
		return HEVC_REC_FALLBACK_DIR;
	}
	return NULL;
}

static int make_out_path(char *buf, size_t buflen, const char *dir)
{
	time_t now;
	struct tm tm_now;

	time(&now);
	localtime_r(&now, &tm_now);
	return snprintf(buf, buflen, "%s/rec_%04d%02d%02d_%02d%02d%02d.h265",
			dir,
			tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
			tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
}

static CVI_S32 save_stream(FILE *fp, VENC_STREAM_S *st)
{
	CVI_U32 i;

	for (i = 0; i < st->u32PackCount; i++) {
		VENC_PACK_S *pk = &st->pstPack[i];
		size_t len = pk->u32Len - pk->u32Offset;

		if (fwrite(pk->pu8Addr + pk->u32Offset, 1, len, fp) != len)
			return CVI_FAILURE;
	}
	return CVI_SUCCESS;
}

static void *getstream_proc(void *arg)
{
	hevc_priv_t *p = (hevc_priv_t *)arg;
	VENC_CHN VencChn = VENC_CHN0;

	(void)arg;

	while (p->running) {
		VENC_CHN_STATUS_S stStat;
		VENC_STREAM_S stStream;
		CVI_S32 s32Ret;

		memset(&stStream, 0, sizeof(stStream));
		s32Ret = CVI_VENC_QueryStatus(VencChn, &stStat);
		if (s32Ret != CVI_SUCCESS) {
			usleep(5000);
			continue;
		}
		if (stStat.u32CurPacks == 0) {
			usleep(2000);
			continue;
		}

		stStream.pstPack = (VENC_PACK_S *)malloc(
			sizeof(VENC_PACK_S) * stStat.u32CurPacks);
		if (!stStream.pstPack) {
			usleep(5000);
			continue;
		}

		s32Ret = CVI_VENC_GetStream(VencChn, &stStream, 1000);
		if (s32Ret != CVI_SUCCESS) {
			free(stStream.pstPack);
			continue;
		}

		if (p->fp)
			save_stream(p->fp, &stStream);

		CVI_VENC_ReleaseStream(VencChn, &stStream);
		free(stStream.pstPack);
	}

	return NULL;
}

void hevc_recorder_init(HEVC_RECORDER_S *rec, const HEVC_RECORDER_CFG_S *cfg,
			hevc_vpss_enable_fn enable_chn,
			hevc_vpss_disable_fn disable_chn, void *vpss_ctx)
{
	memset(&g_priv, 0, sizeof(g_priv));
	if (!rec || !cfg)
		return;

	rec->cfg = *cfg;
	if (rec->cfg.bitrate_kbps <= 0)
		rec->cfg.bitrate_kbps = HEVC_REC_DEFAULT_BITRATE;
	if (rec->cfg.fps <= 0)
		rec->cfg.fps = HEVC_REC_DEFAULT_FPS;
	if (rec->cfg.gop <= 0)
		rec->cfg.gop = HEVC_REC_DEFAULT_GOP;
	/* width/height must be set by caller from active sensor mode */

	rec->enable_chn = enable_chn;
	rec->disable_chn = disable_chn;
	rec->vpss_ctx = vpss_ctx;
	g_priv.rec = rec;
}

bool hevc_recorder_is_recording(const HEVC_RECORDER_S *rec)
{
	(void)rec;
	return g_priv.recording != 0;
}

int hevc_recorder_elapsed_sec(const HEVC_RECORDER_S *rec)
{
	struct timespec now;
	(void)rec;

	if (!g_priv.recording)
		return 0;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int)(now.tv_sec - g_priv.t_start.tv_sec);
}

const char *hevc_recorder_path(const HEVC_RECORDER_S *rec)
{
	(void)rec;
	if (!g_priv.path[0])
		return NULL;
	return g_priv.path;
}

int hevc_recorder_start(HEVC_RECORDER_S *rec)
{
	const char *dir;
	VENC_GOP_ATTR_S stGopAttr;
	PIC_SIZE_E enSize;
	CVI_S32 s32Ret;
	pthread_attr_t attr;
	struct sched_param param;
	HEVC_RECORDER_CFG_S *cfg;

	if (!rec)
		return -1;
	if (g_priv.recording)
		return 0;

	cfg = &rec->cfg;
	dir = pick_out_dir(cfg->out_dir);
	if (!dir) {
		SAMPLE_PRT("no writable output directory\n");
		return -1;
	}

	make_out_path(g_priv.path, sizeof(g_priv.path), dir);
	g_priv.fp = fopen(g_priv.path, "wb");
	if (!g_priv.fp) {
		SAMPLE_PRT("fopen %s failed: %s\n", g_priv.path, strerror(errno));
		g_priv.path[0] = '\0';
		return -1;
	}

	/* Enable VPSS encode channel first */
	if (rec->enable_chn) {
		s32Ret = rec->enable_chn(rec->vpss_ctx);
		if (s32Ret != CVI_SUCCESS) {
			SAMPLE_PRT("enable encode vpss chn failed: %#x\n", s32Ret);
			fclose(g_priv.fp);
			g_priv.fp = NULL;
			unlink(g_priv.path);
			g_priv.path[0] = '\0';
			return -1;
		}
		g_priv.chn_enabled = CVI_TRUE;
	}

	SAMPLE_COMM_VENC_InitChnInputCfg(&g_priv.ic);
	strncpy(g_priv.ic.codec, "265", sizeof(g_priv.ic.codec) - 1);
	g_priv.ic.width = cfg->width;
	g_priv.ic.height = cfg->height;
	g_priv.ic.framerate = cfg->fps;
	g_priv.ic.srcFramerate = cfg->fps;
	g_priv.ic.gop = cfg->gop;
	g_priv.ic.bitrate = cfg->bitrate_kbps;
	g_priv.ic.rcMode = SAMPLE_RC_CBR;
	g_priv.ic.bind_mode = VENC_BIND_VPSS;
	g_priv.ic.vpssGrp = cfg->vpss_grp;
	g_priv.ic.vpssChn = cfg->vpss_chn;
	g_priv.ic.num_frames = -1;
	g_priv.ic.getstream_timeout = 1000;
	g_priv.ic.bCreateChn = CVI_FALSE;
	/* RC defaults required by CVI_VENC_SetRcParam (Init leaves -1). */
	g_priv.ic.maxQp = DEF_264_MAXQP;
	g_priv.ic.minQp = DEF_264_MINQP;
	g_priv.ic.maxIqp = DEF_264_MAXIQP;
	g_priv.ic.minIqp = DEF_264_MINIQP;
	g_priv.ic.firstFrmstartQp = 63;
	g_priv.ic.statTime = 2;

	if (cfg->width == 0 || cfg->height == 0) {
		SAMPLE_PRT("encode size not set (width=%u height=%u)\n",
			   cfg->width, cfg->height);
		goto fail;
	}

	/* Prefer named PIC size when it matches; else CUSTOMIZE with actual dims. */
	if (cfg->width == 1920 && cfg->height == 1080)
		enSize = PIC_1080P;
	else if (cfg->width == 2560 && cfg->height == 1440)
		enSize = PIC_1440P;
	else if (cfg->width == 2880 && cfg->height == 1620)
		enSize = PIC_2880x1620;
	else if (cfg->width == 1440 && cfg->height == 2560)
		enSize = PIC_CUSTOMIZE; /* 2K portrait */
	else if (cfg->width == 1152 && cfg->height == 2048)
		enSize = PIC_CUSTOMIZE;
	else
		enSize = PIC_CUSTOMIZE;

	/* Larger frames need a bigger ES buffer (default 0 lets driver pick minimum). */
	if (g_priv.ic.bitstreamBufSize == 0 &&
	    (CVI_U64)cfg->width * cfg->height > 1920ULL * 1080ULL)
		g_priv.ic.bitstreamBufSize = 1024 * 1024;

	s32Ret = SAMPLE_COMM_VENC_GetGopAttr(VENC_GOPMODE_NORMALP, &stGopAttr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("GetGopAttr failed: %#x\n", s32Ret);
		goto fail;
	}

	/*
	 * Give VPSS a moment to produce frames on the newly enabled channel
	 * before VENC starts receiving.
	 */
	usleep(50 * 1000);

	s32Ret = SAMPLE_COMM_VENC_Start(&g_priv.ic, VENC_CHN0, PT_H265, enSize,
					SAMPLE_RC_CBR, 0, CVI_FALSE, &stGopAttr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("SAMPLE_COMM_VENC_Start failed: %#x\n", s32Ret);
		goto fail;
	}
	g_priv.venc_started = CVI_TRUE;
	g_priv.venc_bound = CVI_TRUE;

	g_priv.running = 1;
	pthread_attr_init(&attr);
	pthread_attr_setschedpolicy(&attr, SCHED_RR);
	param.sched_priority = 80;
	pthread_attr_setschedparam(&attr, &param);
	pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	if (pthread_create(&g_priv.tid, &attr, getstream_proc, &g_priv) != 0) {
		/* Fallback without RT priority */
		if (pthread_create(&g_priv.tid, NULL, getstream_proc, &g_priv) != 0) {
			SAMPLE_PRT("create getstream thread failed\n");
			pthread_attr_destroy(&attr);
			goto fail;
		}
	}
	pthread_attr_destroy(&attr);

	clock_gettime(CLOCK_MONOTONIC, &g_priv.t_start);
	g_priv.recording = 1;
	SAMPLE_PRT("Recording started: %s (%ux%u @ %dkbps)\n",
		   g_priv.path, cfg->width, cfg->height, cfg->bitrate_kbps);
	return 0;

fail:
	if (g_priv.venc_bound) {
		SAMPLE_COMM_VPSS_UnBind_VENC(cfg->vpss_grp, cfg->vpss_chn, VENC_CHN0);
		g_priv.venc_bound = CVI_FALSE;
	}
	if (g_priv.venc_started) {
		SAMPLE_COMM_VENC_Stop(VENC_CHN0);
		g_priv.venc_started = CVI_FALSE;
		g_priv.ic.bCreateChn = CVI_FALSE;
	}
	if (g_priv.chn_enabled && rec->disable_chn) {
		rec->disable_chn(rec->vpss_ctx);
		g_priv.chn_enabled = CVI_FALSE;
	}
	if (g_priv.fp) {
		fclose(g_priv.fp);
		g_priv.fp = NULL;
	}
	if (g_priv.path[0]) {
		unlink(g_priv.path);
		g_priv.path[0] = '\0';
	}
	return -1;
}

void hevc_recorder_stop(HEVC_RECORDER_S *rec)
{
	HEVC_RECORDER_CFG_S *cfg;

	if (!rec || !g_priv.recording)
		return;

	cfg = &rec->cfg;
	g_priv.running = 0;
	pthread_join(g_priv.tid, NULL);

	if (g_priv.fp) {
		fflush(g_priv.fp);
		fclose(g_priv.fp);
		g_priv.fp = NULL;
	}

	if (g_priv.venc_bound) {
		SAMPLE_COMM_VPSS_UnBind_VENC(cfg->vpss_grp, cfg->vpss_chn, VENC_CHN0);
		g_priv.venc_bound = CVI_FALSE;
	}
	if (g_priv.venc_started) {
		SAMPLE_COMM_VENC_Stop(VENC_CHN0);
		g_priv.venc_started = CVI_FALSE;
		g_priv.ic.bCreateChn = CVI_FALSE;
	}
	if (g_priv.chn_enabled && rec->disable_chn) {
		rec->disable_chn(rec->vpss_ctx);
		g_priv.chn_enabled = CVI_FALSE;
	}

	SAMPLE_PRT("Recording stopped: %s\n", g_priv.path);
	g_priv.recording = 0;
}
