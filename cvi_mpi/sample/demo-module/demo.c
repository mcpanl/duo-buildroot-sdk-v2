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


#define ALIGN_UP(x, align) (((x) + (align) - 1) & ~((align) - 1))

#define WIDTH  368
#define HEIGHT 552

#define STRIDE ALIGN_UP(WIDTH * 3, 64)

#define COLOR_R 255
#define COLOR_G 0
#define COLOR_B 0



//int stride = ALIGN_UP(WIDTH, 64);


static VO_DEV VoDev = 0;
static VO_LAYER voLayer = 0;
static VO_CHN voChn = 0;
static VB_BLK blk = VB_INVALID_HANDLE;
static CVI_VOID* virtAddr = NULL;
static CVI_U64 physAddr;
static CVI_U32 u32BlkSize;
static CVI_U32 camera_u32BlkSize;

static int fd;

void m_vo_deinit();

void delay_ms(int milliseconds)
{
	usleep(milliseconds * 1000); // 微秒级延迟（已废弃但广泛支持）
				     // 或使用更现代的 nanosleep
	struct timespec ts = {
		.tv_sec = milliseconds / 1000,
		.tv_nsec = (milliseconds % 1000) * 1000000
	};
	nanosleep(&ts, NULL);
}

typedef enum
{
	DSI_PANEL_MILKV_8HD,
	DSI_PANEL_ST7701_HD228001C31,
	PANEL_MAX
} PANEL_MODEL;

typedef struct _inputPara_
{
	enum mipi_tx_lane_id lane_id[LANE_MAX_NUM];
	bool lane_pn_swap[LANE_MAX_NUM];
	bool lane_id_flag;
	bool pn_swap_flag;
	CVI_U8 dev_no;
	PANEL_MODEL panel_model;
} inputPara;

inputPara g_input_para = {
	//.panel_model = DSI_PANEL_MILKV_8HD,
	.panel_model = DSI_PANEL_ST7701_HD228001C31,
	.dev_no = 0,
};

static struct panel_desc_s g_panel_desc = {
	.panel_mode = "LCD2_28",
	.panel_type = PANEL_MODE_DSI,
	.stdsicfg.dev_cfg = &dev_cfg_st7701_368x552,
	.stdsicfg.hs_timing_cfg = &hs_timing_cfg_st7701_368x552,
	.stdsicfg.dsi_init_cmds = dsi_init_cmds_st7701_368x552,
	.stdsicfg.dsi_init_cmds_size = ARRAY_SIZE(dsi_init_cmds_st7701_368x552)
};

static char* s_panel_model_type_arr[] = {
	"MILKV_8HD",
	"LCD2_28"
};

int dsi_init(int devno, const struct dsc_instr* cmds, int size)
{
	int ret;

	if (cmds == NULL)
	{
		return CVI_FAILURE;
	}

	for (int i = 0; i < size; i++)
	{
		const struct dsc_instr* instr = &cmds[i];
		struct cmd_info_s cmd_info = {
			.devno = devno,
			.cmd_size = instr->size,
			.data_type = instr->data_type,
			.cmd = (void*)instr->data
		};

		ret = CVI_MIPI_TX_SendCmd(fd, &cmd_info);
		if (instr->delay)
			usleep(instr->delay * 1000);

		if (ret)
		{
			printf("dsi init failed at %d instr.\n", i);
			return ret;
		}
	}
	return ret;
}

