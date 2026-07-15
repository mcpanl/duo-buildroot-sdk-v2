#ifndef __IMX678_CMOS_PARAM_H_
#define __IMX678_CMOS_PARAM_H_

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif

#include <linux/cvi_type.h>
#include "cvi_sns_ctrl.h"
#include "imx678_cmos_ex.h"

static const IMX678_MODE_S g_astImx678_mode[IMX678_MODE_NUM] = {
	[IMX678_MODE_8M30] = {
		.name = "5M30",
		.astImg[0] = {
			/*
			 * 4K sensor readout + center crop 2880x1620 for SG2000 5MP ISP limit.
			 * MIPI frame stays 3856x2180; ISP extracts via stWndRect (same as 1080p).
			 * PIX crop: HST=488, HWIDTH=2880, VST=280, VWIDTH=1620.
			 */
			.stSnsSize = { .u32Width = 3856, .u32Height = 2180 },
			.stWndRect = { .s32X = 488, .s32Y = 280, .u32Width = 2880, .u32Height = 1620 },
			.stMaxSize = { .u32Width = 3856, .u32Height = 2180 },
		},
		.f32MaxFps = 30,
		.f32MinFps = 0.01,
		.u32HtsDef = 1100,
		.u32VtsDef = 2250,
		.stExp[0] = { .u16Min = 4, .u16Max = 2249, .u16Def = 400, .u16Step = 1 },
		.stAgain[0] = { .u16Min = 1024, .u16Max = 32381, .u16Def = 1024, .u16Step = 1 },
		.stDgain[0] = { .u16Min = 1024, .u16Max = 65535, .u16Def = 1024, .u16Step = 1 },
	},
	[IMX678_MODE_2M30] = {
		.name = "1080P30",
		.astImg[0] = {
			/*
			 * PIX crop 1920x1080 @ (968,550); MIPI frame stays 3856x2180,
			 * ISP extracts active window via stWndRect (same as 4K pattern).
			 */
			.stSnsSize = { .u32Width = 3856, .u32Height = 2180 },
			.stWndRect = { .s32X = 968, .s32Y = 550, .u32Width = 1920, .u32Height = 1080 },
			.stMaxSize = { .u32Width = 3856, .u32Height = 2180 },
		},
		.f32MaxFps = 30,
		.f32MinFps = 0.01,
		.u32HtsDef = 1100,
		.u32VtsDef = 2250,
		.stExp[0] = { .u16Min = 4, .u16Max = 2249, .u16Def = 400, .u16Step = 1 },
		.stAgain[0] = { .u16Min = 1024, .u16Max = 32381, .u16Def = 1024, .u16Step = 1 },
		.stDgain[0] = { .u16Min = 1024, .u16Max = 65535, .u16Def = 1024, .u16Step = 1 },
	},
	[IMX678_MODE_2M30_BIN] = {
		.name = "1080P30_BIN",
		.astImg[0] = {
			/*
			 * 2x2 hardware binning: sensor PIX window 3840x2160 with ADDMODE=1
			 * yields MIPI ~1920x1080 RAW10 (full FOV). Adjust stSnsSize if
			 * mipi-rx reports padding (e.g. 1936x1088).
			 */
			.stSnsSize = { .u32Width = 1920, .u32Height = 1080 },
			.stWndRect = { .s32X = 0, .s32Y = 0, .u32Width = 1920, .u32Height = 1080 },
			.stMaxSize = { .u32Width = 1920, .u32Height = 1080 },
		},
		.f32MaxFps = 30,
		.f32MinFps = 0.01,
		.u32HtsDef = 1100,
		.u32VtsDef = 2250,
		.stExp[0] = { .u16Min = 4, .u16Max = 2249, .u16Def = 400, .u16Step = 1 },
		.stAgain[0] = { .u16Min = 1024, .u16Max = 32381, .u16Def = 1024, .u16Step = 1 },
		.stDgain[0] = { .u16Min = 1024, .u16Max = 65535, .u16Def = 1024, .u16Step = 1 },
	},
};

static ISP_CMOS_BLACK_LEVEL_S g_stIspBlcCalibratio = {
	.bUpdate = CVI_TRUE,
	.blcAttr = {
		.Enable = 1,
		.enOpType = OP_TYPE_AUTO,
		.stManual = {200, 200, 200, 200, 0, 0, 0, 0},
		.stAuto = {
			{200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200},
			{200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200},
			{200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200},
			{200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200, 200},
		},
	},
};

struct combo_dev_attr_s imx678_rx_attr = {
	.input_mode = INPUT_MODE_MIPI,
	/* IMX678 4K12 linear: 1188 Mbps/lane -> link_freq 594 MHz (RX_MAC_CLK_600M) */
	.mac_clk = RX_MAC_CLK_600M,
	.mipi_attr = {
		.raw_data_type = RAW_DATA_12BIT,
		.lane_id = {0, 1, 3, 2, 4},
		.pn_swap = {1, 1, 1, 1, 1},
		.wdr_mode = CVI_MIPI_WDR_MODE_NONE,
		.dphy = { .enable = 1, .hs_settle = 14 },
	},
	.mclk = { .cam = 0, .freq = CAMPLL_FREQ_37P125M },
	.devno = 0,
};

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif

#endif
