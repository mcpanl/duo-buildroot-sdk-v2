#ifndef __Z_MMF_H__
#define __Z_MMF_H__


#include "sample_comm.h"

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif /* End of #ifdef __cplusplus */

typedef struct _Z_VI_CTX_S {
    VI_DEV ViDev;
    VI_PIPE ViPipe;
    VI_CHN ViChn;
    SAMPLE_VI_CONFIG_S stViConfig;
    VB_CONFIG_S stVbConf;
    SIZE_S stSize;
} Z_VI_CTX_S;

/* 函数声明 */
CVI_S32 Z_VI_INIT(Z_VI_CTX_S *pstViCtx);
CVI_S32 Z_VI_TAKE_FRAME(Z_VI_CTX_S *pstViCtx, VIDEO_FRAME_INFO_S *pstFrameInfo, CVI_S32 s32MilliSec);
CVI_S32 Z_VI_RELEASE_FRAME(Z_VI_CTX_S *pstViCtx, VIDEO_FRAME_INFO_S *pstFrameInfo);
CVI_S32 Z_VO_PUSH_FRAME(VIDEO_FRAME_INFO_S *pstFrameInfo);
CVI_S32 Z_VPSS_TAKE_FRAME(VIDEO_FRAME_INFO_S *pstFrameInfo, CVI_S32 s32MilliSec);
CVI_S32 Z_VPSS_RELEASE_FRAME(VIDEO_FRAME_INFO_S *pstFrameInfo);
CVI_S32 Z_VI_DEINIT(Z_VI_CTX_S *pstViCtx);

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif /* End of #ifdef __cplusplus */

#endif /* End of #ifndef __Z_MMF_H__ */