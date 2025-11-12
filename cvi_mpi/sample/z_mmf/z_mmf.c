#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/prctl.h>

#include "cvi_buffer.h"
#include "cvi_ae_comm.h"
#include "cvi_awb_comm.h"
#include "cvi_comm_isp.h"

#include "z_mmf.h"

CVI_S32 Z_VI_INIT(Z_VI_CTX_S *pstViCtx)
{
    CVI_S32 s32Ret = CVI_SUCCESS;
    COMPRESS_MODE_E enCompressMode = COMPRESS_MODE_NONE;
    PIC_SIZE_E enPicSize;
    CVI_U32 u32BlkSize;

    SAMPLE_INI_CFG_S stIniCfg = {0};
    stIniCfg = (SAMPLE_INI_CFG_S) {
            .enSource  = VI_PIPE_FRAME_SOURCE_DEV,
            .devNum    = 1,
            .enSnsType[0] = SONY_IMX327_2L_MIPI_2M_30FPS_12BIT,
            .enWDRMode[0] = WDR_MODE_NONE,
            .s32BusId[0]  = 3,
            .MipiDev[0]   = 0xff,
    };

    /* 1. 解析ini */
    s32Ret = SAMPLE_COMM_VI_ParseIni(&stIniCfg);
    if (s32Ret != CVI_SUCCESS) {
        SAMPLE_PRT("Parse ini fail\n");
        return s32Ret;
    }

    CVI_VI_SetDevNum(stIniCfg.devNum);
    s32Ret = SAMPLE_COMM_VI_IniToViCfg(&stIniCfg, &pstViCtx->stViConfig);
    if (s32Ret != CVI_SUCCESS) return s32Ret;

    /* 2. 获取图像尺寸 */
    s32Ret = SAMPLE_COMM_VI_GetSizeBySensor(
            pstViCtx->stViConfig.astViInfo[0].stSnsInfo.enSnsType, &enPicSize);
    if (s32Ret != CVI_SUCCESS) return s32Ret;

    s32Ret = SAMPLE_COMM_SYS_GetPicSize(enPicSize, &pstViCtx->stSize);
    if (s32Ret != CVI_SUCCESS) return s32Ret;

    /* 3. 初始化系统 VB */
    memset(&pstViCtx->stVbConf, 0, sizeof(VB_CONFIG_S));
    pstViCtx->stVbConf.u32MaxPoolCnt = 1;

    u32BlkSize = COMMON_GetPicBufferSize(
            pstViCtx->stSize.u32Width, pstViCtx->stSize.u32Height,
            SAMPLE_PIXEL_FORMAT, DATA_BITWIDTH_8, enCompressMode, DEFAULT_ALIGN
    );

    pstViCtx->stVbConf.astCommPool[0].u32BlkSize = u32BlkSize;
    pstViCtx->stVbConf.astCommPool[0].u32BlkCnt  = 8;

    s32Ret = SAMPLE_COMM_SYS_Init(&pstViCtx->stVbConf);
    if (s32Ret != CVI_SUCCESS) return s32Ret;

    /* 4. 启动 VI */
    s32Ret = SAMPLE_COMM_VI_StartSensor(&pstViCtx->stViConfig);
    if (s32Ret != CVI_SUCCESS) return s32Ret;

    s32Ret = SAMPLE_COMM_VI_StartDev(&pstViCtx->stViConfig.astViInfo[0]);
    if (s32Ret != CVI_SUCCESS) return s32Ret;

    s32Ret = SAMPLE_COMM_VI_StartMIPI(&pstViCtx->stViConfig);
    if (s32Ret != CVI_SUCCESS) return s32Ret;

    /* 创建 pipe */
    VI_PIPE_ATTR_S stPipeAttr;
    stPipeAttr.bYuvSkip = CVI_FALSE;
    stPipeAttr.u32MaxW = pstViCtx->stSize.u32Width;
    stPipeAttr.u32MaxH = pstViCtx->stSize.u32Height;
    stPipeAttr.enPixFmt = PIXEL_FORMAT_RGB_BAYER_12BPP;
    stPipeAttr.enBitWidth = DATA_BITWIDTH_12;
    stPipeAttr.stFrameRate.s32SrcFrameRate = -1;
    stPipeAttr.stFrameRate.s32DstFrameRate = -1;
    stPipeAttr.bNrEn = CVI_TRUE;
    stPipeAttr.bYuvBypassPath = CVI_FALSE;
    stPipeAttr.enCompressMode = pstViCtx->stViConfig.astViInfo[0].stChnInfo.enCompressMode;

    pstViCtx->ViPipe = 0;
    pstViCtx->ViChn = 0;
    pstViCtx->ViDev = 0;

    s32Ret = CVI_VI_CreatePipe(pstViCtx->ViPipe, &stPipeAttr);
    if (s32Ret != CVI_SUCCESS) return s32Ret;

    s32Ret = CVI_VI_StartPipe(pstViCtx->ViPipe);
    if (s32Ret != CVI_SUCCESS) return s32Ret;

    s32Ret = SAMPLE_COMM_VI_CreateIsp(&pstViCtx->stViConfig);
    if (s32Ret != CVI_SUCCESS) return s32Ret;

    s32Ret = SAMPLE_COMM_VI_StartViChn(&pstViCtx->stViConfig);
    if (s32Ret != CVI_SUCCESS) return s32Ret;


    /************************************************
     * step5:  Init VPSS
     ************************************************/
    VPSS_GRP	   VpssGrp	  = 0;
    VPSS_GRP_ATTR_S    stVpssGrpAttr;
    VPSS_CHN           VpssChn        = VPSS_CHN0;
    CVI_BOOL           abChnEnable[VPSS_MAX_PHY_CHN_NUM] = {0};
    VPSS_CHN_ATTR_S    astVpssChnAttr[VPSS_MAX_PHY_CHN_NUM] = {0};

    stVpssGrpAttr.stFrameRate.s32SrcFrameRate    = -1;
    stVpssGrpAttr.stFrameRate.s32DstFrameRate    = -1;
    stVpssGrpAttr.enPixelFormat                  = SAMPLE_PIXEL_FORMAT;
    stVpssGrpAttr.u32MaxW                        = pstViCtx->stSize.u32Width;
    stVpssGrpAttr.u32MaxH                        = pstViCtx->stSize.u32Height;
    stVpssGrpAttr.u8VpssDev                      = 0;

    astVpssChnAttr[VpssChn].u32Width                    = 552;
    astVpssChnAttr[VpssChn].u32Height                   = 368;
    astVpssChnAttr[VpssChn].enVideoFormat               = VIDEO_FORMAT_LINEAR;
    astVpssChnAttr[VpssChn].enPixelFormat               = SAMPLE_PIXEL_FORMAT;
    astVpssChnAttr[VpssChn].stFrameRate.s32SrcFrameRate = 30;
    astVpssChnAttr[VpssChn].stFrameRate.s32DstFrameRate = 30;
    astVpssChnAttr[VpssChn].u32Depth                    = 0;
    astVpssChnAttr[VpssChn].bMirror                     = CVI_FALSE;
    astVpssChnAttr[VpssChn].bFlip                       = CVI_FALSE;
    astVpssChnAttr[VpssChn].stAspectRatio.enMode        = ASPECT_RATIO_AUTO;
    astVpssChnAttr[VpssChn].stAspectRatio.bEnableBgColor = CVI_TRUE;
    astVpssChnAttr[VpssChn].stAspectRatio.u32BgColor    = COLOR_RGB_BLACK;
    astVpssChnAttr[VpssChn].stNormalize.bEnable         = CVI_FALSE;

    /*start vpss*/
    abChnEnable[0] = CVI_TRUE;
    s32Ret = SAMPLE_COMM_VPSS_Init(VpssGrp, abChnEnable, &stVpssGrpAttr, astVpssChnAttr);
    if (s32Ret != CVI_SUCCESS) {
        SAMPLE_PRT("init vpss group failed. s32Ret: 0x%x !\n", s32Ret);
        return s32Ret;
    }

    s32Ret = SAMPLE_COMM_VPSS_Start(VpssGrp, abChnEnable, &stVpssGrpAttr, astVpssChnAttr);
    if (s32Ret != CVI_SUCCESS) {
        SAMPLE_PRT("start vpss group failed. s32Ret: 0x%x !\n", s32Ret);
        return s32Ret;
    }

    s32Ret = SAMPLE_COMM_VI_Bind_VPSS(pstViCtx->ViPipe, pstViCtx->ViChn, VpssGrp);
    if (s32Ret != CVI_SUCCESS) {
        SAMPLE_PRT("vi bind vpss failed. s32Ret: 0x%x !\n", s32Ret);
        return s32Ret;
    }

    /************************************************
     * step6:  Init VO
     ************************************************/
    SAMPLE_VO_CONFIG_S stVoConfig;
    RECT_S stDefDispRect  = {0, 0, 368, 552};
    SIZE_S stDefImageSize = {368, 552};
    VO_CHN VoChn = 0;

    s32Ret = SAMPLE_COMM_VO_GetDefConfig(&stVoConfig);
    if (s32Ret != CVI_SUCCESS) {
        CVI_TRACE_LOG(CVI_DBG_ERR, "SAMPLE_COMM_VO_GetDefConfig failed with %#x\n", s32Ret);
        return s32Ret;
    }

    stVoConfig.VoDev	 = VoChn;
    stVoConfig.stVoPubAttr.enIntfType  = VO_INTF_MIPI;
    stVoConfig.stVoPubAttr.enIntfSync  = VO_OUTPUT_720P60;
    stVoConfig.stDispRect	 = stDefDispRect;
    stVoConfig.stImageSize	 = stDefImageSize;
    stVoConfig.enPixFormat	 = SAMPLE_PIXEL_FORMAT;
    stVoConfig.enVoMode	 = VO_MODE_1MUX;

    s32Ret = SAMPLE_COMM_VO_StartVO(&stVoConfig);
    if (s32Ret != CVI_SUCCESS) {
        SAMPLE_PRT("SAMPLE_COMM_VO_StartVO failed with %#x\n", s32Ret);
        return s32Ret;
    }

    CVI_VO_SetChnRotation(VoChn, VoChn, ROTATION_90);

    CVI_VO_EnableChn(VoChn, VoChn);
    CVI_VO_ShowChn(VoChn, VoChn);


    SAMPLE_PRT("[Z_VI_INIT] success. width=%d height=%d\n",
               pstViCtx->stSize.u32Width, pstViCtx->stSize.u32Height);

    return CVI_SUCCESS;
}

