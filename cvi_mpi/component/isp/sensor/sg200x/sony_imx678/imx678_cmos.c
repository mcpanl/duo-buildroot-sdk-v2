#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <syslog.h>
#include <errno.h>

#include <linux/cvi_type.h>
#include <linux/cvi_comm_video.h>
#include "cvi_debug.h"
#include "cvi_comm_sns.h"
#include "cvi_sns_ctrl.h"
#include "cvi_ae_comm.h"
#include "cvi_awb_comm.h"
#include "cvi_ae.h"
#include "cvi_awb.h"
#include "cvi_isp.h"

#include "imx678_cmos_ex.h"
#include "imx678_cmos_param.h"

#define DIV_0_TO_1(a) ((0 == (a)) ? 1 : (a))
#define IMX678_ID 678
#define IMX678_FULL_LINES_MAX 0x7fff
#define IMX678_HOLD_ADDR 0x3001
#define IMX678_SHR0_ADDR 0x3050
#define IMX678_GAIN_ADDR 0x3070
#define IMX678_HCG_ADDR 0x3030
#define IMX678_DGAIN_ADDR 0x3076
#define IMX678_VMAX_ADDR 0x3028
#define IMX678_RES_IS_5M(w, h) ((w) <= 2880 && (h) <= 1620)
#define IMX678_RES_IS_8M(w, h) (((w) <= 3856 && (h) <= 2180) || ((w) <= 3840 && (h) <= 2160))
#define IMX678_RES_IS_2M(w, h) ((w) <= 1920 && (h) <= 1080)
#define IMX678_FPS_EXCEEDS_MAX(fps, max) ((fps) > (max) + 0.01f)

ISP_SNS_STATE_S *g_pastImx678[VI_MAX_PIPE_NUM] = {CVI_NULL};

#define IMX678_SENSOR_GET_CTX(dev, pstCtx) (pstCtx = g_pastImx678[dev])
#define IMX678_SENSOR_SET_CTX(dev, pstCtx) (g_pastImx678[dev] = pstCtx)
#define IMX678_SENSOR_RESET_CTX(dev) (g_pastImx678[dev] = CVI_NULL)

static const IMX678_MODE_S *imx678_get_mode(VI_PIPE ViPipe)
{
	ISP_SNS_STATE_S *pstSnsState = CVI_NULL;

	IMX678_SENSOR_GET_CTX(ViPipe, pstSnsState);
	if (pstSnsState == CVI_NULL)
		return &g_astImx678_mode[IMX678_MODE_8M30];
	return &g_astImx678_mode[pstSnsState->u8ImgMode];
}

ISP_SNS_COMMBUS_U g_aunImx678_BusInfo[VI_MAX_PIPE_NUM] = {
	[0] = { .s8I2cDev = 3 },
	[1 ... VI_MAX_PIPE_NUM - 1] = { .s8I2cDev = -1 }
};

static CVI_U32 g_au32InitExposure[VI_MAX_PIPE_NUM] = {0};
static CVI_U32 g_au32LinesPer500ms[VI_MAX_PIPE_NUM] = {0};
static CVI_U16 g_au16InitWBGain[VI_MAX_PIPE_NUM][3] = {{0}};
static CVI_U16 g_au16SampleRgain[VI_MAX_PIPE_NUM] = {0};
static CVI_U16 g_au16SampleBgain[VI_MAX_PIPE_NUM] = {0};
static ISP_SNS_MIRRORFLIP_TYPE_E g_aeImx678_MirrorFip[VI_MAX_PIPE_NUM] = {0};

static CVI_S32 cmos_get_wdr_size(VI_PIPE ViPipe, ISP_SNS_ISP_INFO_S *pstIspCfg)
{
	ISP_SNS_STATE_S *pstSnsState = CVI_NULL;
	const IMX678_MODE_S *pstMode = CVI_NULL;

	IMX678_SENSOR_GET_CTX(ViPipe, pstSnsState);
	CMOS_CHECK_POINTER(pstSnsState);
	CMOS_CHECK_POINTER(pstIspCfg);

	pstMode = &g_astImx678_mode[pstSnsState->u8ImgMode];
	pstIspCfg->frm_num = 1;
	pstIspCfg->img_size[0] = pstMode->astImg[0];
	return CVI_SUCCESS;
}

