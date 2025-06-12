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

#define ALIGN_UP(x, align) (((x) + (align) - 1) & ~((align) - 1))

#define WIDTH  768
#define HEIGHT 1280

#define COLOR_R 255
#define COLOR_G 0
#define COLOR_B 0

static int fd;

#define MAX_OPTIONS	128
#define LANE_MAX_NUM   5

typedef enum _ARG_TYPE_ {
	ARG_INT = 0,
	ARG_UINT,
	ARG_STRING,
	ARG_NONE
} ARG_TYPE;

typedef struct _optionExt_ {
	struct option opt;
	int type;
	int64_t min;
	int64_t max;
	const char *help;
} optionExt;

typedef enum {
	DSI_PANEL_MILKV_8HD,
	PANEL_MAX
} PANEL_MODEL;

typedef struct _inputPara_ {
	enum mipi_tx_lane_id    lane_id[LANE_MAX_NUM];
	bool                    lane_pn_swap[LANE_MAX_NUM];
	bool					lane_id_flag;
	bool					pn_swap_flag;
	CVI_U8					dev_no;
	PANEL_MODEL			panel_model;
} inputPara;

inputPara g_input_para = {
	.panel_model = DSI_PANEL_MILKV_8HD,
	.dev_no = 0,
};

static struct panel_desc_s g_panel_desc = {
	.panel_mode = "MILKV_8HD",
	.panel_type = PANEL_MODE_DSI,
	.stdsicfg.dev_cfg = &dev_cfg_milkv_8hd_800x1280,
	.stdsicfg.hs_timing_cfg = &hs_timing_cfg_milkv_8hd_800x1280,
	.stdsicfg.dsi_init_cmds = dsi_init_cmds_milkv_8hd_800x1280,
	.stdsicfg.dsi_init_cmds_size = ARRAY_SIZE(dsi_init_cmds_milkv_8hd_800x1280)
};

static optionExt long_option_ext[] = {
	{{"test", no_argument, NULL, 't'}, ARG_NONE, 0, 0,
                "test screen demo."},
        {{"init", no_argument, NULL, 'i'}, ARG_NONE, 0, 0,
                "initialize and enable the panel manually."},
	{{"panel",  optional_argument, NULL, 'm'},   ARG_STRING,   0,   0,
		"choose diaply panel model"},
	{{"laneid",    optional_argument, NULL, 'l'},   ARG_STRING,    0,   0,
		"laneid sequence by order"},
	{{"pnswap",    optional_argument, NULL, 'p'},   ARG_STRING,   0,   0,
		"pnswap sequence by order"},
	{{"dsi-control",     no_argument, NULL, 'd'}, ARG_STRING, 0,   0,
		"set/get dsi status or settings." },
	{{"show-pattern", optional_argument, NULL, 's'}, ARG_STRING, 0,   0,
		"show colorbar or snow or rgb color pattern." },
	{{"help",      no_argument, NULL, 'h'},       ARG_STRING, 0,   0,
		"print usage."},
	{{NULL, 0, NULL, 0}, ARG_INT, 0, 0, "no param: just init the panel."}
};

static char *s_panel_model_type_arr[] = {
	"MILKV_8HD"
};

void printdsiHelp(void)
{
	printf("\n// ------------------------dsi-control------------------------\n");
	printf(" 0: dcs send\n");
	printf(" 1: dcs get\n");
	printf(" 2: switch to lp\n");
	printf(" 3: switch to hs\n");
	printf(" 4: get hs settle settings\n");
	printf(" 5: set hs settle settings\n");
}

void printPatternHelp(void)
{
	printf("\n// ------------------------show-pattern------------------------\n");
	printf(" 0: VO_PAT_OFF\n");
	printf(" 1: VO_PAT_SNOW\n");
	printf(" 2: VO_PAT_AUTO\n");
	printf(" 3: VO_PAT_RED\n");
	printf(" 4: VO_PAT_GREEN\n");
	printf(" 5: VO_PAT_BLUE\n");
	printf(" 6: VO_PAT_COLORBAR\n");
	printf(" 7: VO_PAT_GRAY_GRAD_H\n");
	printf(" 8: VO_PAT_GRAY_GRAD_V\n");
	printf(" 9: VO_PAT_BLACK\n");
}