void SAMPLE_SET_PANEL_DESC(void)
{
	switch (g_input_para.panel_model)
	{
		case DSI_PANEL_MILKV_8HD:
			g_panel_desc.panel_type = PANEL_MODE_DSI;
			g_panel_desc.stdsicfg.dev_cfg = &dev_cfg_milkv_8hd_800x1280;
			g_panel_desc.stdsicfg.hs_timing_cfg = &hs_timing_cfg_milkv_8hd_800x1280;
			g_panel_desc.stdsicfg.dsi_init_cmds = dsi_init_cmds_milkv_8hd_800x1280;
			g_panel_desc.stdsicfg.dsi_init_cmds_size = ARRAY_SIZE(dsi_init_cmds_milkv_8hd_800x1280);
			break;
		case DSI_PANEL_ST7701_HD228001C31:
                        g_panel_desc.panel_type = PANEL_MODE_DSI;
                        g_panel_desc.stdsicfg.dev_cfg = &dev_cfg_st7701_368x552;
                        g_panel_desc.stdsicfg.hs_timing_cfg = &hs_timing_cfg_st7701_368x552;
                        g_panel_desc.stdsicfg.dsi_init_cmds = dsi_init_cmds_st7701_368x552;
			g_panel_desc.stdsicfg.dsi_init_cmds_size = ARRAY_SIZE(dsi_init_cmds_st7701_368x552);
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
}

CVI_S32 SAMPLE_SET_PANEL_MODEL(char* pinput_str)
{
	CVI_S32 i = 0;
	bool is_find = false;
	PANEL_MODEL panel_model = DSI_PANEL_MILKV_8HD;

	for (i = 0; i < PANEL_MAX; i++)
	{
		if (strcmp(pinput_str, s_panel_model_type_arr[i]) == 0)
		{
			is_find = true;
			break;
		}
	}

	if (is_find)
	{
		panel_model = (PANEL_MODEL)i;
	}
	else
	{
		return CVI_FAILURE;
	}

	g_input_para.panel_model = panel_model;
	g_panel_desc.panel_mode = s_panel_model_type_arr[i];
	return CVI_SUCCESS;
}

CVI_S32 init_custom_vb_pool_once(VB_CONFIG_S* pstVbConfig)
{
	CVI_S32 s32Ret = CVI_FAILURE;

	CVI_SYS_Exit();
	CVI_VB_Exit();

	if (pstVbConfig == NULL)
	{
		CVI_TRACE_LOG(CVI_DBG_ERR, "input parameter is null, it is invaild!\n");
		return CVI_FAILURE;
	}

	s32Ret = CVI_VB_SetConfig(pstVbConfig);
	if (s32Ret != CVI_SUCCESS)
	{
		CVI_TRACE_LOG(CVI_DBG_ERR, "CVI_VB_SetConf failed!\n");
		return s32Ret;
	}

	s32Ret = CVI_VB_Init();
	if (s32Ret != CVI_SUCCESS)
	{
		CVI_TRACE_LOG(CVI_DBG_ERR, "CVI_VB_Init failed!\n");
		return s32Ret;
	}

	s32Ret = CVI_SYS_Init();
	if (s32Ret != CVI_SUCCESS)
	{
		CVI_TRACE_LOG(CVI_DBG_ERR, "CVI_SYS_Init failed!\n");
		CVI_VB_Exit();
		return s32Ret;
	}

	return CVI_SUCCESS;
}

CVI_S32 _vo_init() {
    CVI_S32 s32Ret = CVI_SUCCESS;
    printf("==> VO init start\n");

    printf("Calling CVI_SYS_Exit()\n");
    CVI_SYS_Exit();

    printf("Calling CVI_VB_Exit()\n");
    CVI_VB_Exit();
/*
    VO_PUB_ATTR_S pstPubAttr = {
        .enIntfType = VO_INTF_MIPI,
        .enIntfSync = VO_OUTPUT_720P60,
        .u32BgColor = 0x000000
    };
*/

VO_PUB_ATTR_S pstPubAttr = {
    .enIntfType = VO_INTF_MIPI,      // 使用 MIPI DSI 接口
    .enIntfSync = VO_OUTPUT_USER,    // 使用自定义时序
    .u32BgColor = 0x000000,          // 背景色，黑色
    .stSyncInfo = {
        .bSynm = CVI_TRUE,           // 信号同步方式，通常 TRUE（同步信号）
        .bIop = CVI_TRUE,            // Progressive (逐行) 显示
        .u16FrameRate = 60,          // 刷新率，建议设为 60Hz

        // 垂直方向参数
        .u16Vact = 552,              // 垂直有效区域
        .u16Vbb = 20,                // 垂直后肩 (VBP)
        .u16Vfb = 20,                // 垂直前肩 (VFP)

        // 水平方向参数
        .u16Hact = 368,              // 水平有效区域
        .u16Hbb = 160,               // 水平后肩 (HBP)
        .u16Hfb = 160,               // 水平前肩 (HFP)

        // 同步脉冲宽度
        .u16Hpw = 8,                 // 水平同步脉冲宽度 (HSA)
        .u16Vpw = 10,                // 垂直同步脉冲宽度 (VSA)

        // 同步极性
        .bIdv = CVI_TRUE,            // 垂直同步极性，通常从屏幕手册确认（假设屏幕需要正极性）
        .bIhs = CVI_FALSE,           // 水平同步极性，通常从屏幕手册确认（假设屏幕需要负极性）
        .bIvs = CVI_TRUE             // 垂直同步极性，通常从屏幕手册确认（假设屏幕需要正极性）
    }
};

    printf("Setting VO public attributes...\n");
    s32Ret = CVI_VO_SetPubAttr(VoDev, &pstPubAttr);
    if (s32Ret != CVI_SUCCESS) {
        printf("Error: CVI_VO_SetPubAttr failed with code %d\n", s32Ret);
        return s32Ret;
    }

    printf("Enabling VO device...\n");
    s32Ret = CVI_VO_Enable(VoDev);
    if (s32Ret != CVI_SUCCESS) {
        printf("Error: CVI_VO_Enable failed with code %d\n", s32Ret);
        return s32Ret;
    }

    VO_VIDEO_LAYER_ATTR_S pstLayerAttr = {
        .stDispRect = {0, 0, WIDTH, HEIGHT},
        .stImageSize = {WIDTH, HEIGHT},
        .enPixFormat = PIXEL_FORMAT_RGB_888,
        .u32DispFrmRt = 60
    };

    printf("Setting video layer attributes...\n");
    s32Ret = CVI_VO_SetVideoLayerAttr(voLayer, &pstLayerAttr);
    if (s32Ret != CVI_SUCCESS) {
        printf("Error: CVI_VO_SetVideoLayerAttr failed with code %d\n", s32Ret);
        return s32Ret;
    }

    printf("Enabling video layer...\n");
    s32Ret = CVI_VO_EnableVideoLayer(voLayer);
    if (s32Ret != CVI_SUCCESS) {
        printf("Error: CVI_VO_EnableVideoLayer failed with code %d\n", s32Ret);
        return s32Ret;
    }

    VO_CHN_ATTR_S stChnAttr = {
        .stRect = {0, 0, WIDTH, HEIGHT},
        .u32Priority = 0
    };

    printf("Setting VO channel attributes...\n");
    s32Ret = CVI_VO_SetChnAttr(voLayer, voChn, &stChnAttr);
    if (s32Ret != CVI_SUCCESS) {
        printf("Error: CVI_VO_SetChnAttr failed with code %d\n", s32Ret);
        return s32Ret;
    }

    printf("Enabling VO channel...\n");
    CVI_VO_EnableChn(voLayer, voChn);

    printf("Showing VO channel...\n");
    CVI_VO_ShowChn(voLayer, voChn);

    u32BlkSize = STRIDE * HEIGHT;
    printf("Calculated block size: %u\n", u32BlkSize);

    camera_u32BlkSize = 64;

    VB_CONFIG_S vbConfig = {
        .u32MaxPoolCnt = 3,
        .astCommPool = {
            [0] = {
                .u32BlkSize = u32BlkSize,
                .u32BlkCnt = 4
            },
	    [1] = {
                .u32BlkSize = u32BlkSize,
                .u32BlkCnt = 4
            },
	    [2] = {
	    	.u32BlkSize = camera_u32BlkSize,
		.u32BlkCnt = 4
	    }
        }
    };

    printf("Setting VB config...\n");
    s32Ret = CVI_VB_SetConfig(&vbConfig);
    if (s32Ret != CVI_SUCCESS) {
        printf("Error: CVI_VB_SetConfig failed with code %d\n", s32Ret);
        return s32Ret;
    }

    printf("Initializing VB...\n");
    s32Ret = CVI_VB_Init();
    if (s32Ret != CVI_SUCCESS) {
        printf("Error: CVI_VB_Init failed with code %d\n", s32Ret);
        return s32Ret;
    }

    printf("Initializing SYS...\n");
    s32Ret = CVI_SYS_Init();
    if (s32Ret != CVI_SUCCESS) {
        printf("Error: CVI_SYS_Init failed with code %d\n", s32Ret);
    }

    printf("<== VO init end with result: %d\n", s32Ret);
    return s32Ret;
}

CVI_S32 _vo_init_no_print() {
	CVI_S32 s32Ret = CVI_SUCCESS;

        CVI_SYS_Exit();
        CVI_VB_Exit();

	VO_PUB_ATTR_S pstPubAttr = {
		.enIntfType = VO_INTF_MIPI,
		.enIntfSync = VO_OUTPUT_720P60,
		.u32BgColor = 0x000000
	};

	s32Ret = CVI_VO_SetPubAttr(VoDev, &pstPubAttr);
	if (s32Ret != CVI_SUCCESS) return s32Ret;

	s32Ret = CVI_VO_Enable(VoDev);
	if (s32Ret != CVI_SUCCESS) return s32Ret;

	VO_VIDEO_LAYER_ATTR_S pstLayerAttr = {
		.stDispRect = {0, 0, WIDTH, HEIGHT},
		.stImageSize = {WIDTH, HEIGHT},
		.enPixFormat = PIXEL_FORMAT_ARGB_8888,
		.u32DispFrmRt = 60
	};

	s32Ret = CVI_VO_SetVideoLayerAttr(voLayer, &pstLayerAttr);
	if (s32Ret != CVI_SUCCESS) return s32Ret;

	s32Ret = CVI_VO_EnableVideoLayer(voLayer);
	if (s32Ret != CVI_SUCCESS) return s32Ret;

	VO_CHN_ATTR_S stChnAttr = {
		.stRect = {0, 0, WIDTH, HEIGHT},
		.u32Priority = 0
	};

	s32Ret = CVI_VO_SetChnAttr(voLayer, voChn, &stChnAttr);
	if (s32Ret != CVI_SUCCESS) return s32Ret;

	CVI_VO_EnableChn(voLayer, voChn);
	CVI_VO_ShowChn(voLayer, voChn);

	u32BlkSize = WIDTH * HEIGHT * 4;

	//CVI_SYS_Exit();
	//CVI_VB_Exit();

	VB_CONFIG_S vbConfig = {
		.u32MaxPoolCnt = 1,
		.astCommPool = {
			[0] = {
				.u32BlkSize = u32BlkSize,
				.u32BlkCnt = 4
			},
		}
	};

	s32Ret = CVI_VB_SetConfig(&vbConfig);
	if (s32Ret != CVI_SUCCESS) return s32Ret;

	s32Ret = CVI_VB_Init();
	if (s32Ret != CVI_SUCCESS) return s32Ret;

	s32Ret = CVI_SYS_Init();
	return s32Ret;
}


CVI_S32 _vo_send_frame_from_nv12(const uint8_t *y, const uint8_t *uv) {
    CVI_S32 s32Ret = CVI_SUCCESS;

    printf("[NV12] Send Frame Start\n");

    if (blk == VB_INVALID_HANDLE) {
        u32BlkSize = STRIDE * HEIGHT * 3 / 2;
        printf("[NV12] Allocating VB block of size %u\n", u32BlkSize);

        blk = CVI_VB_GetBlock(VB_INVALID_POOLID, u32BlkSize);
        if (blk == VB_INVALID_HANDLE) {
            printf("[NV12] Failed to get VB block!\n");
            return CVI_FAILURE;
        }

        physAddr = CVI_VB_Handle2PhysAddr(blk);
        printf("[NV12] Got physAddr = 0x%llx\n", physAddr);

        virtAddr = CVI_SYS_Mmap(physAddr, u32BlkSize);
        if (!virtAddr) {
            printf("[NV12] Failed to mmap physAddr\n");
            CVI_VB_ReleaseBlock(blk);
            blk = VB_INVALID_HANDLE;
            return CVI_FAILURE;
        }

        printf("[NV12] Mapped virtAddr = %p\n", virtAddr);
    } else {
        printf("[NV12] Reusing previously allocated VB block\n");
    }

    // 拷贝 Y 平面
    memcpy(virtAddr, y, STRIDE * HEIGHT);
    // 拷贝 UV 平面
    memcpy((uint8_t *)virtAddr + STRIDE * HEIGHT, uv, STRIDE * HEIGHT / 2);
    printf("[NV12] Copied Y (%d bytes) and UV (%d bytes)\n", WIDTH * HEIGHT, WIDTH * HEIGHT / 2);

    VIDEO_FRAME_INFO_S stFrame = {0};
    stFrame.stVFrame.u32Width = WIDTH;
    stFrame.stVFrame.u32Height = HEIGHT;
    stFrame.stVFrame.enPixelFormat = PIXEL_FORMAT_NV12;

    stFrame.stVFrame.u64PhyAddr[0] = physAddr;
    stFrame.stVFrame.pu8VirAddr[0] = (uint8_t *)virtAddr;
    stFrame.stVFrame.u64PhyAddr[1] = physAddr + STRIDE * HEIGHT;
    stFrame.stVFrame.pu8VirAddr[1] = (uint8_t *)virtAddr + STRIDE * HEIGHT;

    stFrame.stVFrame.u32Stride[0] = STRIDE;
    stFrame.stVFrame.u32Stride[1] = STRIDE;

    stFrame.stVFrame.u32Length[0] = STRIDE * HEIGHT;
    stFrame.stVFrame.u32Length[1] = STRIDE * HEIGHT / 2;

    stFrame.stVFrame.u32TimeRef = 1;

    stFrame.u32PoolId = CVI_VB_Handle2PoolId(blk);
    if (stFrame.u32PoolId == VB_INVALID_POOLID) {
        printf("[NV12] Invalid pool ID!\n");
        return CVI_FAILURE;
    }

    // 打印调试结构信息
    printf("[NV12] Frame info:\n");
    printf("    Width = %u, Height = %u\n", stFrame.stVFrame.u32Width, stFrame.stVFrame.u32Height);
    printf("    Y Addr: phy = 0x%llx, vir = %p, len = %u\n",
        stFrame.stVFrame.u64PhyAddr[0],
        stFrame.stVFrame.pu8VirAddr[0],
        stFrame.stVFrame.u32Length[0]);

    printf("    UV Addr: phy = 0x%llx, vir = %p, len = %u\n",
        stFrame.stVFrame.u64PhyAddr[1],
        stFrame.stVFrame.pu8VirAddr[1],
        stFrame.stVFrame.u32Length[1]);

    printf("    Stride Y = %u, Stride UV = %u\n",
        stFrame.stVFrame.u32Stride[0],
        stFrame.stVFrame.u32Stride[1]);

    printf("    Pool ID = %u\n", stFrame.u32PoolId);

    // 发送帧
    s32Ret = CVI_VO_SendFrame(voLayer, voChn, &stFrame, 0);
    printf("[NV12] CVI_VO_SendFrame returned %d\n", s32Ret);

    return s32Ret;
}

CVI_S32 _vo_send_frame_from_nv12_no_print(const uint8_t *y, const uint8_t *uv) {
    CVI_S32 s32Ret = CVI_SUCCESS;

    if (blk == VB_INVALID_HANDLE) {
        u32BlkSize = WIDTH * HEIGHT * 3 / 2; // NV12
        blk = CVI_VB_GetBlock(VB_INVALID_POOLID, u32BlkSize);
        if (blk == VB_INVALID_HANDLE) return CVI_FAILURE;

        physAddr = CVI_VB_Handle2PhysAddr(blk);
        virtAddr = CVI_SYS_Mmap(physAddr, u32BlkSize);
        if (!virtAddr) {
            CVI_VB_ReleaseBlock(blk);
            blk = VB_INVALID_HANDLE;
            return CVI_FAILURE;
        }
    }

    // 写 Y + UV 平面
    memcpy(virtAddr, y, WIDTH * HEIGHT);
    memcpy((uint8_t *)virtAddr + WIDTH * HEIGHT, uv, WIDTH * HEIGHT / 2);

    VIDEO_FRAME_INFO_S stFrame = {0};
    stFrame.stVFrame.u32Width = WIDTH;
    stFrame.stVFrame.u32Height = HEIGHT;
    stFrame.stVFrame.enPixelFormat = PIXEL_FORMAT_NV12;
    stFrame.stVFrame.u64PhyAddr[0] = physAddr;
    stFrame.stVFrame.pu8VirAddr[0] = (uint8_t *)virtAddr;
    stFrame.stVFrame.u64PhyAddr[1] = physAddr + WIDTH * HEIGHT;
    stFrame.stVFrame.pu8VirAddr[1] = (uint8_t *)virtAddr + WIDTH * HEIGHT;
    stFrame.stVFrame.u32Stride[0] = WIDTH;
    stFrame.stVFrame.u32Stride[1] = WIDTH;
    stFrame.u32PoolId = CVI_VB_Handle2PoolId(blk);

    s32Ret = CVI_VO_SendFrame(voLayer, voChn, &stFrame, 0);
    return s32Ret;
}


CVI_S32 _vo_send_frame_from_argb8888(const uint32_t *pARGB8888Data) {
	CVI_S32 s32Ret = CVI_SUCCESS;
	printf("FB (ARGB8888)\n");

        if (blk == VB_INVALID_HANDLE) {
                u32BlkSize = WIDTH * HEIGHT * 4;  // ARGB888 每像素4字节
                blk = CVI_VB_GetBlock(VB_INVALID_POOLID, u32BlkSize);
                if (blk == VB_INVALID_HANDLE) return CVI_FAILURE;

                physAddr = CVI_VB_Handle2PhysAddr(blk);
                virtAddr = CVI_SYS_Mmap(physAddr, u32BlkSize);
                if (!virtAddr) {
                        CVI_VB_ReleaseBlock(blk);
                        blk = VB_INVALID_HANDLE;
                        return CVI_FAILURE;
                }
        }

        // 直接复制 ARGB888 数据
        memcpy(virtAddr, pARGB8888Data, WIDTH * HEIGHT * 4);

        VIDEO_FRAME_INFO_S stFrame = {0};
        stFrame.stVFrame.u32Width = WIDTH;
        stFrame.stVFrame.u32Height = HEIGHT;
        stFrame.stVFrame.enPixelFormat = PIXEL_FORMAT_ARGB_8888;
        stFrame.stVFrame.u64PhyAddr[0] = physAddr;
        stFrame.stVFrame.pu8VirAddr[0] = (uint8_t *)virtAddr;
        stFrame.stVFrame.u32Length[0] = u32BlkSize;
        stFrame.stVFrame.u32Stride[0] = WIDTH * 4;
        stFrame.stVFrame.u32TimeRef = 1;

        stFrame.u32PoolId = CVI_VB_Handle2PoolId(blk);

        if (stFrame.u32PoolId == VB_INVALID_POOLID) {
                return CVI_FAILURE;
        }


        s32Ret = CVI_VO_SendFrame(voLayer, voChn, &stFrame, 0);
        return s32Ret;
}

CVI_S32 _vo_send_frame_from_rgb888(const uint8_t *pRGB888Data) {
	CVI_S32 s32Ret = CVI_SUCCESS;

	// printf("FB (RGB888)\n");

	if (blk == VB_INVALID_HANDLE) {
		u32BlkSize = STRIDE * HEIGHT;  // RGB888 每像素3字节
		blk = CVI_VB_GetBlock(VB_INVALID_POOLID, u32BlkSize);
		if (blk == VB_INVALID_HANDLE) return CVI_FAILURE;
		
		printf("**** Init blk ****\n");

		physAddr = CVI_VB_Handle2PhysAddr(blk);
		virtAddr = CVI_SYS_Mmap(physAddr, u32BlkSize);
		if (!virtAddr) {
			CVI_VB_ReleaseBlock(blk);
			blk = VB_INVALID_HANDLE;
			return CVI_FAILURE;
		}
	}

	// 直接复制 RGB888 数据
	memcpy(virtAddr, pRGB888Data, STRIDE * HEIGHT);

	VIDEO_FRAME_INFO_S stFrame = {0};
	stFrame.stVFrame.u32Width = WIDTH;
	stFrame.stVFrame.u32Height = HEIGHT;
	stFrame.stVFrame.enPixelFormat = PIXEL_FORMAT_RGB_888;
	stFrame.stVFrame.u64PhyAddr[0] = physAddr;
	stFrame.stVFrame.pu8VirAddr[0] = (uint8_t *)virtAddr;
	stFrame.stVFrame.u32Length[0] = u32BlkSize;
	stFrame.stVFrame.u32Stride[0] = STRIDE;
	stFrame.stVFrame.u32TimeRef = 1;

	stFrame.u32PoolId = CVI_VB_Handle2PoolId(blk);

	if (stFrame.u32PoolId == VB_INVALID_POOLID) {
		return CVI_FAILURE;
	}

	s32Ret = CVI_VO_SendFrame(voLayer, voChn, &stFrame, 0);
	return s32Ret;
}

CVI_S32 _vo_send_frame_from_rgb565(const uint16_t *pRGB565Data) {
	CVI_S32 s32Ret = CVI_SUCCESS;

	printf("FB x\n");

	if (blk == VB_INVALID_HANDLE) {
		u32BlkSize = WIDTH * HEIGHT * 3;  // RGB888 每像素3字节
		blk = CVI_VB_GetBlock(VB_INVALID_POOLID, u32BlkSize);
		if (blk == VB_INVALID_HANDLE) return CVI_FAILURE;

		physAddr = CVI_VB_Handle2PhysAddr(blk);
		virtAddr = CVI_SYS_Mmap(physAddr, u32BlkSize);
		if (!virtAddr) {
			CVI_VB_ReleaseBlock(blk);
			blk = VB_INVALID_HANDLE;
			return CVI_FAILURE;
		}
	}

	// RGB565 -> RGB888 数据填充
	uint8_t *pRGB888 = (uint8_t *)virtAddr;
	for (int i = 0; i < WIDTH * HEIGHT; i++) {
		uint16_t pixel565 = pRGB565Data[i];
		uint8_t r = ((pixel565 >> 11) & 0x1F) << 3;
		uint8_t g = ((pixel565 >> 5) & 0x3F) << 2;
		uint8_t b = (pixel565 & 0x1F) << 3;

		// 补高位精度
		pRGB888[i * 3 + 0] = r | (r >> 5);
		pRGB888[i * 3 + 1] = g | (g >> 6);
		pRGB888[i * 3 + 2] = b | (b >> 5);
	}

	VIDEO_FRAME_INFO_S stFrame = {0};
	stFrame.stVFrame.u32Width = WIDTH;
	stFrame.stVFrame.u32Height = HEIGHT;
	stFrame.stVFrame.enPixelFormat = PIXEL_FORMAT_RGB_888;
	stFrame.stVFrame.u64PhyAddr[0] = physAddr;
	stFrame.stVFrame.pu8VirAddr[0] = (uint8_t *)virtAddr;
	stFrame.stVFrame.u32Length[0] = u32BlkSize;
	stFrame.stVFrame.u32Stride[0] = WIDTH * 3;
	stFrame.stVFrame.u32TimeRef = 1;
	stFrame.u32PoolId = CVI_VB_Handle2PoolId(blk);

	if (stFrame.u32PoolId == VB_INVALID_POOLID) {
		return CVI_FAILURE;
	}

	s32Ret = CVI_VO_SendFrame(voLayer, voChn, &stFrame, 0);
	return s32Ret;
}


void _vo_release_static_blk() {
	if (virtAddr) {
		CVI_SYS_Munmap(virtAddr, u32BlkSize);
		virtAddr = NULL;
	}

	if (blk != VB_INVALID_HANDLE) {
		CVI_VB_ReleaseBlock(blk);
		blk = VB_INVALID_HANDLE;
	}
}

CVI_S32 __vo_send_frame_from_rgb565(const uint16_t *pRGB565Data) {
	CVI_S32 s32Ret = CVI_SUCCESS;
	uint32_t u32BlkSize = WIDTH * HEIGHT * 3;  // RGB888 每像素3字节

	VB_BLK blk = CVI_VB_GetBlock(VB_INVALID_POOLID, u32BlkSize);
	if (blk == VB_INVALID_HANDLE) return CVI_FAILURE;

	uint64_t physAddr = CVI_VB_Handle2PhysAddr(blk);
	uint8_t *virtAddr = (uint8_t *)CVI_SYS_Mmap(physAddr, u32BlkSize);
	if (!virtAddr) {
		CVI_VB_ReleaseBlock(blk);
		return CVI_FAILURE;
	}

	// RGB565 -> RGB888 转换
	for (int i = 0; i < WIDTH * HEIGHT; i++) {
		uint16_t pixel565 = pRGB565Data[i];
		uint8_t r5 = (pixel565 >> 11) & 0x1F;
		uint8_t g6 = (pixel565 >> 5) & 0x3F;
		uint8_t b5 = pixel565 & 0x1F;

		uint8_t r8 = (r5 << 3) | (r5 >> 2);
		uint8_t g8 = (g6 << 2) | (g6 >> 4);
		uint8_t b8 = (b5 << 3) | (b5 >> 2);

		virtAddr[i * 3 + 0] = r8;
		virtAddr[i * 3 + 1] = g8;
		virtAddr[i * 3 + 2] = b8;
	}

	VIDEO_FRAME_INFO_S stFrame = {0};
	stFrame.stVFrame.u32Width = WIDTH;
	stFrame.stVFrame.u32Height = HEIGHT;
	stFrame.stVFrame.enPixelFormat = PIXEL_FORMAT_RGB_888;
	stFrame.stVFrame.u64PhyAddr[0] = physAddr;
	stFrame.stVFrame.pu8VirAddr[0] = virtAddr;
	stFrame.stVFrame.u32Length[0] = u32BlkSize;
	stFrame.stVFrame.u32Stride[0] = WIDTH * 3;
	stFrame.stVFrame.u32TimeRef = 1;
	stFrame.u32PoolId = CVI_VB_Handle2PoolId(blk);

	if (stFrame.u32PoolId == VB_INVALID_POOLID) {
		CVI_SYS_Munmap(virtAddr, u32BlkSize);
		CVI_VB_ReleaseBlock(blk);
		return CVI_FAILURE;
	}

	s32Ret = CVI_VO_SendFrame(voLayer, voChn, &stFrame, 0);

	// 清理内存
	CVI_SYS_Munmap(virtAddr, u32BlkSize);
	CVI_VB_ReleaseBlock(blk);

	return s32Ret;
}

CVI_S32 _vo_send_frame() {
	CVI_S32 s32Ret = CVI_SUCCESS;

	blk = CVI_VB_GetBlock(VB_INVALID_POOLID, u32BlkSize);
	if (blk == VB_INVALID_HANDLE) return CVI_FAILURE;

	physAddr = CVI_VB_Handle2PhysAddr(blk);
	virtAddr = CVI_SYS_Mmap(physAddr, u32BlkSize);
	if (!virtAddr) return CVI_FAILURE;

	// 填充红蓝像素 (紫色)
	for (int i = 0; i < WIDTH * HEIGHT; i++) {
		((CVI_U8*)virtAddr)[i * 3 + 0] = 255; // R
		((CVI_U8*)virtAddr)[i * 3 + 1] = 0;   // G
		((CVI_U8*)virtAddr)[i * 3 + 2] = 255; // B
	}

	VIDEO_FRAME_INFO_S stFrame = {0};
	stFrame.stVFrame.u32Width = WIDTH;
	stFrame.stVFrame.u32Height = HEIGHT;
	stFrame.stVFrame.enPixelFormat = PIXEL_FORMAT_RGB_888;
	stFrame.stVFrame.u64PhyAddr[0] = physAddr;
	stFrame.stVFrame.pu8VirAddr[0] = virtAddr;
	stFrame.stVFrame.u32Length[0] = u32BlkSize;
	stFrame.stVFrame.u32Stride[0] = WIDTH * 3;
	stFrame.stVFrame.u32TimeRef = 1;
	stFrame.u32PoolId = CVI_VB_Handle2PoolId(blk);

	if (stFrame.u32PoolId == VB_INVALID_POOLID) return CVI_FAILURE;

	s32Ret = CVI_VO_SendFrame(voLayer, voChn, &stFrame, 0);
	return s32Ret;
}

void _vo_deinit() {
	if (virtAddr) {
		printf("**** Release virtAddr ****\n");
		CVI_SYS_Munmap(virtAddr, u32BlkSize);
		virtAddr = NULL;
	}

	if (blk != VB_INVALID_HANDLE) {
		printf("**** Release blk ****\n");
		CVI_VB_ReleaseBlock(blk);
		blk = VB_INVALID_HANDLE;
	}

	CVI_VO_DisableChn(voLayer, voChn);
	CVI_VO_DisableVideoLayer(voLayer);
	CVI_VO_Disable(VoDev);
	CVI_SYS_Exit();
	CVI_VB_Exit();
}

void vo_test_function() {
	printf("Hello from vo module!\n");
}


int m_vo_init() {
	return _vo_init() == CVI_SUCCESS?0:-1;
}

int m_vo_send_frame_from_nv12(const uint8_t *y, const uint8_t *uv) {
	CVI_S32 s32Ret = _vo_send_frame_from_nv12(y, uv);
        return (s32Ret == CVI_SUCCESS) ? 0 : s32Ret;
}

int m_vo_send_frame_from_argb8888(const uint32_t *pARGB8888Data) {
        if (!pARGB8888Data) {
                return -1;  // 参数错误
        }

        CVI_S32 s32Ret = _vo_send_frame_from_argb8888(pARGB8888Data);
        return (s32Ret == CVI_SUCCESS) ? 0 : s32Ret;
}

int m_vo_send_frame_from_rgb888(const uint8_t *pRGB888Data) {
        if (!pRGB888Data) {
                return -1;  // 参数错误
        }

        CVI_S32 s32Ret = _vo_send_frame_from_rgb888(pRGB888Data);
        return (s32Ret == CVI_SUCCESS) ? 0 : s32Ret;
}

int m_vo_send_frame_from_rgb565(const uint16_t *pRGB565Data) {
	if (!pRGB565Data) {
		return -1;  // 参数错误
	}

	CVI_S32 s32Ret = _vo_send_frame_from_rgb565(pRGB565Data);
	return (s32Ret == CVI_SUCCESS) ? 0 : s32Ret;
}

void m_vo_deinit() {
	_vo_deinit();
}


#if 0
void my_vo_init() {
	printf("My vo init!!!\n");

	if (_vo_init() != CVI_SUCCESS) {
		printf("VO Init Failed\n");
		return;
	}

	if (_vo_send_frame() != CVI_SUCCESS) {
		printf("Send Frame Failed\n");
		_vo_deinit();
		return;
	}

	printf("Frame sent. Waiting 2 seconds...\n");
	sleep(2);

	_vo_deinit();
}
#endif

int _main()
{
	return 0;
	//CVI_U32 ret = do_test();

	//if (ret == CVI_SUCCESS)
	//{
	//    SAMPLE_PRT("demo exit success!\n");
	//}
	//else
	//{
	//    SAMPLE_PRT("demo exit abnormally!\n");
	//}

	//return CVI_SUCCESS;
}