static CVI_S32 cmos_get_ae_default(VI_PIPE ViPipe, AE_SENSOR_DEFAULT_S *pstAeSnsDft)
{
	ISP_SNS_STATE_S *pstSnsState = CVI_NULL;
	const IMX678_MODE_S *pstMode = CVI_NULL;

	CMOS_CHECK_POINTER(pstAeSnsDft);
	IMX678_SENSOR_GET_CTX(ViPipe, pstSnsState);
	CMOS_CHECK_POINTER(pstSnsState);
	pstMode = imx678_get_mode(ViPipe);
	CVI_U32 u32MaxFps = (CVI_U32)pstMode->f32MaxFps;

	pstAeSnsDft->u32FullLinesStd = pstSnsState->u32FLStd;
	pstAeSnsDft->u32FlickerFreq = 50 * 256;
	pstAeSnsDft->u32FullLinesMax = IMX678_FULL_LINES_MAX;
	pstAeSnsDft->u32HmaxTimes = (1000000) / DIV_0_TO_1(pstSnsState->u32FLStd * u32MaxFps);
	pstAeSnsDft->stIntTimeAccu.enAccuType = AE_ACCURACY_LINEAR;
	pstAeSnsDft->stIntTimeAccu.f32Accuracy = 1;
	pstAeSnsDft->stAgainAccu.enAccuType = AE_ACCURACY_TABLE;
	pstAeSnsDft->stAgainAccu.f32Accuracy = 1;
	pstAeSnsDft->stDgainAccu.enAccuType = AE_ACCURACY_TABLE;
	pstAeSnsDft->stDgainAccu.f32Accuracy = 1;
	pstAeSnsDft->u32ISPDgainShift = 8;
	pstAeSnsDft->u32MinISPDgainTarget = 1 << pstAeSnsDft->u32ISPDgainShift;
	pstAeSnsDft->u32MaxISPDgainTarget = 2 << pstAeSnsDft->u32ISPDgainShift;
	pstAeSnsDft->u32LinesPer500ms = g_au32LinesPer500ms[ViPipe] ?
		g_au32LinesPer500ms[ViPipe] : pstSnsState->u32FLStd * u32MaxFps / 2;
	pstAeSnsDft->u32SnsStableFrame = 8;
	pstAeSnsDft->f32Fps = pstMode->f32MaxFps;
	pstAeSnsDft->f32MinFps = pstMode->f32MinFps;
	pstAeSnsDft->au8HistThresh[0] = 0xd;
	pstAeSnsDft->au8HistThresh[1] = 0x28;
	pstAeSnsDft->au8HistThresh[2] = 0x60;
	pstAeSnsDft->au8HistThresh[3] = 0x80;
	pstAeSnsDft->u32MaxAgain = pstMode->stAgain[0].u16Max;
	pstAeSnsDft->u32MinAgain = pstMode->stAgain[0].u16Min;
	pstAeSnsDft->u32MaxAgainTarget = pstAeSnsDft->u32MaxAgain;
	pstAeSnsDft->u32MinAgainTarget = pstAeSnsDft->u32MinAgain;
	pstAeSnsDft->u32MaxDgain = pstMode->stDgain[0].u16Max;
	pstAeSnsDft->u32MinDgain = pstMode->stDgain[0].u16Min;
	pstAeSnsDft->u32MaxDgainTarget = pstAeSnsDft->u32MaxDgain;
	pstAeSnsDft->u32MinDgainTarget = pstAeSnsDft->u32MinDgain;
	pstAeSnsDft->u8AeCompensation = 40;
	pstAeSnsDft->u32InitAESpeed = 64;
	pstAeSnsDft->u32InitAETolerance = 5;
	pstAeSnsDft->u32AEResponseFrame = 4;
	pstAeSnsDft->u32SnsResponseFrame = 5;
	pstAeSnsDft->enAeExpMode = AE_EXP_HIGHLIGHT_PRIOR;
	pstAeSnsDft->u32InitExposure = g_au32InitExposure[ViPipe] ? g_au32InitExposure[ViPipe] : 76151;
	pstAeSnsDft->u32MaxIntTime = pstSnsState->u32FLStd - 1;
	pstAeSnsDft->u32MinIntTime = 4;
	pstAeSnsDft->u32MaxIntTimeTarget = pstAeSnsDft->u32MaxIntTime;
	pstAeSnsDft->u32MinIntTimeTarget = 4;

	return CVI_SUCCESS;
}

static CVI_S32 cmos_fps_set(VI_PIPE ViPipe, CVI_FLOAT f32Fps, AE_SENSOR_DEFAULT_S *pstAeSnsDft)
{
	ISP_SNS_STATE_S *pstSnsState = CVI_NULL;
	CVI_U32 u32Vts;
	ISP_SNS_REGS_INFO_S *pstSnsRegsInfo = CVI_NULL;
	const IMX678_MODE_S *pstMode = imx678_get_mode(ViPipe);

	CMOS_CHECK_POINTER(pstAeSnsDft);
	IMX678_SENSOR_GET_CTX(ViPipe, pstSnsState);
	CMOS_CHECK_POINTER(pstSnsState);

	if (f32Fps <= 0 || IMX678_FPS_EXCEEDS_MAX(f32Fps, pstMode->f32MaxFps))
		return CVI_FAILURE;

	u32Vts = pstMode->u32VtsDef * pstMode->f32MaxFps / f32Fps;
	u32Vts = (u32Vts > IMX678_FULL_LINES_MAX) ? IMX678_FULL_LINES_MAX : u32Vts;
	pstSnsState->u32FLStd = u32Vts;
	pstSnsState->au32FL[0] = u32Vts;
	pstAeSnsDft->u32FullLinesStd = u32Vts;
	pstAeSnsDft->u32MaxIntTime = u32Vts - 1;
	pstAeSnsDft->u32FullLines = u32Vts;

	pstSnsRegsInfo = &pstSnsState->astSyncInfo[0].snsCfg;
	pstSnsRegsInfo->astI2cData[LINEAR_VMAX_0].u32Data = u32Vts & 0xff;
	pstSnsRegsInfo->astI2cData[LINEAR_VMAX_1].u32Data = (u32Vts >> 8) & 0xff;
	pstSnsRegsInfo->astI2cData[LINEAR_VMAX_2].u32Data = (u32Vts >> 16) & 0x0f;

	return CVI_SUCCESS;
}