void printHelp(char **argv)
{
	CVI_U32 idx;

	printf("// ------------------------help------------------------\n");
	printf("\noptional panel mode support list:\n");
	for (idx = 0; idx < sizeof(s_panel_model_type_arr) / sizeof(char *); idx++) {
		printf(" %s\n", s_panel_model_type_arr[idx]);
	}

	printf("\n.for mipi/lvds panel you can cfg lane seq or pnswap");
	printf("\nEX.\n");
	printf(" %s --panel=MILKV_8HD --laneid=1,2,0,3,4 --pnswap=0,0,0,0,0\n", argv[0]);
	printf("\n.for mipi panel You can also manually set the dsi by -d");
	printf("\nEX.\n");
	printf(" %s -d\n", argv[0]);
	printf("\n.After initializing panel, to show specific pattern by --show-pattern");
	printf("\nEX.\n");
	printf(" %s --panel=MILKV_8HD --show-pattern=6\n", argv[0]);
	printf("\n.After initializing panel, to show any kind of pattern by -s");
	printf("\nEX.\n");
	printf(" %s --panel=MILKV_8HD -s\n\n", argv[0]);
	printf("\n.explicitly initialize and enable panel:\n");
	printf(" %s --init --panel=MILKV_8HD\n", argv[0]);
	

	for (idx = 0; idx < sizeof(long_option_ext) / sizeof(optionExt); idx++) {
		if (long_option_ext[idx].opt.name == NULL) {
			break;
		}

		printf("--%s\n", long_option_ext[idx].opt.name);
		printf("    %s\n", long_option_ext[idx].help);
	}

	printf("// ------------------------------------------------\n");
}

int dsi_init(int devno, const struct dsc_instr *cmds, int size)
{
	int ret;

	if (cmds == NULL) {
		return CVI_FAILURE;
	}

	for (int i = 0; i < size; i++) {
		const struct dsc_instr *instr = &cmds[i];
		struct cmd_info_s cmd_info = {
			.devno = devno,
			.cmd_size = instr->size,
			.data_type = instr->data_type,
			.cmd = (void *)instr->data
		};

		ret = CVI_MIPI_TX_SendCmd(fd, &cmd_info);
		if (instr->delay)
			usleep(instr->delay * 1000);

		if (ret) {
			printf("dsi init failed at %d instr.\n", i);
			return ret;
		}
	}
	return ret;
}

CVI_S32 SAMPLE_MIPI_TX_ENABLE(void)
{
	CVI_S32 ret = 0;

	fd = open(MIPI_TX_NAME, O_RDWR | O_NONBLOCK, 0);
	if (fd == -1) {
		printf("Cannot open '%s': %d, %s\n", MIPI_TX_NAME, errno, strerror(errno));
		return CVI_FAILURE;
	}

	ret = CVI_MIPI_TX_Disable(fd);
	if (ret != CVI_SUCCESS) {
		printf("CVI_MIPI_TX_Disable fail!\n");
		return CVI_FAILURE;
	}

	ret = CVI_MIPI_TX_Cfg(fd, (struct combo_dev_cfg_s *)g_panel_desc.stdsicfg.dev_cfg);
	if (ret != CVI_SUCCESS) {
		printf("CVI_MIPI_TX_Cfg fail!\n");
		return CVI_FAILURE;
	}
	ret = dsi_init(0, g_panel_desc.stdsicfg.dsi_init_cmds, g_panel_desc.stdsicfg.dsi_init_cmds_size);
	if (ret != CVI_SUCCESS) {
		printf("dsi_init fail!\n");
		return CVI_FAILURE;
	}

	ret = CVI_MIPI_TX_SetHsSettle(fd, g_panel_desc.stdsicfg.hs_timing_cfg);
	if (ret != CVI_SUCCESS) {
		printf("CVI_MIPI_TX_SetHsSettle fail!\n");
		return CVI_FAILURE;
	}

	ret = CVI_MIPI_TX_Enable(fd);
	if (ret != CVI_SUCCESS) {
		printf("CVI_MIPI_TX_Enable fail!\n");
		return CVI_FAILURE;
	}

	printf("Init for MIPI-Driver-%s\n", g_panel_desc.panel_mode);

	close(fd);

	return CVI_SUCCESS;
}

