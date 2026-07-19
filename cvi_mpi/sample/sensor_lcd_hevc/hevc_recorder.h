#ifndef HEVC_RECORDER_H
#define HEVC_RECORDER_H

#include <stdbool.h>

#include "sample_comm.h"

#define HEVC_REC_DEFAULT_DIR     "/mnt/data"
#define HEVC_REC_FALLBACK_DIR    "/tmp"
#define HEVC_REC_DEFAULT_BITRATE 6000 /* kbps */
#define HEVC_REC_DEFAULT_FPS     30
#define HEVC_REC_DEFAULT_GOP     30

typedef struct HEVC_RECORDER_CFG_S {
	CVI_U32 width;
	CVI_U32 height;
	CVI_S32 bitrate_kbps;
	CVI_S32 fps;
	CVI_S32 gop;
	VPSS_GRP vpss_grp;
	VPSS_CHN vpss_chn;
	const char *out_dir; /* NULL -> try /mnt/data then /tmp */
} HEVC_RECORDER_CFG_S;

/* Callbacks used by recorder to enable/disable the encode VPSS channel. */
typedef CVI_S32 (*hevc_vpss_enable_fn)(void *ctx);
typedef CVI_S32 (*hevc_vpss_disable_fn)(void *ctx);

typedef struct HEVC_RECORDER_S {
	HEVC_RECORDER_CFG_S cfg;
	hevc_vpss_enable_fn enable_chn;
	hevc_vpss_disable_fn disable_chn;
	void *vpss_ctx;
} HEVC_RECORDER_S;

void hevc_recorder_init(HEVC_RECORDER_S *rec, const HEVC_RECORDER_CFG_S *cfg,
			hevc_vpss_enable_fn enable_chn,
			hevc_vpss_disable_fn disable_chn, void *vpss_ctx);

/* Start H265 encode -> .h265 file. Returns 0 on success. */
int hevc_recorder_start(HEVC_RECORDER_S *rec);

/* Stop encode, join getstream thread, disable VPSS encode chn. */
void hevc_recorder_stop(HEVC_RECORDER_S *rec);

bool hevc_recorder_is_recording(const HEVC_RECORDER_S *rec);
int hevc_recorder_elapsed_sec(const HEVC_RECORDER_S *rec);
const char *hevc_recorder_path(const HEVC_RECORDER_S *rec);

#endif /* HEVC_RECORDER_H */