static CVI_S32 cmos_inttime_update(VI_PIPE ViPipe, CVI_U32 *u32IntTime)
{
	ISP_SNS_STATE_S *pstSnsState = CVI_NULL;
	ISP_SNS_REGS_INFO_S *pstSnsRegsInfo = CVI_NULL;
	CVI_U32 u32Shr;

	IMX678_SENSOR_GET_CTX(ViPipe, pstSnsState);
	CMOS_CHECK_POINTER(pstSnsState);
	CMOS_CHECK_POINTER(u32IntTime);
	pstSnsRegsInfo = &pstSnsState->astSyncInfo[0].snsCfg;

	u32Shr = pstSnsState->au32FL[0] - *u32IntTime;
	u32Shr = (u32Shr > (pstSnsState->au32FL[0] - 1)) ? (pstSnsState->au32FL[0] - 1) :
		((u32Shr < 4) ? 4 : u32Shr);
	pstSnsRegsInfo->astI2cData[LINEAR_SHR0_0].u32Data = u32Shr & 0xff;
	pstSnsRegsInfo->astI2cData[LINEAR_SHR0_1].u32Data = (u32Shr >> 8) & 0xff;
	pstSnsRegsInfo->astI2cData[LINEAR_SHR0_2].u32Data = (u32Shr >> 16) & 0x0f;

	return CVI_SUCCESS;
}

static CVI_S32 cmos_again_calc_table(VI_PIPE ViPipe, CVI_U32 *pu32AgainLin, CVI_U32 *pu32AgainDb)
{
	(void)ViPipe;
	CMOS_CHECK_POINTER(pu32AgainLin);
	CMOS_CHECK_POINTER(pu32AgainDb);
	if (*pu32AgainLin < 1024)
		*pu32AgainLin = 1024;
	if (*pu32AgainLin > 32381)
		*pu32AgainLin = 32381;
	*pu32AgainDb = (*pu32AgainLin - 1024) * 240 / (32381 - 1024);
	return CVI_SUCCESS;
}

static CVI_S32 cmos_dgain_calc_table(VI_PIPE ViPipe, CVI_U32 *pu32DgainLin, CVI_U32 *pu32DgainDb)
{
	(void)ViPipe;
	CMOS_CHECK_POINTER(pu32DgainLin);
	CMOS_CHECK_POINTER(pu32DgainDb);
	if (*pu32DgainLin < 1024)
		*pu32DgainLin = 1024;
	if (*pu32DgainLin > 128914)
		*pu32DgainLin = 128914;
	*pu32DgainDb = (*pu32DgainLin - 1024) * 140 / (128914 - 1024);
	return CVI_SUCCESS;
}

static CVI_S32 cmos_gains_update(VI_PIPE ViPipe, CVI_U32 *pu32Again, CVI_U32 *pu32Dgain)
{
	ISP_SNS_STATE_S *pstSnsState = CVI_NULL;
	ISP_SNS_REGS_INFO_S *pstSnsRegsInfo = CVI_NULL;
	CVI_U32 u32Again;
	CVI_U32 u32Dgain;

	IMX678_SENSOR_GET_CTX(ViPipe, pstSnsState);
	CMOS_CHECK_POINTER(pstSnsState);
	CMOS_CHECK_POINTER(pu32Again);
	CMOS_CHECK_POINTER(pu32Dgain);
	pstSnsRegsInfo = &pstSnsState->astSyncInfo[0].snsCfg;

	u32Again = pu32Again[0] > 0xf0 ? 0xf0 : pu32Again[0];
	u32Dgain = pu32Dgain[0] > 0x8c ? 0x8c : pu32Dgain[0];
	pstSnsRegsInfo->astI2cData[LINEAR_GAIN].u32Data = u32Again & 0xff;
	pstSnsRegsInfo->astI2cData[LINEAR_HCG].u32Data = 0x00;
	pstSnsRegsInfo->astI2cData[LINEAR_DGAIN].u32Data = u32Dgain & 0xff;

	return CVI_SUCCESS;
}