CVI_S32 SAMPLE_PANEL_ShowPattern(CVI_S32 patern_cmd)
{
	CVI_S32 ret = 0;
	VO_DEV VoDev = 0;

	if (patern_cmd >= 0 && patern_cmd < VO_PAT_MAX) {
		ret = CVI_VO_ShowPattern(VoDev, patern_cmd);
		if (ret != CVI_SUCCESS) {
			printf("CVI_VO_ShowPattern failed with %#x!\n", ret);
			return ret;
		}
		sleep(2);
	} else if (patern_cmd == VO_PAT_MAX) {
		do {
			printPatternHelp();
			printf(" others: exit\n");
			scanf("%d", &patern_cmd);
			if (patern_cmd >= 0 && patern_cmd < VO_PAT_MAX) {
				ret = CVI_VO_ShowPattern(VoDev, patern_cmd);
				if (ret != CVI_SUCCESS) {
					printf("CVI_VO_ShowPattern failed with %#x!\n", ret);
					return CVI_FAILURE;
				}
			} else {
				break;
			}
		} while (1);
	} else {
		printf("invalid pattern mode parameter\n");
		return ret;
	}

	ret = CVI_VO_ShowPattern(VoDev, VO_PAT_OFF);
	if (ret != CVI_SUCCESS) {
		printf("CVI_VO_ShowPattern failed with %#x!\n", ret);
		return CVI_FAILURE;
	}

	return CVI_SUCCESS;
}

CVI_S32 SAMPLE_PANEL_ENABLE(void)
{
	CVI_S32 ret = 0;
	VO_DEV VoDev = 0;

	if (g_panel_desc.panel_type == PANEL_MODE_DSI) {
		ret = SAMPLE_MIPI_TX_ENABLE();
		if (ret != CVI_SUCCESS) {
			printf("SAMPLE_MIPI_TX_ENABLE fail!\n");
			return CVI_FAILURE;
		}
	} else {
		ret = CVI_VO_SetPubAttr(VoDev, &g_panel_desc.stVoPubAttr);
		if (ret != CVI_SUCCESS) {
			printf("failed with %#x!\n", ret);
			return CVI_FAILURE;
		}
		printf("Init for Driver-%s\n", g_panel_desc.panel_mode);
	}

	return CVI_SUCCESS;
}

void SAMPLE_DSI_CONTROLE(void)
{
	CVI_U32 tmp;

	fd = open(MIPI_TX_NAME, O_RDWR | O_NONBLOCK, 0);
	if (fd == -1) {
		printf("Cannot open '%s': %d, %s\n", MIPI_TX_NAME, errno, strerror(errno));
	}

	do {
		printdsiHelp();
		printf(" others: exit\n");
		scanf("%d", &tmp);
		if (tmp == 0) {
			struct cmd_info_s cmd_info;
			CVI_U8 data[16] = { 0 };
			int len = 0;

			printf("data size:\n");
			scanf("%d", &tmp);
			cmd_info.cmd_size = tmp;

			printf("data type: 0x\n");
			scanf("%x", &tmp);
			cmd_info.data_type = tmp;

			do {
				printf("data[%d]: 0x\n", len);
				scanf("%x", &tmp);
				data[len++] = tmp;
			} while (len < cmd_info.cmd_size);
			cmd_info.cmd = data;
			CVI_MIPI_TX_SendCmd(fd, &cmd_info);
		} else if (tmp == 1) {
			struct get_cmd_info_s cmd_info;
			CVI_U8 data[4] = { 0 };

			printf("get data size:\n");
			scanf("%d", &tmp);
			cmd_info.get_data_size = tmp;

			printf("data type: 0x\n");
			scanf("%x", &tmp);
			cmd_info.data_type = tmp;

			printf("data param: 0x\n");
			scanf("%x", &tmp);
			cmd_info.data_param = tmp;

			cmd_info.get_data = data;
			CVI_MIPI_TX_RecvCmd(fd, &cmd_info);
			printf("data[0]: %#x [1]: %#x [2]: %#x [3]: %#x\n"
				, cmd_info.get_data[0], cmd_info.get_data[1]
				, cmd_info.get_data[2], cmd_info.get_data[3]);
		} else if (tmp == 2) {
			CVI_MIPI_TX_Disable(fd);
		} else if (tmp == 3) {
			CVI_MIPI_TX_Enable(fd);
		} else if (tmp == 4) {
			struct hs_settle_s hs_cfg;

			CVI_MIPI_TX_GetHsSettle(fd, &hs_cfg);
			printf("prepare(%d) zero(%d) trail(%d)\n",
				hs_cfg.prepare, hs_cfg.zero, hs_cfg.trail);
		} else if (tmp == 5) {
			struct hs_settle_s hs_cfg;

			printf("prepare:\n");
			scanf("%d", &tmp);
			hs_cfg.prepare = tmp;

			printf("zero:\n");
			scanf("%d", &tmp);
			hs_cfg.zero = tmp;

			printf("trail:\n");
			scanf("%d", &tmp);
			hs_cfg.trail = tmp;

			CVI_MIPI_TX_SetHsSettle(fd, &hs_cfg);
		} else
			break;
	} while (1);

	close(fd);
}