CVI_S32 Z_VI_TAKE_FRAME(Z_VI_CTX_S *pstViCtx, VIDEO_FRAME_INFO_S *pstFrameInfo, CVI_S32 s32MilliSec)
{
    CVI_S32 s32Ret;
    s32Ret = CVI_VI_GetChnFrame(pstViCtx->ViPipe, pstViCtx->ViChn, pstFrameInfo, s32MilliSec);
    if (s32Ret != CVI_SUCCESS) {
        SAMPLE_PRT("[Z_VI_TAKE_FRAME] failed: 0x%x\n", s32Ret);
    }
    return s32Ret;
}

CVI_S32 Z_VI_RELEASE_FRAME(Z_VI_CTX_S *pstViCtx, VIDEO_FRAME_INFO_S *pstFrameInfo)
{
    CVI_S32 s32Ret;
    s32Ret = CVI_VI_ReleaseChnFrame(pstViCtx->ViPipe, pstViCtx->ViChn, pstFrameInfo);
    if (s32Ret != CVI_SUCCESS) {
        SAMPLE_PRT("[Z_VI_RELEASE_FRAME] failed: 0x%x\n", s32Ret);
    }
    return s32Ret;
}

CVI_S32 Z_VPSS_TAKE_FRAME(VIDEO_FRAME_INFO_S *pstFrameInfo, CVI_S32 s32MilliSec)
{
    CVI_S32 s32GetRet = CVI_SUCCESS;
    VPSS_GRP VpssGrp = 0;      /* 与上面创建的保持一致 */
    VPSS_CHN VpssChn = VPSS_CHN0;

    s32GetRet = CVI_VPSS_GetChnFrame(VpssGrp, VpssChn, pstFrameInfo, s32MilliSec);
    if (s32GetRet != CVI_SUCCESS) {
        /* 超时或失败：打印并继续（可根据需要做重试/断开处理） */
        SAMPLE_PRT("CVI_VPSS_GetChnFrame failed: 0x%x\n", s32GetRet);
        return s32GetRet;
    }

    return s32GetRet;
}

