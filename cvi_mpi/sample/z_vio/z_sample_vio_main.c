#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#include "sample_comm.h"
#include "cvi_sys.h"
#include <linux/cvi_type.h>

#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/param.h>
#include <sys/prctl.h>
#include <inttypes.h>

#include "cvi_buffer.h"
#include "cvi_ae_comm.h"
#include "cvi_awb_comm.h"
#include "cvi_comm_isp.h"

#include "sample_comm.h"

bool _need_exit = false;

bool need_exit()
{
    return _need_exit;
}

CVI_S32 SAMPLE_VIO_DualVpssChn(void)
{
	COMPRESS_MODE_E    enCompressMode   = COMPRESS_MODE_NONE;
	VB_CONFIG_S        stVbConf;
	PIC_SIZE_E         enPicSize;
	CVI_U32	       u32BlkSize;
	SIZE_S stSize;
	CVI_S32 s32Ret = CVI_SUCCESS;

	VI_DEV ViDev = 0;
	VI_PIPE ViPipe = 0;
	VI_CHN ViChn = 0;
	CVI_S32 s32WorkSnsId = 0;
	SAMPLE_VI_CONFIG_S stViConfig;
	VI_PIPE_ATTR_S     stPipeAttr;

	SAMPLE_INI_CFG_S   stIniCfg = {0};

	stIniCfg = (SAMPLE_INI_CFG_S) {
			.enSource  = VI_PIPE_FRAME_SOURCE_DEV,
			.devNum    = 1,
			.enSnsType[0] = SONY_IMX327_2L_MIPI_2M_30FPS_12BIT,
			.enWDRMode[0] = WDR_MODE_NONE,
			.s32BusId[0]  = 3,
			.MipiDev[0]   = 0xff,
			.enSnsType[1] = SONY_IMX327_SLAVE_MIPI_2M_30FPS_12BIT,
			.s32BusId[1] = 0,
			.MipiDev[1] = 0xff,
	};
	// Get config from ini if found.
	s32Ret = SAMPLE_COMM_VI_ParseIni(&stIniCfg);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("Parse fail\n");
	} else {
		SAMPLE_PRT("Parse complete\n");
	}

	if (stIniCfg.devNum > 1) {
		SAMPLE_PRT("Only support 1 sensor device\n");
		return CVI_FAILURE;
	}
	CVI_VI_SetDevNum(stIniCfg.devNum);
	/************************************************
     * step1:  Config VI
     ************************************************/
	s32Ret = SAMPLE_COMM_VI_IniToViCfg(&stIniCfg, &stViConfig);
	if (s32Ret != CVI_SUCCESS)
		return s32Ret;

	/************************************************
     * step2:  Get input size
     ************************************************/
	s32Ret = SAMPLE_COMM_VI_GetSizeBySensor(stViConfig.astViInfo[s32WorkSnsId].stSnsInfo.enSnsType, &enPicSize);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("SAMPLE_COMM_VI_GetSizeBySensor failed with %#x\n", s32Ret);
		return s32Ret;
	}

	s32Ret = SAMPLE_COMM_SYS_GetPicSize(enPicSize, &stSize);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("SAMPLE_COMM_SYS_GetPicSize failed with %#x\n", s32Ret);
		return s32Ret;
	}

	/************************************************
     * step3:  Init SYS and common VB
     ************************************************/
	memset(&stVbConf, 0, sizeof(VB_CONFIG_S));
	stVbConf.u32MaxPoolCnt		= 1;

	u32BlkSize = COMMON_GetPicBufferSize(stSize.u32Width, stSize.u32Height, SAMPLE_PIXEL_FORMAT, DATA_BITWIDTH_8
			, enCompressMode, DEFAULT_ALIGN);
	stVbConf.astCommPool[0].u32BlkSize	= u32BlkSize;
	stVbConf.astCommPool[0].u32BlkCnt	= 8;
	SAMPLE_PRT("common pool[0] BlkSize %d\n", u32BlkSize);

	s32Ret = SAMPLE_COMM_SYS_Init(&stVbConf);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("system init failed with %#x\n", s32Ret);
		return -1;
	}

	/************************************************
     * step4:  Init VI ISP
     ************************************************/
	s32Ret = SAMPLE_COMM_VI_StartSensor(&stViConfig);
	if (s32Ret != CVI_SUCCESS) {
		CVI_TRACE_LOG(CVI_DBG_ERR, "system start sensor failed with %#x\n", s32Ret);
		return s32Ret;
	}
	s32Ret = SAMPLE_COMM_VI_StartDev(&stViConfig.astViInfo[ViDev]);
	if (s32Ret != CVI_SUCCESS) {
		CVI_TRACE_LOG(CVI_DBG_ERR, "VI_StartDev failed with %#x!\n", s32Ret);
		return s32Ret;
	}
	s32Ret = SAMPLE_COMM_VI_StartMIPI(&stViConfig);
	if (s32Ret != CVI_SUCCESS) {
		CVI_TRACE_LOG(CVI_DBG_ERR, "system start MIPI failed with %#x\n", s32Ret);
		return s32Ret;
	}

	stPipeAttr.bYuvSkip = CVI_FALSE;
	stPipeAttr.u32MaxW = stSize.u32Width;
	stPipeAttr.u32MaxH = stSize.u32Height;
	stPipeAttr.enPixFmt = PIXEL_FORMAT_RGB_BAYER_12BPP;
	stPipeAttr.enBitWidth = DATA_BITWIDTH_12;
	stPipeAttr.stFrameRate.s32SrcFrameRate = -1;
	stPipeAttr.stFrameRate.s32DstFrameRate = -1;
	stPipeAttr.bNrEn = CVI_TRUE;
	stPipeAttr.bYuvBypassPath = CVI_FALSE;
	stPipeAttr.enCompressMode = stViConfig.astViInfo[0].stChnInfo.enCompressMode;

	s32Ret = CVI_VI_CreatePipe(ViPipe, &stPipeAttr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("CVI_VI_CreatePipe failed with %#x!\n", s32Ret);
		return s32Ret;
	}

	s32Ret = CVI_VI_StartPipe(ViPipe);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("CVI_VI_StartPipe failed with %#x!\n", s32Ret);
		return s32Ret;
	}

	s32Ret = CVI_VI_GetPipeAttr(ViPipe, &stPipeAttr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("CVI_VI_StartPipe failed with %#x!\n", s32Ret);
		return s32Ret;
	}

	s32Ret = SAMPLE_COMM_VI_CreateIsp(&stViConfig);
	if (s32Ret != CVI_SUCCESS) {
		CVI_TRACE_LOG(CVI_DBG_ERR, "VI_CreateIsp failed with %#x!\n", s32Ret);
		return s32Ret;
	}

	SAMPLE_COMM_VI_StartViChn(&stViConfig);

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
	stVpssGrpAttr.u32MaxW                        = stSize.u32Width;
	stVpssGrpAttr.u32MaxH                        = stSize.u32Height;
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

	s32Ret = SAMPLE_COMM_VI_Bind_VPSS(ViPipe, ViChn, VpssGrp);
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

	stVoConfig.VoDev	 = 0;
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

	CVI_VO_SetChnRotation(stVoConfig.VoDev, VoChn, ROTATION_90);




	SAMPLE_COMM_VPSS_Bind_VO(VpssGrp, VpssChn, stVoConfig.VoDev, VoChn);


	/* 下面进入软件循环：从 VPSS 抓帧 -> 处理 -> send 到 VO -> 释放帧 */
	{
		VIDEO_FRAME_INFO_S stFrameInfo;
		CVI_S32 s32GetRet = CVI_SUCCESS;
//        VPSS_GRP VpssGrp = 0;      /* 与上面创建的保持一致 */
//        VPSS_CHN VpssChn = VPSS_CHN0;
		VO_CHN VoChn = 0;
		VO_LAYER VoLayer = stVoConfig.VoDev; /* 在你的示例里 VoDev 被当作 layer 使用 */

		/* 确保 VO 通道已 enable/show（若 SAMPLE_COMM 启动已经完成这步可省） */
		CVI_VO_EnableChn(VoLayer, VoChn);
		CVI_VO_ShowChn(VoLayer, VoChn);

		SAMPLE_PRT("Entering software grab/send loop. Press Ctrl+C to exit...\n");

		while (!need_exit()) {
			/* from vi get a frame */
			s32GetRet = CVI_VI_GetChnFrame(ViPipe, ViChn, &stFrameInfo, 1000);
			if(s32GetRet != CVI_SUCCESS) {
				SAMPLE_PRT("CVI_VI_GetChnFrame failed: 0x%x\n", s32GetRet);
				continue;
			}

			/* 从 VPSS 通道拿一帧，超时 1000ms（按需调整）*/
//            s32GetRet = CVI_VPSS_GetChnFrame(VpssGrp, VpssChn, &stFrameInfo, 1000);
//            if (s32GetRet != CVI_SUCCESS) {
//                /* 超时或失败：打印并继续（可根据需要做重试/断开处理） */
//                SAMPLE_PRT("CVI_VPSS_GetChnFrame failed: 0x%x\n", s32GetRet);
//                continue;
//            }

			/* -------------------------
             * 在这里可以对 stFrameInfo 做任意处理（例如 OSD/图像滤波/缩放/颜色空间转换等）
             * 注意：如果你修改了像素格式/尺寸，VO 端需要能接受该格式或做相应转换。
             * 示例里我们暂时不改动内容，直接透传到 VO。
             * ------------------------- */
			/* 示例占位：处理函数（用户自行实现）
             * ProcessFrame(&stFrameInfo);
             */

			printf(">>> stFrameInfo w=%d, h=%d\n", stFrameInfo.stVFrame.u32Width, stFrameInfo.stVFrame.u32Height);

			/* 发送到 VO 显示通道（非阻塞，s32MilliSec 当前无效）。注意参数为 VideoLayer, VoChn。 */
//            s32Ret = CVI_VO_SendFrame(VoLayer, VoChn, &stFrameInfo, 0);
//            if (s32Ret != CVI_SUCCESS) {
//                SAMPLE_PRT("CVI_VO_SendFrame failed: 0x%x\n", s32Ret);
//                /* 即便发送失败，也应 Release 帧以防内存泄露 */
//            }

			/* 释放 VPSS 帧（无论是否成功发送都需要释放） */
//            s32Ret = CVI_VPSS_ReleaseChnFrame(VpssGrp, VpssChn, &stFrameInfo);
//            if (s32Ret != CVI_SUCCESS) {
//                SAMPLE_PRT("CVI_VPSS_ReleaseChnFrame failed: 0x%x\n", s32Ret);
//            }

			/* release vi frame */
			s32Ret = CVI_VI_ReleaseChnFrame(ViPipe, ViChn, &stFrameInfo);


			/* 根据需要 sleep 一小段时间以避免 busy loop（可调整或删除） */
			usleep(32000); /* 约 30 fps 的节奏（可按需修改） */
		}

		/* 退出前，隐藏/disable VO 通道（可选） */
		CVI_VO_HideChn(VoLayer, VoChn);
		CVI_VO_DisableChn(VoLayer, VoChn);
	}