void SAMPLE_SET_PANEL_DESC(void)
{
	switch (g_input_para.panel_model) {
	case DSI_PANEL_MILKV_8HD:
		g_panel_desc.panel_type = PANEL_MODE_DSI;
		g_panel_desc.stdsicfg.dev_cfg = &dev_cfg_milkv_8hd_800x1280;
		g_panel_desc.stdsicfg.hs_timing_cfg = &hs_timing_cfg_milkv_8hd_800x1280;
		g_panel_desc.stdsicfg.dsi_init_cmds = dsi_init_cmds_milkv_8hd_800x1280;
		g_panel_desc.stdsicfg.dsi_init_cmds_size = ARRAY_SIZE(dsi_init_cmds_milkv_8hd_800x1280);
		break;
	default:
		printf("default\n");
		g_panel_desc.panel_type = PANEL_MODE_DSI;
		g_panel_desc.stdsicfg.dev_cfg = &dev_cfg_milkv_8hd_800x1280;
		g_panel_desc.stdsicfg.hs_timing_cfg = &hs_timing_cfg_milkv_8hd_800x1280;
		g_panel_desc.stdsicfg.dsi_init_cmds = dsi_init_cmds_milkv_8hd_800x1280;
		g_panel_desc.stdsicfg.dsi_init_cmds_size = ARRAY_SIZE(dsi_init_cmds_milkv_8hd_800x1280);
		break;
	}
	if (g_input_para.pn_swap_flag) {
		if (g_panel_desc.panel_type == PANEL_MODE_LVDS) {
			for (CVI_U32 i = 0; i < LANE_MAX_NUM; i++) {
				g_panel_desc.stVoPubAttr.stLvdsAttr.lane_pn_swap[i] =
				(enum VO_LVDS_LANE_ID)g_input_para.lane_pn_swap[i];
			}
		} else if (g_panel_desc.panel_type == PANEL_MODE_DSI) {
			for (CVI_U32 i = 0; i < LANE_MAX_NUM; i++) {
				g_panel_desc.stdsicfg.dev_cfg->lane_pn_swap[i] = g_input_para.lane_pn_swap[i];
			}
		}
	}
	if (g_input_para.lane_id_flag) {
		if (g_panel_desc.panel_type == PANEL_MODE_LVDS) {
			for (CVI_U32 i = 0; i < LANE_MAX_NUM; i++) {
				g_panel_desc.stVoPubAttr.stLvdsAttr.lane_id[i] =
				(enum VO_LVDS_LANE_ID)g_input_para.lane_id[i];
			}
		} else if (g_panel_desc.panel_type == PANEL_MODE_DSI) {
			for (CVI_U32 i = 0; i < LANE_MAX_NUM; i++) {
				g_panel_desc.stdsicfg.dev_cfg->lane_id[i] = g_input_para.lane_id[i];
			}
		}
	}
}

CVI_S32 SAMPLE_SET_PANEL_MODEL(char *pinput_str)
{
	CVI_S32 i = 0;
	bool is_find = false;
	PANEL_MODEL panel_model = DSI_PANEL_MILKV_8HD;

	for (i = 0; i < PANEL_MAX; i++) {
		if (strcmp(pinput_str, s_panel_model_type_arr[i]) == 0) {
			is_find = true;
			break;
		}
	}

	if (is_find) {
		panel_model = (PANEL_MODEL)i;
	} else {
		return CVI_FAILURE;
	}

	g_input_para.panel_model = panel_model;
	g_panel_desc.panel_mode = s_panel_model_type_arr[i];
	return CVI_SUCCESS;
}