static CVI_S32 cmos_get_inttime_max(VI_PIPE ViPipe, CVI_U16 u16ManRatioEnable, CVI_U32 *au32Ratio,
		CVI_U32 *au32IntTimeMax, CVI_U32 *au32IntTimeMin, CVI_U32 *pu32LFMaxIntTime)
{
	ISP_SNS_STATE_S *pstSnsState = CVI_NULL;
	(void)u16ManRatioEnable;
	(void)au32Ratio;
	CMOS_CHECK_POINTER(au32IntTimeMax);
	CMOS_CHECK_POINTER(au32IntTimeMin);
	CMOS_CHECK_POINTER(pu32LFMaxIntTime);
	IMX678_SENSOR_GET_CTX(ViPipe, pstSnsState);
	CMOS_CHECK_POINTER(pstSnsState);
	au32IntTimeMax[0] = pstSnsState->au32FL[0] - 1;
	au32IntTimeMin[0] = 4;
	*pu32LFMaxIntTime = au32IntTimeMax[0];
	return CVI_SUCCESS;
}

static CVI_S32 cmos_init_ae_exp_function(AE_SENSOR_EXP_FUNC_S *pstExpFuncs)
{
	CMOS_CHECK_POINTER(pstExpFuncs);
	memset(pstExpFuncs, 0, sizeof(AE_SENSOR_EXP_FUNC_S));
	pstExpFuncs->pfn_cmos_get_ae_default = cmos_get_ae_default;
	pstExpFuncs->pfn_cmos_fps_set = cmos_fps_set;
	pstExpFuncs->pfn_cmos_inttime_update = cmos_inttime_update;
	pstExpFuncs->pfn_cmos_gains_update = cmos_gains_update;
	pstExpFuncs->pfn_cmos_again_calc_table = cmos_again_calc_table;
	pstExpFuncs->pfn_cmos_dgain_calc_table = cmos_dgain_calc_table;
	pstExpFuncs->pfn_cmos_get_inttime_max = cmos_get_inttime_max;
	return CVI_SUCCESS;
}

static CVI_S32 cmos_get_awb_default(VI_PIPE ViPipe, AWB_SENSOR_DEFAULT_S *pstAwbSnsDft)
{
	(void)ViPipe;
	CMOS_CHECK_POINTER(pstAwbSnsDft);
	memset(pstAwbSnsDft, 0, sizeof(AWB_SENSOR_DEFAULT_S));
	pstAwbSnsDft->u16InitGgain = 1024;
	pstAwbSnsDft->u8AWBRunInterval = 1;
	return CVI_SUCCESS;
}

static CVI_S32 cmos_init_awb_exp_function(AWB_SENSOR_EXP_FUNC_S *pstExpFuncs)
{
	CMOS_CHECK_POINTER(pstExpFuncs);
	memset(pstExpFuncs, 0, sizeof(AWB_SENSOR_EXP_FUNC_S));
	pstExpFuncs->pfn_cmos_get_awb_default = cmos_get_awb_default;
	return CVI_SUCCESS;
}

static CVI_S32 cmos_get_isp_default(VI_PIPE ViPipe, ISP_CMOS_DEFAULT_S *pstDef)
{
	(void)ViPipe;
	CMOS_CHECK_POINTER(pstDef);
	memset(pstDef, 0, sizeof(ISP_CMOS_DEFAULT_S));
	return CVI_SUCCESS;
}

static CVI_S32 cmos_get_blc_default(VI_PIPE ViPipe, ISP_CMOS_BLACK_LEVEL_S *pstBlc)
{
	(void)ViPipe;
	CMOS_CHECK_POINTER(pstBlc);
	memcpy(pstBlc, &g_stIspBlcCalibratio, sizeof(ISP_CMOS_BLACK_LEVEL_S));
	return CVI_SUCCESS;
}