CVI_S32 Z_VPSS_RELEASE_FRAME(VIDEO_FRAME_INFO_S *pstFrameInfo)
{
    CVI_S32 s32Ret = CVI_SUCCESS;

    VPSS_GRP VpssGrp = 0;      /* 与上面创建的保持一致 */
    VPSS_CHN VpssChn = VPSS_CHN0;

    s32Ret = CVI_VPSS_ReleaseChnFrame(VpssGrp, VpssChn, pstFrameInfo);
    if (s32Ret != CVI_SUCCESS) {
        SAMPLE_PRT("CVI_VPSS_ReleaseChnFrame failed: 0x%x\n", s32Ret);
    }

    return s32Ret;
}

CVI_S32 Z_VO_PUSH_FRAME(VIDEO_FRAME_INFO_S *pstFrameInfo)
{
    CVI_S32 s32Ret;

    VO_CHN VoChn = 0;

    s32Ret = CVI_VO_SendFrame(VoChn, VoChn, pstFrameInfo, 0);
    if (s32Ret != CVI_SUCCESS) {
        SAMPLE_PRT("CVI_VO_SendFrame failed: 0x%x\n", s32Ret);
        /* 即便发送失败，也应 Release 帧以防内存泄露 */
    }

    return s32Ret;
}


CVI_S32 Z_VI_DEINIT(Z_VI_CTX_S *pstViCtx)
{
    SAMPLE_COMM_VI_DestroyIsp(&pstViCtx->stViConfig);
    SAMPLE_COMM_VI_DestroyVi(&pstViCtx->stViConfig);

    VO_CHN VoChn = 0;
    VPSS_GRP	   VpssGrp	  = 0;
    CVI_BOOL           abChnEnable[VPSS_MAX_PHY_CHN_NUM] = {0};

    CVI_VO_HideChn(VoChn, VoChn);
    CVI_VO_DisableChn(VoChn, VoChn);
//    SAMPLE_COMM_VO_StopVO(&stVoConfig);

    SAMPLE_COMM_VI_UnBind_VPSS(pstViCtx->ViPipe, pstViCtx->ViChn, VpssGrp);
    SAMPLE_COMM_VPSS_Stop(VpssGrp, abChnEnable);

    SAMPLE_COMM_SYS_Exit();
    SAMPLE_PRT("[Z_VI_DEINIT] done\n");
    return CVI_SUCCESS;
}