CVI_S32 SAMPLE_SET_LANEID(char *pLaneid)
{
	CVI_S32 lane_id[] = {0, 0, 0, 0, 0};

	if (pLaneid == NULL)
		return CVI_FAILURE;

	CVI_S32 n = sscanf(pLaneid, "%02d,%02d,%02d,%02d,%02d",
		&lane_id[0], &lane_id[1], &lane_id[2], &lane_id[3], &lane_id[4]);

	if (n != sizeof(lane_id)/sizeof(CVI_S32)) {
		return CVI_FAILURE;
	}
	g_input_para.lane_id_flag = true;
	for (CVI_U32 i = 0; i < sizeof(lane_id)/sizeof(CVI_S32); i++) {
		if (lane_id[i] < -1 || lane_id[i] > 5) {
			return CVI_FAILURE;
		}
		g_input_para.lane_id[i] = lane_id[i];
	}

	return CVI_SUCCESS;
}

CVI_S32 SAMPLE_SET_PNSWAP(char *pPnswap)
{
	CVI_U32 pnswap[] = {0, 0, 0, 0, 0};

	if (pPnswap == NULL)
		return CVI_FAILURE;

	CVI_S32 n = sscanf(pPnswap, "%02d,%02d,%02d,%02d,%02d",
		&pnswap[0], &pnswap[1], &pnswap[2], &pnswap[3], &pnswap[4]);

	if (n != sizeof(pnswap)/sizeof(CVI_U32)) {
		return CVI_FAILURE;
	}
	g_input_para.pn_swap_flag = true;
	for (CVI_U32 i = 0; i < sizeof(pnswap)/sizeof(CVI_U32); i++) {
		if ((pnswap[i] != 0) && (pnswap[i] != 1)) {
			return CVI_FAILURE;
		}
		g_input_para.lane_pn_swap[i] = (bool)(pnswap[i]);
	}

	return CVI_SUCCESS;
}

void SAMPLE_PANEL_I2C_SEND(void)
{
	printf("...");
}

void _do_test(void)
{
	VO_LAYER layer = 0;  // 视频层编号
    	VO_VIDEO_LAYER_ATTR_S layerAttr;

    	// 获取视频层属性
    	CVI_S32 ret = CVI_VO_GetVideoLayerAttr(layer, &layerAttr);
    	if (ret == 0) {
        	printf("获取视频层属性成功。\n");
        	// 你可以在这里访问 layerAttr 中的字段
		printf("更新张数：%d", layerAttr.u32DispFrmRt);
        	printf("宽度: %d, 高度: %d\n", layerAttr.stDispRect.u32Width, layerAttr.stDispRect.u32Height);
    	} else {
       		printf("获取视频层属性失败，错误码: %d\n", ret);
   	}

}

void __do_test(void)
{
	VO_DEV voDev = 0;
	VO_PUB_ATTR_S voAttr;

	CVI_S32 ret = CVI_VO_GetPubAttr(voDev, &voAttr);

        if (ret == 0) {
                printf("获取视频设备属性成功。\n");
		printf("接口类型: %d \n", voAttr.enIntfType);
		printf("标准时序: %d \n", voAttr.enIntfSync);
	} else {
                printf("获取视频设备属性失败，错误码: %d\n", ret);
        }

}

CVI_S32 init_custom_vb_pool_once(VB_CONFIG_S *pstVbConfig) {
	CVI_S32 s32Ret = CVI_FAILURE;

	CVI_SYS_Exit();
	CVI_VB_Exit();

	if (pstVbConfig == NULL) {
		CVI_TRACE_LOG(CVI_DBG_ERR, "input parameter is null, it is invaild!\n");
		return CVI_FAILURE;
	}

	s32Ret = CVI_VB_SetConfig(pstVbConfig);
	if (s32Ret != CVI_SUCCESS) {
		CVI_TRACE_LOG(CVI_DBG_ERR, "CVI_VB_SetConf failed!\n");
		return s32Ret;
	}

	s32Ret = CVI_VB_Init();
	if (s32Ret != CVI_SUCCESS) {
		CVI_TRACE_LOG(CVI_DBG_ERR, "CVI_VB_Init failed!\n");
		return s32Ret;
	}

	s32Ret = CVI_SYS_Init();
	if (s32Ret != CVI_SUCCESS) {
		CVI_TRACE_LOG(CVI_DBG_ERR, "CVI_SYS_Init failed!\n");
		CVI_VB_Exit();
		return s32Ret;
	}

	return CVI_SUCCESS;
}