static CVI_S32 cmos_get_sns_regs_info(VI_PIPE ViPipe, ISP_SNS_SYNC_INFO_S *pstSnsSyncInfo)
{
	ISP_SNS_STATE_S *pstSnsState = CVI_NULL;
	ISP_SNS_SYNC_INFO_S *pstCfg0 = CVI_NULL;
	ISP_SNS_SYNC_INFO_S *pstCfg1 = CVI_NULL;
	ISP_I2C_DATA_S *pstI2cData = CVI_NULL;
	CVI_U32 i;

	CMOS_CHECK_POINTER(pstSnsSyncInfo);
	IMX678_SENSOR_GET_CTX(ViPipe, pstSnsState);
	CMOS_CHECK_POINTER(pstSnsState);

	pstCfg0 = &pstSnsState->astSyncInfo[0];
	pstCfg1 = &pstSnsState->astSyncInfo[1];
	pstI2cData = pstCfg0->snsCfg.astI2cData;

	pstCfg0->snsCfg.enSnsType = SNS_I2C_TYPE;
	pstCfg0->snsCfg.u8Cfg2ValidDelayMax = 2;
	pstCfg0->snsCfg.u32RegNum = LINEAR_REGS_NUM;
	pstCfg0->snsCfg.unComBus = g_aunImx678_BusInfo[ViPipe];

	for (i = 0; i < pstCfg0->snsCfg.u32RegNum; i++) {
		pstI2cData[i].bUpdate = CVI_TRUE;
		pstI2cData[i].u8DevAddr = imx678_i2c_addr;
		pstI2cData[i].u32AddrByteNum = imx678_addr_byte;
		pstI2cData[i].u32DataByteNum = imx678_data_byte;
	}

	if (pstSnsState->bSyncInit == CVI_FALSE) {
		const IMX678_MODE_S *pstMode = imx678_get_mode(ViPipe);
		CVI_U32 u32VtsDef = pstMode->u32VtsDef;

		pstI2cData[LINEAR_HOLD].u32RegAddr = IMX678_HOLD_ADDR;
		pstI2cData[LINEAR_HOLD].u32Data = 0x01;
		pstI2cData[LINEAR_SHR0_0].u32RegAddr = IMX678_SHR0_ADDR;
		pstI2cData[LINEAR_SHR0_1].u32RegAddr = IMX678_SHR0_ADDR + 1;
		pstI2cData[LINEAR_SHR0_2].u32RegAddr = IMX678_SHR0_ADDR + 2;
		pstI2cData[LINEAR_SHR0_0].u32Data = 0x27;
		pstI2cData[LINEAR_SHR0_1].u32Data = 0x06;
		pstI2cData[LINEAR_SHR0_2].u32Data = 0x00;
		pstI2cData[LINEAR_GAIN].u32RegAddr = IMX678_GAIN_ADDR;
		pstI2cData[LINEAR_GAIN].u32Data = 0x00;
		pstI2cData[LINEAR_HCG].u32RegAddr = IMX678_HCG_ADDR;
		pstI2cData[LINEAR_HCG].u32Data = 0x00;
		pstI2cData[LINEAR_DGAIN].u32RegAddr = IMX678_DGAIN_ADDR;
		pstI2cData[LINEAR_DGAIN].u32Data = 0x00;
		pstI2cData[LINEAR_VMAX_0].u32RegAddr = IMX678_VMAX_ADDR;
		pstI2cData[LINEAR_VMAX_1].u32RegAddr = IMX678_VMAX_ADDR + 1;
		pstI2cData[LINEAR_VMAX_2].u32RegAddr = IMX678_VMAX_ADDR + 2;
		pstI2cData[LINEAR_VMAX_0].u32Data = u32VtsDef & 0xff;
		pstI2cData[LINEAR_VMAX_1].u32Data = (u32VtsDef >> 8) & 0xff;
		pstI2cData[LINEAR_VMAX_2].u32Data = 0x00;
		pstI2cData[LINEAR_REL].u32RegAddr = IMX678_HOLD_ADDR;
		pstI2cData[LINEAR_REL].u32Data = 0x00;
		cmos_get_wdr_size(ViPipe, &pstCfg0->ispCfg);
		pstCfg0->snsCfg.need_update = CVI_TRUE;
		pstCfg0->ispCfg.need_update = CVI_TRUE;
		pstSnsState->bSyncInit = CVI_TRUE;
	} else {
		pstCfg0->snsCfg.need_update = CVI_FALSE;
		for (i = 0; i < pstCfg0->snsCfg.u32RegNum; i++) {
			pstI2cData[i].bUpdate = (pstI2cData[i].u32Data != pstCfg1->snsCfg.astI2cData[i].u32Data);
			if (pstI2cData[i].bUpdate)
				pstCfg0->snsCfg.need_update = CVI_TRUE;
		}
		if (pstCfg0->snsCfg.need_update) {
			pstI2cData[LINEAR_HOLD].u32Data = 0x01;
			pstI2cData[LINEAR_HOLD].bUpdate = CVI_TRUE;
			pstI2cData[LINEAR_REL].u32Data = 0x00;
			pstI2cData[LINEAR_REL].bUpdate = CVI_TRUE;
		}
		pstCfg0->ispCfg.need_update = CVI_FALSE;
	}

	pstCfg0->snsCfg.bConfig = CVI_FALSE;
	memcpy(pstSnsSyncInfo, pstCfg0, sizeof(ISP_SNS_SYNC_INFO_S));
	memcpy(pstCfg1, pstCfg0, sizeof(ISP_SNS_SYNC_INFO_S));
	pstSnsState->au32FL[1] = pstSnsState->au32FL[0];

	return CVI_SUCCESS;
}

