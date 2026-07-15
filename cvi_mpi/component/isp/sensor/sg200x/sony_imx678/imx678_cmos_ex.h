#ifndef __IMX678_CMOS_EX_H_
#define __IMX678_CMOS_EX_H_

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif

#include <linux/cvi_type.h>
#include "cvi_sns_ctrl.h"

#define syslog(level, fmt, ...) \
	do { printf(fmt, ##__VA_ARGS__); } while (0)

enum imx678_linear_regs_e {
	LINEAR_HOLD = 0,
	LINEAR_SHR0_0,
	LINEAR_SHR0_1,
	LINEAR_SHR0_2,
	LINEAR_GAIN,
	LINEAR_HCG,
	LINEAR_DGAIN,
	LINEAR_VMAX_0,
	LINEAR_VMAX_1,
	LINEAR_VMAX_2,
	LINEAR_REL,
	LINEAR_REGS_NUM
};

typedef enum _IMX678_MODE_E {
	IMX678_MODE_8M30 = 0,	/* 5MP center crop from 4K */
	IMX678_MODE_2M30,	/* 1080p center crop (FOV shrunk) */
	IMX678_MODE_2M30_BIN,	/* 1080p 2x2 hardware binning (full FOV) */
	IMX678_MODE_NUM
} IMX678_MODE_E;

typedef struct _IMX678_MODE_S {
	ISP_WDR_SIZE_S astImg[1];
	CVI_FLOAT f32MaxFps;
	CVI_FLOAT f32MinFps;
	CVI_U32 u32HtsDef;
	CVI_U32 u32VtsDef;
	SNS_ATTR_S stExp[1];
	SNS_ATTR_S stAgain[1];
	SNS_ATTR_S stDgain[1];
	char name[64];
} IMX678_MODE_S;

extern ISP_SNS_STATE_S *g_pastImx678[VI_MAX_PIPE_NUM];
extern ISP_SNS_COMMBUS_U g_aunImx678_BusInfo[];
extern const CVI_U8 imx678_i2c_addr;
extern const CVI_U32 imx678_addr_byte;
extern const CVI_U32 imx678_data_byte;
extern void imx678_init(VI_PIPE ViPipe);
extern void imx678_exit(VI_PIPE ViPipe);
extern void imx678_standby(VI_PIPE ViPipe);
extern void imx678_restart(VI_PIPE ViPipe);
extern int imx678_write_register(VI_PIPE ViPipe, int addr, int data);
extern int imx678_read_register(VI_PIPE ViPipe, int addr);
extern void imx678_mirror_flip(VI_PIPE ViPipe, ISP_SNS_MIRRORFLIP_TYPE_E eSnsMirrorFlip);
extern int imx678_probe(VI_PIPE ViPipe);

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif

#endif