void delay_ms(int milliseconds) {
    usleep(milliseconds * 1000);  // 微秒级延迟（已废弃但广泛支持）
    // 或使用更现代的 nanosleep
    struct timespec ts = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (milliseconds % 1000) * 1000000
    };
    nanosleep(&ts, NULL);
}

CVI_S32 do_test(void)
{
	printf("== RUN DO TEST ==\n");

	CVI_S32 s32Ret = CVI_SUCCESS;

	// Start VO
	
	VO_PUB_ATTR_S pstPubAttr;
	memset(&pstPubAttr, 0, sizeof(pstPubAttr));
	pstPubAttr.enIntfType = VO_INTF_MIPI;
	pstPubAttr.enIntfSync = VO_OUTPUT_720P60;
	pstPubAttr.u32BgColor = 0xFFF000; // 黑色背景

	VO_DEV VoDev = 0;

	s32Ret = CVI_VO_SetPubAttr(VoDev, &pstPubAttr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("failed with %#x!\n", s32Ret);
		return CVI_FAILURE;
	}

	s32Ret = CVI_VO_Enable(VoDev);
	if (s32Ret != CVI_SUCCESS) {
  		SAMPLE_PRT("failed with %#x!\n", s32Ret);
  		return CVI_FAILURE;
	}

	sleep(1);

	// Start Layer
	VO_LAYER voLayer = 0;

        VO_VIDEO_LAYER_ATTR_S pstLayerAttr;
        memset(&pstLayerAttr, 0, sizeof(pstLayerAttr));

	pstLayerAttr.stDispRect.s32X = 0;
	pstLayerAttr.stDispRect.s32Y = 0;
	pstLayerAttr.stDispRect.u32Width = WIDTH;
	pstLayerAttr.stDispRect.u32Height = HEIGHT;

	pstLayerAttr.stImageSize.u32Width = WIDTH;
	pstLayerAttr.stImageSize.u32Height = HEIGHT;

	pstLayerAttr.enPixFormat = PIXEL_FORMAT_RGB_888; 
	pstLayerAttr.u32DispFrmRt = 60;

	s32Ret = CVI_VO_SetVideoLayerAttr(voLayer, &pstLayerAttr);
	if (s32Ret != CVI_SUCCESS) {
		SAMPLE_PRT("failed with %#x!\n", s32Ret);
		return CVI_FAILURE;
	}

	s32Ret = CVI_VO_EnableVideoLayer(voLayer);
        if (s32Ret != CVI_SUCCESS) {
                SAMPLE_PRT("failed with %#x!\n", s32Ret);
                return CVI_FAILURE;
        }

	sleep(1);

	// Start Channel
	VO_CHN_ATTR_S stChnAttr;
	memset(&stChnAttr, 0, sizeof(stChnAttr));

	stChnAttr.stRect.s32X = 0;
	stChnAttr.stRect.s32Y = 0;
	stChnAttr.stRect.u32Width = WIDTH;
	stChnAttr.stRect.u32Height = HEIGHT;

	stChnAttr.u32Priority = 0;

	VO_CHN voChn = 0;

	CVI_VO_SetChnAttr(voLayer, voChn, &stChnAttr);
	CVI_VO_EnableChn(voLayer, voChn);
	CVI_VO_ShowChn(voLayer, voChn);

	sleep(1);
	
	// init vb
	CVI_U32 u32BlkSize = WIDTH * 3 * HEIGHT;
	
	CVI_SYS_Exit();
	CVI_VB_Exit();

	VB_CONFIG_S pstVbConfig;
	memset(&pstVbConfig, 0, sizeof(VB_CONFIG_S));

	// 公共视频区块池数量
	pstVbConfig.u32MaxPoolCnt		= 1;

	// 第一个区块池 - 视频区块大小
	pstVbConfig.astCommPool[0].u32BlkSize	= u32BlkSize;
	// 第一个区块池 - 视频区块池内的区块数
	pstVbConfig.astCommPool[0].u32BlkCnt	= 8;
	SAMPLE_PRT("common pool[0] BlkSize %d\n", u32BlkSize);


	s32Ret = CVI_VB_SetConfig(&pstVbConfig);

	if (s32Ret != CVI_SUCCESS) {
	    SAMPLE_PRT("CVI_VB_SetConf failed!\n");
	    return CVI_FAILURE;
	}

	s32Ret = CVI_VB_Init();

	if (s32Ret != CVI_SUCCESS) {
	    SAMPLE_PRT("CVI_VB_Init failed!\n");
	    return CVI_FAILURE;
	}

	s32Ret = CVI_SYS_Init();

	if (s32Ret != CVI_SUCCESS) {
	    SAMPLE_PRT("CVI_SYS_Init failed!\n");
	    CVI_VB_Exit();
	    return CVI_FAILURE;
	}

	// Get a Block
	// VB_BLK CVI_VB_GetBlock(VB_POOL Pool, CVI_U32 u32BlkSize);

	
	printf("try get block\n");
	VB_BLK blk = CVI_VB_GetBlock(VB_INVALID_POOLID, u32BlkSize);
	
	if(blk == VB_INVALID_HANDLE) {
		SAMPLE_PRT("get block fail\n");
		return CVI_FAILURE;
	}
	
	printf("get block success %lld\n", blk);
	
	// get address by block
	
	CVI_U64 physAddr = CVI_VB_Handle2PhysAddr(blk);
	CVI_VOID *virtAddr = CVI_SYS_Mmap(physAddr, u32BlkSize);
	if (virtAddr == NULL) {
	    SAMPLE_PRT("CVI_SYS_Mmap failed\n");
	    CVI_VB_ReleaseBlock(blk);
	    return CVI_FAILURE;
	}

	printf("get address success %lld\n", physAddr);
	
	sleep(1);

	memset(virtAddr, 0, u32BlkSize);

for (int i = 0; i < WIDTH * HEIGHT; i++) {
    ((CVI_U8 *)virtAddr)[i * 3 + 0] = 255; // R
    ((CVI_U8 *)virtAddr)[i * 3 + 1] = 255;   // G
    ((CVI_U8 *)virtAddr)[i * 3 + 2] = 0;   // B
}

	// make frame
	VIDEO_FRAME_INFO_S stFrame;
    	memset(&stFrame, 0, sizeof(stFrame));
    	stFrame.stVFrame.u32Width = WIDTH;
    	stFrame.stVFrame.u32Height = HEIGHT;
    	stFrame.stVFrame.enPixelFormat = PIXEL_FORMAT_RGB_888;
    	stFrame.stVFrame.u64PhyAddr[0] = physAddr;
    	stFrame.stVFrame.pu8VirAddr[0] = virtAddr;
	stFrame.stVFrame.u32Length[0] = u32BlkSize;
    	stFrame.stVFrame.u32Stride[0] = WIDTH * 3;
	stFrame.stVFrame.u32TimeRef = 1;

	stFrame.u32PoolId = CVI_VB_Handle2PoolId(blk);

	if(VB_INVALID_POOLID == stFrame.u32PoolId) {
		SAMPLE_PRT("Frame poolId fail\n");
		CVI_VB_ReleaseBlock(blk);
		return CVI_FAILURE;
	}

	printf("Frame poolId success %d\n", stFrame.u32PoolId);

CVI_U32 colors[][3] = {
    {255, 0, 0},     // Red
    {0, 255, 0},     // Green
    {0, 0, 255},     // Blue
    {255, 255, 0},   // Yellow
    {0, 255, 255},   // Cyan
    {255, 0, 255},   // Magenta
    {255, 255, 255}, // White
    {0, 0, 0}        // Black
};

int num_colors = sizeof(colors) / sizeof(colors[0]);

while (1) {
    for (int c = 0; c < num_colors; ++c) {
        CVI_U8 R = colors[c][0];
        CVI_U8 G = colors[c][1];
        CVI_U8 B = colors[c][2];

        // 填充颜色
        for (int i = 0; i < WIDTH * HEIGHT; i++) {
            ((CVI_U8 *)virtAddr)[i * 3 + 0] = R;
            ((CVI_U8 *)virtAddr)[i * 3 + 1] = G;
            ((CVI_U8 *)virtAddr)[i * 3 + 2] = B;
        }

        // 设置时间戳递增（可选）
        stFrame.stVFrame.u32TimeRef++;

        // 发送帧
        CVI_S32 ret = CVI_VO_SendFrame(voLayer, voChn, &stFrame, 0);
        if (ret != CVI_SUCCESS) {
            SAMPLE_PRT("Send frame failed: %d\n", ret);
        } else {
            printf("发送颜色帧 R:%d G:%d B:%d\n", R, G, B);
        }

        delay_ms(200); // 每帧延迟 1 秒
    }
}


    	CVI_VO_SendFrame(voLayer, voChn, &stFrame, 0);

	printf("已发送帧数据\n");
	sleep(5);

	printf("开始销毁\n");

CVI_SYS_Munmap(virtAddr, u32BlkSize);
CVI_VB_ReleaseBlock(blk);
CVI_VO_DisableChn(voLayer, voChn);
CVI_VO_DisableVideoLayer(voLayer);
CVI_VO_Disable(VoDev);
CVI_SYS_Exit();
CVI_VB_Exit();

	printf("== END DO TEST ==\n");
	return s32Ret;
}