static CVI_S32 cmos_set_image_mode(VI_PIPE ViPipe, ISP_CMOS_SENSOR_IMAGE_MODE_S *pstSensorImageMode)
{
	ISP_SNS_STATE_S *pstSnsState = CVI_NULL;
	const IMX678_MODE_S *pstMode = CVI_NULL;
	CVI_U8 u8SensorImageMode = 0;

	CMOS_CHECK_POINTER(pstSensorImageMode);
	IMX678_SENSOR_GET_CTX(ViPipe, pstSnsState);
	CMOS_CHECK_POINTER(pstSnsState);

	pstSnsState->bSyncInit = CVI_FALSE;

	if (pstSnsState->enWDRMode != WDR_MODE_NONE) {
		CVI_TRACE_SNS(CVI_DBG_ERR, "Not support! Width:%d, Height:%d, Fps:%f, WDRMode:%d\n",
		       pstSensorImageMode->u16Width, pstSensorImageMode->u16Height,
		       pstSensorImageMode->f32Fps, pstSnsState->enWDRMode);
		return CVI_FAILURE;
	}

	if (IMX678_RES_IS_2M(pstSensorImageMode->u16Width, pstSensorImageMode->u16Height)) {
		/* u8SnsMode: 0 = crop (default), 1 = 2x2 binning (from sample/isp attr) */
		if (pstSensorImageMode->u8SnsMode == 1)
			u8SensorImageMode = IMX678_MODE_2M30_BIN;
		else
			u8SensorImageMode = IMX678_MODE_2M30;
	} else if (IMX678_RES_IS_5M(pstSensorImageMode->u16Width, pstSensorImageMode->u16Height) ||
		 IMX678_RES_IS_8M(pstSensorImageMode->u16Width, pstSensorImageMode->u16Height))
		u8SensorImageMode = IMX678_MODE_8M30;
	else {
		CVI_TRACE_SNS(CVI_DBG_ERR, "Not support! Width:%d, Height:%d, Fps:%f, WDRMode:%d\n",
		       pstSensorImageMode->u16Width, pstSensorImageMode->u16Height,
		       pstSensorImageMode->f32Fps, pstSnsState->enWDRMode);
		return CVI_FAILURE;
	}

	if (IMX678_FPS_EXCEEDS_MAX(pstSensorImageMode->f32Fps,
				   g_astImx678_mode[u8SensorImageMode].f32MaxFps)) {
		CVI_TRACE_SNS(CVI_DBG_ERR, "Not support! Width:%d, Height:%d, Fps:%f, WDRMode:%d\n",
		       pstSensorImageMode->u16Width, pstSensorImageMode->u16Height,
		       pstSensorImageMode->f32Fps, pstSnsState->enWDRMode);
		return CVI_FAILURE;
	}

	if ((pstSnsState->bInit == CVI_TRUE) && (u8SensorImageMode == pstSnsState->u8ImgMode))
		return CVI_FAILURE;

	pstSnsState->u8ImgMode = u8SensorImageMode;
	pstMode = &g_astImx678_mode[u8SensorImageMode];
	pstSnsState->u32FLStd = pstMode->u32VtsDef;
	pstSnsState->au32FL[0] = pstMode->u32VtsDef;
	pstSnsState->au32FL[1] = pstMode->u32VtsDef;
	return CVI_SUCCESS;
}

static CVI_S32 cmos_set_wdr_mode(VI_PIPE ViPipe, CVI_U8 u8Mode)
{
	ISP_SNS_STATE_S *pstSnsState = CVI_NULL;
	const IMX678_MODE_S *pstMode = CVI_NULL;

	IMX678_SENSOR_GET_CTX(ViPipe, pstSnsState);
	CMOS_CHECK_POINTER(pstSnsState);
	if (u8Mode != WDR_MODE_NONE)
		return CVI_FAILURE;
	pstSnsState->enWDRMode = WDR_MODE_NONE;
	pstMode = &g_astImx678_mode[pstSnsState->u8ImgMode];
	pstSnsState->u32FLStd = pstMode->u32VtsDef;
	pstSnsState->au32FL[0] = pstMode->u32VtsDef;
	pstSnsState->au32FL[1] = pstMode->u32VtsDef;
	pstSnsState->bSyncInit = CVI_FALSE;
	return CVI_SUCCESS;
}

static CVI_VOID sensor_mirror_flip(VI_PIPE ViPipe, ISP_SNS_MIRRORFLIP_TYPE_E eSnsMirrorFlip)
{
	ISP_SNS_STATE_S *pstSnsState = CVI_NULL;
	IMX678_SENSOR_GET_CTX(ViPipe, pstSnsState);
	CMOS_CHECK_POINTER_VOID(pstSnsState);
	if (pstSnsState->bInit == CVI_TRUE && g_aeImx678_MirrorFip[ViPipe] != eSnsMirrorFlip) {
		imx678_mirror_flip(ViPipe, eSnsMirrorFlip);
		g_aeImx678_MirrorFip[ViPipe] = eSnsMirrorFlip;
	}
}

static CVI_VOID sensor_global_init(VI_PIPE ViPipe)
{
	ISP_SNS_STATE_S *pstSnsState = CVI_NULL;
	const IMX678_MODE_S *pstMode = &g_astImx678_mode[IMX678_MODE_8M30];
	IMX678_SENSOR_GET_CTX(ViPipe, pstSnsState);
	CMOS_CHECK_POINTER_VOID(pstSnsState);
	pstSnsState->bInit = CVI_FALSE;
	pstSnsState->bSyncInit = CVI_FALSE;
	pstSnsState->u8ImgMode = IMX678_MODE_8M30;
	pstSnsState->enWDRMode = WDR_MODE_NONE;
	pstSnsState->u32FLStd = pstMode->u32VtsDef;
	pstSnsState->au32FL[0] = pstMode->u32VtsDef;
	pstSnsState->au32FL[1] = pstMode->u32VtsDef;
	memset(&pstSnsState->astSyncInfo[0], 0, sizeof(ISP_SNS_SYNC_INFO_S));
	memset(&pstSnsState->astSyncInfo[1], 0, sizeof(ISP_SNS_SYNC_INFO_S));
}

