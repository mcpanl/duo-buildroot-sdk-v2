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

static char* s_panel_model_type_arr[] = {
    "MILKV_8HD"
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
    if (s32Ret != CVI_SUCCESS)
    {
        SAMPLE_PRT("failed with %#x!\n", s32Ret);
        return CVI_FAILURE;
    }

    s32Ret = CVI_VO_Enable(VoDev);
    if (s32Ret != CVI_SUCCESS)
    {
        SAMPLE_PRT("failed with %#x!\n", s32Ret);
        return CVI_FAILURE;
    }


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
    if (s32Ret != CVI_SUCCESS)
    {
        SAMPLE_PRT("failed with %#x!\n", s32Ret);
        return CVI_FAILURE;
    }

    s32Ret = CVI_VO_EnableVideoLayer(voLayer);
    if (s32Ret != CVI_SUCCESS)
    {
        SAMPLE_PRT("failed with %#x!\n", s32Ret);
        return CVI_FAILURE;
    }


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


    // init vb
    CVI_U32 u32BlkSize = WIDTH * 3 * HEIGHT;

    CVI_SYS_Exit();
    CVI_VB_Exit();

    VB_CONFIG_S pstVbConfig;
    memset(&pstVbConfig, 0, sizeof(VB_CONFIG_S));

    // 公共视频区块池数量
    pstVbConfig.u32MaxPoolCnt = 1;

    // 第一个区块池 - 视频区块大小
    pstVbConfig.astCommPool[0].u32BlkSize = u32BlkSize;
    // 第一个区块池 - 视频区块池内的区块数
    pstVbConfig.astCommPool[0].u32BlkCnt = 8;
    SAMPLE_PRT("common pool[0] BlkSize %d\n", u32BlkSize);


    s32Ret = CVI_VB_SetConfig(&pstVbConfig);

    if (s32Ret != CVI_SUCCESS)
    {
        SAMPLE_PRT("CVI_VB_SetConf failed!\n");
        return CVI_FAILURE;
    }

    s32Ret = CVI_VB_Init();

    if (s32Ret != CVI_SUCCESS)
    {
        SAMPLE_PRT("CVI_VB_Init failed!\n");
        return CVI_FAILURE;
    }

    s32Ret = CVI_SYS_Init();

    if (s32Ret != CVI_SUCCESS)
    {
        SAMPLE_PRT("CVI_SYS_Init failed!\n");
        CVI_VB_Exit();
        return CVI_FAILURE;
    }

    // Get a Block
    // VB_BLK CVI_VB_GetBlock(VB_POOL Pool, CVI_U32 u32BlkSize);


    printf("try get block\n");
    VB_BLK blk = CVI_VB_GetBlock(VB_INVALID_POOLID, u32BlkSize);

    if (blk == VB_INVALID_HANDLE)
    {
        SAMPLE_PRT("get block fail\n");
        return CVI_FAILURE;
    }

    printf("get block success %lld\n", blk);

    // get address by block

    CVI_U64 physAddr = CVI_VB_Handle2PhysAddr(blk);
    CVI_VOID* virtAddr = CVI_SYS_Mmap(physAddr, u32BlkSize);
    if (virtAddr == NULL)
    {
        SAMPLE_PRT("CVI_SYS_Mmap failed\n");
        CVI_VB_ReleaseBlock(blk);
        return CVI_FAILURE;
    }

    printf("get address success %lld\n", physAddr);


    memset(virtAddr, 0, u32BlkSize);

    for (int i = 0; i < WIDTH * HEIGHT; i++)
    {
        ((CVI_U8*)virtAddr)[i * 3 + 0] = 255; // R
        ((CVI_U8*)virtAddr)[i * 3 + 1] = 0; // G
        ((CVI_U8*)virtAddr)[i * 3 + 2] = 255; // B
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

    if (VB_INVALID_POOLID == stFrame.u32PoolId)
    {
        SAMPLE_PRT("Frame poolId fail\n");
        CVI_VB_ReleaseBlock(blk);
        return CVI_FAILURE;
    }

    printf("Frame poolId success %d\n", stFrame.u32PoolId);

    // CVI_U32 colors[][3] = {
    //     {255, 0, 0},     // Red
    //     {0, 255, 0},     // Green
    //     {0, 0, 255},     // Blue
    //     {255, 255, 0},   // Yellow
    //     {0, 255, 255},   // Cyan
    //     {255, 0, 255},   // Magenta
    //     {255, 255, 255}, // White
    //     {0, 0, 0}        // Black
    // };

    // int num_colors = sizeof(colors) / sizeof(colors[0]);

    // while (1) {
    //     for (int c = 0; c < num_colors; ++c) {
    //         CVI_U8 R = colors[c][0];
    //         CVI_U8 G = colors[c][1];
    //         CVI_U8 B = colors[c][2];
    //
    //         // 填充颜色
    //         for (int i = 0; i < WIDTH * HEIGHT; i++) {
    //             ((CVI_U8 *)virtAddr)[i * 3 + 0] = R;
    //             ((CVI_U8 *)virtAddr)[i * 3 + 1] = G;
    //             ((CVI_U8 *)virtAddr)[i * 3 + 2] = B;
    //         }
    //
    //         // 设置时间戳递增（可选）
    //         stFrame.stVFrame.u32TimeRef++;
    //
    //         // 发送帧
    //         CVI_S32 ret = CVI_VO_SendFrame(voLayer, voChn, &stFrame, 0);
    //         if (ret != CVI_SUCCESS) {
    //             SAMPLE_PRT("Send frame failed: %d\n", ret);
    //         } else {
    //             printf("发送颜色帧 R:%d G:%d B:%d\n", R, G, B);
    //         }
    //
    //         delay_ms(200); // 每帧延迟 1 秒
    //     }
    // }


    CVI_VO_SendFrame(voLayer, voChn, &stFrame, 0);

    printf("已发送帧数据\n");
    sleep(2);

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

void vo_test_function() {
    printf("Hello from vo module!\n");
}

void my_vo_init() {
    printf("My vo init!\n");

    do_test();
}

int _main()
{
    CVI_U32 ret = do_test();

    if (ret == CVI_SUCCESS)
    {
        SAMPLE_PRT("demo exit success!\n");
    }
    else
    {
        SAMPLE_PRT("demo exit abnormally!\n");
    }

    return CVI_SUCCESS;
}