//    PAUSE();

	SAMPLE_COMM_VPSS_UnBind_VO(VpssGrp, VpssChn, stVoConfig.VoDev, VoChn);

	SAMPLE_COMM_VO_StopVO(&stVoConfig);

	SAMPLE_COMM_VI_UnBind_VPSS(ViPipe, ViChn, VpssGrp);

	SAMPLE_COMM_VPSS_Stop(VpssGrp, abChnEnable);

	SAMPLE_COMM_VI_DestroyIsp(&stViConfig);

	SAMPLE_COMM_VI_DestroyVi(&stViConfig);

	SAMPLE_COMM_SYS_Exit();
	return s32Ret;
}

void SAMPLE_VIO_HandleSig(CVI_S32 signo)
{
	signal(SIGINT, SIG_IGN);
	signal(SIGTERM, SIG_IGN);

	if (SIGINT == signo || SIGTERM == signo) {
		_need_exit = true;
//		SAMPLE_PRT("Program termination abnormally\n");
	}
//	exit(-1);
}

int main()
{
	printf("=== Z ===\n");
	CVI_S32 s32Ret = CVI_FAILURE;

	signal(SIGINT, SAMPLE_VIO_HandleSig);
	signal(SIGTERM, SAMPLE_VIO_HandleSig);

	s32Ret = SAMPLE_VIO_DualVpssChn();

	if (s32Ret == CVI_SUCCESS)
    {
        SAMPLE_PRT("sample_vio exit success!\n");
    }
	else
    {
        SAMPLE_PRT("sample_vio exit abnormally!\n");
    }

    printf("~~~ Z ~~~\n");

	return s32Ret;
}