static CVI_S32 sensor_rx_attr(VI_PIPE ViPipe, SNS_COMBO_DEV_ATTR_S *pstRxAttr)
{
	ISP_SNS_STATE_S *pstSnsState = CVI_NULL;
	IMX678_SENSOR_GET_CTX(ViPipe, pstSnsState);
	CMOS_CHECK_POINTER(pstSnsState);
	CMOS_CHECK_POINTER(pstRxAttr);
	memcpy(pstRxAttr, &imx678_rx_attr, sizeof(*pstRxAttr));
	pstRxAttr->img_size.width = g_astImx678_mode[pstSnsState->u8ImgMode].astImg[0].stSnsSize.u32Width;
	pstRxAttr->img_size.height = g_astImx678_mode[pstSnsState->u8ImgMode].astImg[0].stSnsSize.u32Height;
	pstRxAttr->mipi_attr.wdr_mode = CVI_MIPI_WDR_MODE_NONE;
	/* 2x2 binning uses 10-bit AD/MIPI per Sony/Linux upstream convention */
	if (pstSnsState->u8ImgMode == IMX678_MODE_2M30_BIN)
		pstRxAttr->mipi_attr.raw_data_type = RAW_DATA_10BIT;
	return CVI_SUCCESS;
}

static CVI_S32 sensor_patch_rx_attr(RX_INIT_ATTR_S *pstRxInitAttr)
{
	SNS_COMBO_DEV_ATTR_S *pstRxAttr = &imx678_rx_attr;
	int i;
	CMOS_CHECK_POINTER(pstRxInitAttr);
	if (pstRxInitAttr->stMclkAttr.bMclkEn)
		pstRxAttr->mclk.cam = pstRxInitAttr->stMclkAttr.u8Mclk;
	if (pstRxInitAttr->MipiDev >= 2)
		return CVI_SUCCESS;
	pstRxAttr->devno = pstRxInitAttr->MipiDev;
	for (i = 0; i < MIPI_LANE_NUM + 1; i++) {
		pstRxAttr->mipi_attr.lane_id[i] = pstRxInitAttr->as16LaneId[i];
		pstRxAttr->mipi_attr.pn_swap[i] = pstRxInitAttr->as8PNSwap[i];
	}
	return CVI_SUCCESS;
}

static CVI_S32 cmos_init_sensor_exp_function(ISP_SENSOR_EXP_FUNC_S *pstSensorExpFunc)
{
	CMOS_CHECK_POINTER(pstSensorExpFunc);
	memset(pstSensorExpFunc, 0, sizeof(ISP_SENSOR_EXP_FUNC_S));
	pstSensorExpFunc->pfn_cmos_sensor_init = imx678_init;
	pstSensorExpFunc->pfn_cmos_sensor_exit = imx678_exit;
	pstSensorExpFunc->pfn_cmos_sensor_global_init = sensor_global_init;
	pstSensorExpFunc->pfn_cmos_set_image_mode = cmos_set_image_mode;
	pstSensorExpFunc->pfn_cmos_set_wdr_mode = cmos_set_wdr_mode;
	pstSensorExpFunc->pfn_cmos_get_isp_default = cmos_get_isp_default;
	pstSensorExpFunc->pfn_cmos_get_isp_black_level = cmos_get_blc_default;
	pstSensorExpFunc->pfn_cmos_get_sns_reg_info = cmos_get_sns_regs_info;
	return CVI_SUCCESS;
}

static CVI_S32 imx678_set_bus_info(VI_PIPE ViPipe, ISP_SNS_COMMBUS_U unSNSBusInfo)
{
	g_aunImx678_BusInfo[ViPipe].s8I2cDev = unSNSBusInfo.s8I2cDev;
	return CVI_SUCCESS;
}

static CVI_S32 sensor_ctx_init(VI_PIPE ViPipe)
{
	ISP_SNS_STATE_S *pastSnsStateCtx = CVI_NULL;
	IMX678_SENSOR_GET_CTX(ViPipe, pastSnsStateCtx);
	if (pastSnsStateCtx == CVI_NULL) {
		pastSnsStateCtx = (ISP_SNS_STATE_S *)malloc(sizeof(ISP_SNS_STATE_S));
		if (pastSnsStateCtx == CVI_NULL) {
			CVI_TRACE_SNS(CVI_DBG_ERR, "Isp[%d] SnsCtx malloc memory failed!\n", ViPipe);
			return -ENOMEM;
		}
	}
	memset(pastSnsStateCtx, 0, sizeof(ISP_SNS_STATE_S));
	IMX678_SENSOR_SET_CTX(ViPipe, pastSnsStateCtx);
	return CVI_SUCCESS;
}

