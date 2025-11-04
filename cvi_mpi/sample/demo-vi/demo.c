#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <inttypes.h>
#include "cvi_vo.h"
#include "sample_comm.h"
#include "cvi_mipi_tx.h"
#include "demo.h"
#include "cvi_sys.h"
#include "cvi_vb.h"
#include <stdint.h> 


#include "cvi_buffer.h"
#include "cvi_ae_comm.h"
#include "cvi_awb_comm.h"
#include "cvi_comm_isp.h"


#define ALIGN_UP(x, align) (((x) + (align) - 1) & ~((align) - 1))

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


int m_vi_get_frame()
{
	printf("** vi get frame **\n");
	return 0;
}

int m_vi_init()
{
	printf("** vi init **\n");


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


	return 0;
}

int m_vi_deinit()
{
	printf("** vi deinit **\n");
	return 0;
}

int __main()
{
	return 0;
}