int main(int argc, char *argv[])
{
	if (argc == 1) {
		printHelp(argv);
		return CVI_SUCCESS;
	}

	struct option long_options[MAX_OPTIONS + 1];
	CVI_S32 ch, idx, ret, patern_cmd = VO_PAT_MAX;
	bool is_pattern = false;
	bool do_init = false;

	memset((void *)long_options, 0, sizeof(long_options));

	for (idx = 0; idx < MAX_OPTIONS; idx++) {
		if (long_option_ext[idx].opt.name == NULL)
			break;

		if (idx >= MAX_OPTIONS) {
			printf("too many options\n");
			return -1;
		}

		memcpy(&long_options[idx], &long_option_ext[idx].opt, sizeof(struct option));
	}

	optind = 0;
	while ((ch = getopt_long(argc, argv, "dhs", long_options, &idx)) != -1) {
		switch (ch) {
		case 't':
			printf("Do Test?\n");
			do_test();
			break;
		case 'i':
        		do_init = true;
        		break;
		case 'l':
			ret = SAMPLE_SET_LANEID(optarg);
			if (ret != CVI_SUCCESS) {
				printf("invalid laneid parameter\n");
				return ret;
			}
			break;
		case 'p':
			ret = SAMPLE_SET_PNSWAP(optarg);
			if (ret != CVI_SUCCESS) {
				printf("invalid pnswap parameter\n");
				return ret;
			}
			break;
		case 'm':
			ret = SAMPLE_SET_PANEL_MODEL(optarg);
			if (ret != CVI_SUCCESS) {
				printf("invalid input panel model\n");
				return ret;
			}
			break;
		case 'd':
			if (argc > 2) {
				printf("usage:%s -d. -d can't use in the same time with other command\n", argv[0]);
				return CVI_FAILURE;
			}
			SAMPLE_DSI_CONTROLE();
			break;
		case 's':
			is_pattern =true;
			if (optarg != NULL){
				sscanf(optarg, "%02d", &patern_cmd);
				if (patern_cmd == VO_PAT_MAX)
					patern_cmd = -1;
			}
			break;
		case 'h':
			printHelp(argv);
			goto EXIT1;
		default:
			printf("ch = %c\n", ch);
			printHelp(argv);
			goto EXIT1;
		}
	}

	if (optind < argc) {
		printHelp(argv);
	}


	if (do_init) {
        	SAMPLE_SET_PANEL_DESC();
        	SAMPLE_PANEL_ENABLE();
	}

	if (is_pattern) {
		ret = SAMPLE_PANEL_ShowPattern(patern_cmd);
		if (ret != CVI_SUCCESS) {
			printf("Show pattern failed\n");
			return ret;
		}
	}

	if (g_panel_desc.panel_type == PANEL_MODE_BT)
		SAMPLE_PANEL_I2C_SEND();

EXIT1:
	if (ret == CVI_SUCCESS)
		SAMPLE_PRT("demo exit success!\n");
	else
		SAMPLE_PRT("demo exit abnormally!\n");


	return CVI_SUCCESS;
}