static CVI_VOID sensor_ctx_exit(VI_PIPE ViPipe)
{
	ISP_SNS_STATE_S *pastSnsStateCtx = CVI_NULL;
	IMX678_SENSOR_GET_CTX(ViPipe, pastSnsStateCtx);
	SENSOR_FREE(pastSnsStateCtx);
	IMX678_SENSOR_RESET_CTX(ViPipe);
}

static CVI_S32 sensor_register_callback(VI_PIPE ViPipe, ALG_LIB_S *pstAeLib, ALG_LIB_S *pstAwbLib)
{
	CVI_S32 s32Ret;
	ISP_SENSOR_REGISTER_S stIspRegister;
	AE_SENSOR_REGISTER_S stAeRegister;
	AWB_SENSOR_REGISTER_S stAwbRegister;
	ISP_SNS_ATTR_INFO_S stSnsAttrInfo;
	CMOS_CHECK_POINTER(pstAeLib);
	CMOS_CHECK_POINTER(pstAwbLib);
	s32Ret = sensor_ctx_init(ViPipe);
	if (s32Ret != CVI_SUCCESS)
		return CVI_FAILURE;
	stSnsAttrInfo.eSensorId = IMX678_ID;
	s32Ret = cmos_init_sensor_exp_function(&stIspRegister.stSnsExp);
	s32Ret |= CVI_ISP_SensorRegCallBack(ViPipe, &stSnsAttrInfo, &stIspRegister);
	if (s32Ret != CVI_SUCCESS)
		return s32Ret;
	s32Ret = cmos_init_ae_exp_function(&stAeRegister.stAeExp);
	s32Ret |= CVI_AE_SensorRegCallBack(ViPipe, pstAeLib, &stSnsAttrInfo, &stAeRegister);
	if (s32Ret != CVI_SUCCESS)
		return s32Ret;
	s32Ret = cmos_init_awb_exp_function(&stAwbRegister.stAwbExp);
	s32Ret |= CVI_AWB_SensorRegCallBack(ViPipe, pstAwbLib, &stSnsAttrInfo, &stAwbRegister);
	return s32Ret;
}

static CVI_S32 sensor_unregister_callback(VI_PIPE ViPipe, ALG_LIB_S *pstAeLib, ALG_LIB_S *pstAwbLib)
{
	CVI_S32 s32Ret = CVI_SUCCESS;
	CMOS_CHECK_POINTER(pstAeLib);
	CMOS_CHECK_POINTER(pstAwbLib);
	s32Ret = CVI_ISP_SensorUnRegCallBack(ViPipe, IMX678_ID);
	s32Ret |= CVI_AE_SensorUnRegCallBack(ViPipe, pstAeLib, IMX678_ID);
	s32Ret |= CVI_AWB_SensorUnRegCallBack(ViPipe, pstAwbLib, IMX678_ID);
	sensor_ctx_exit(ViPipe);
	return s32Ret;
}

static CVI_S32 sensor_set_init(VI_PIPE ViPipe, ISP_INIT_ATTR_S *pstInitAttr)
{
	CMOS_CHECK_POINTER(pstInitAttr);
	g_au32InitExposure[ViPipe] = pstInitAttr->u32Exposure;
	g_au32LinesPer500ms[ViPipe] = pstInitAttr->u32LinesPer500ms;
	g_au16InitWBGain[ViPipe][0] = pstInitAttr->u16WBRgain;
	g_au16InitWBGain[ViPipe][1] = pstInitAttr->u16WBGgain;
	g_au16InitWBGain[ViPipe][2] = pstInitAttr->u16WBBgain;
	g_au16SampleRgain[ViPipe] = pstInitAttr->u16SampleRgain;
	g_au16SampleBgain[ViPipe] = pstInitAttr->u16SampleBgain;
	return CVI_SUCCESS;
}

static CVI_S32 sensor_probe(VI_PIPE ViPipe)
{
	return imx678_probe(ViPipe);
}

ISP_SNS_OBJ_S stSnsImx678_Obj = {
	.pfnRegisterCallback = sensor_register_callback,
	.pfnUnRegisterCallback = sensor_unregister_callback,
	.pfnStandby = imx678_standby,
	.pfnRestart = imx678_restart,
	.pfnMirrorFlip = sensor_mirror_flip,
	.pfnWriteReg = imx678_write_register,
	.pfnReadReg = imx678_read_register,
	.pfnSetBusInfo = imx678_set_bus_info,
	.pfnSetInit = sensor_set_init,
	.pfnPatchRxAttr = sensor_patch_rx_attr,
	.pfnPatchI2cAddr = CVI_NULL,
	.pfnGetRxAttr = sensor_rx_attr,
	.pfnExpSensorCb = cmos_init_sensor_exp_function,
	.pfnExpAeCb = cmos_init_ae_exp_function,
	.pfnSnsProbe = sensor_probe,
};
