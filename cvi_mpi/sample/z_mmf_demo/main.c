#include <stdio.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

#include "../z_mmf/z_mmf.h"   // 指向 project 目录中的头文件

#define PRINT_BYTES 24
#define CLIP(x) ((x) < 0 ? 0 : ((x) > 255 ? 255 : (x)))

static void yuv_to_rgb(uint8_t Y, uint8_t U, uint8_t V, uint8_t *R, uint8_t *G, uint8_t *B)
{
    int C = Y - 16;
    int D = U - 128;
    int E = V - 128;

    int r = (298 * C + 409 * E + 128) >> 8;
    int g = (298 * C - 100 * D - 208 * E + 128) >> 8;
    int b = (298 * C + 516 * D + 128) >> 8;

    *R = (uint8_t)CLIP(r);
    *G = (uint8_t)CLIP(g);
    *B = (uint8_t)CLIP(b);
}

void print_frame_pixels_from_phy(const VIDEO_FRAME_S *frame)
{
    if (!frame) {
        printf("frame is NULL\n");
        return;
    }

    CVI_U64 y_phy = frame->u64PhyAddr[0];
    CVI_U64 vu_phy = frame->u64PhyAddr[1];
    CVI_U32 y_len = frame->u32Length[0];
    CVI_U32 vu_len = frame->u32Length[1];

    if (y_phy == 0 || vu_phy == 0) {
        printf("Invalid physical address.\n");
        return;
    }

    uint8_t *y_plane = (uint8_t *)CVI_SYS_Mmap(y_phy, y_len);
    uint8_t *vu_plane = (uint8_t *)CVI_SYS_Mmap(vu_phy, vu_len);

    if (!y_plane || !vu_plane) {
        printf("Mmap failed.\n");
        return;
    }

    printf("--- NV21 Y plane (first %d bytes) ---\n", PRINT_BYTES);
    for (int i = 0; i < PRINT_BYTES; i++) {
        printf("%02X ", y_plane[i]);
        if ((i + 1) % 8 == 0) printf("\n");
    }
    printf("\n");

    printf("--- NV21 VU plane (first %d bytes) ---\n", PRINT_BYTES);
    for (int i = 0; i < PRINT_BYTES; i++) {
        printf("%02X ", vu_plane[i]);
        if ((i + 1) % 8 == 0) printf("\n");
    }
    printf("\n");

    printf("--- First %d pixels YUV -> RGB888 ---\n", PRINT_BYTES / 2 * 2);
    for (int i = 0; i < PRINT_BYTES / 2; i++) {
        uint8_t V = vu_plane[i * 2 + 0];
        uint8_t U = vu_plane[i * 2 + 1];

        // NV21: 每一对 VU 对应两个 Y 值（两个像素）
        uint8_t Y0 = y_plane[i * 2 + 0];
        uint8_t Y1 = y_plane[i * 2 + 1];

        uint8_t R, G, B;

        yuv_to_rgb(Y0, U, V, &R, &G, &B);
        printf("Pixel %02d: Y=%3d U=%3d V=%3d  ->  RGB=(%3d,%3d,%3d)\n", i * 2, Y0, U, V, R, G, B);

        yuv_to_rgb(Y1, U, V, &R, &G, &B);
        printf("Pixel %02d: Y=%3d U=%3d V=%3d  ->  RGB=(%3d,%3d,%3d)\n", i * 2 + 1, Y1, U, V, R, G, B);
    }

    CVI_SYS_Munmap(y_plane, y_len);
    CVI_SYS_Munmap(vu_plane, vu_len);
}

void dump_video_frame_info(const VIDEO_FRAME_S *frame) {
    if (frame == NULL) {
        printf("VIDEO_FRAME_S is NULL\n");
        return;
    }

    printf("========== VIDEO_FRAME_S DUMP ==========\n");
    printf("u32Width        : %u\n", frame->u32Width);
    printf("u32Height       : %u\n", frame->u32Height);
    printf("enPixelFormat   : %d\n", frame->enPixelFormat);
    printf("enBayerFormat   : %d\n", frame->enBayerFormat);
    printf("enVideoFormat   : %d\n", frame->enVideoFormat);
    printf("enCompressMode  : %d\n", frame->enCompressMode);
    printf("enDynamicRange  : %d\n", frame->enDynamicRange);
    printf("enColorGamut    : %d\n", frame->enColorGamut);

    for (int i = 0; i < 3; i++) {
        printf("u32Stride[%d]    : %u\n", i, frame->u32Stride[i]);
    }

    for (int i = 0; i < 3; i++) {
        printf("u64PhyAddr[%d]   : 0x%llX\n", i, (unsigned long long)frame->u64PhyAddr[i]);
    }

    for (int i = 0; i < 3; i++) {
        printf("pu8VirAddr[%d]   : %p\n", i, frame->pu8VirAddr[i]);
    }

    for (int i = 0; i < 3; i++) {
        printf("u32Length[%d]    : %u\n", i, frame->u32Length[i]);
    }

    printf("s16OffsetTop    : %d\n", frame->s16OffsetTop);
    printf("s16OffsetBottom : %d\n", frame->s16OffsetBottom);
    printf("s16OffsetLeft   : %d\n", frame->s16OffsetLeft);
    printf("s16OffsetRight  : %d\n", frame->s16OffsetRight);

    printf("u32TimeRef      : %u\n", frame->u32TimeRef);
    printf("u64PTS          : %llu\n", (unsigned long long)frame->u64PTS);

    printf("pPrivateData    : %p\n", frame->pPrivateData);
    printf("u32FrameFlag    : %u\n", frame->u32FrameFlag);

    // 打印 Y 平面和 VU 平面前 24 个字节（NV21）
    printf("\n--- NV21 Plane Data Preview (first 24 bytes) ---\n");
    if (frame->pu8VirAddr[0]) {
        printf("Y plane: ");
        for (int i = 0; i < 24; i++) {
            printf("%02X ", frame->pu8VirAddr[0][i]);
        }
        printf("\n");
    }

    if (frame->pu8VirAddr[1]) {
        printf("VU plane: ");
        for (int i = 0; i < 24; i++) {
            printf("%02X ", frame->pu8VirAddr[1][i]);
        }
        printf("\n");
    }

    printf("==========================================\n");
}

int main(void)
{
    printf("z_mmf_demo running \n");
    Z_VI_CTX_S stViCtx;
    VIDEO_FRAME_INFO_S stFrameInfo;
    CVI_S32 s32Ret;

    // 1. 初始化
    s32Ret = Z_VI_INIT(&stViCtx);
    if (s32Ret != CVI_SUCCESS) return -1;

    // 2. 主循环获取和释放帧
    {
        // 帧率统计变量
        struct timeval start_time, current_time, last_fps_time;
        int frame_count = 0;
        float fps = 0.0;
        int fps_interval = 1; // 每秒计算一次帧率

        gettimeofday(&start_time, NULL);
        last_fps_time = start_time;
        int i = 0;
        for (i = 0; i < 100; i++) {
#if 0
            if (Z_VI_TAKE_FRAME(&stViCtx, &stFrameInfo, 1000) == CVI_SUCCESS) {
                frame_count++;

                printf("Got frame: %ux%u\n", stFrameInfo.stVFrame.u32Width, stFrameInfo.stVFrame.u32Height);
//                dump_video_frame_info(&stFrameInfo.stVFrame);
//                print_frame_pixels_from_phy(&stFrameInfo.stVFrame);  // 打印前24字节内容

                Z_VO_PUSH_FRAME(&stFrameInfo);

                // 每秒计算一次帧率
                gettimeofday(&current_time, NULL);
                double time_since_last = (current_time.tv_sec - last_fps_time.tv_sec) +
                                         (current_time.tv_usec - last_fps_time.tv_usec) / 1000000.0;

                if (time_since_last >= fps_interval) {
                    fps = frame_count / time_since_last;
                    printf("Current FPS: %.2f\n", fps);

                    // 重置计数器
                    frame_count = 0;
                    last_fps_time = current_time;
                }

                Z_VI_RELEASE_FRAME(&stViCtx, &stFrameInfo);
            }
#endif

            if(Z_VPSS_TAKE_FRAME(&stFrameInfo, 1000) == CVI_SUCCESS) {
                frame_count++;

                printf("Got frame from vpss: %ux%u\n", stFrameInfo.stVFrame.u32Width, stFrameInfo.stVFrame.u32Height);

                if(Z_VO_PUSH_FRAME(&stFrameInfo) == CVI_SUCCESS) {
                    printf("Push to vo success\n");
                }


                // 每秒计算一次帧率
                gettimeofday(&current_time, NULL);
                double time_since_last = (current_time.tv_sec - last_fps_time.tv_sec) +
                                         (current_time.tv_usec - last_fps_time.tv_usec) / 1000000.0;

                if (time_since_last >= fps_interval) {
                    fps = frame_count / time_since_last;
                    printf("Current FPS: %.2f\n", fps);

                    // 重置计数器
                    frame_count = 0;
                    last_fps_time = current_time;
                }


                Z_VPSS_RELEASE_FRAME(&stFrameInfo);
            }
        }

        // 最终统计
        gettimeofday(&current_time, NULL);
        double total_time = (current_time.tv_sec - start_time.tv_sec) +
                            (current_time.tv_usec - start_time.tv_usec) / 1000000.0;
        printf("Final: processed %d frames in %.2f seconds\n", i, total_time);
    }

    // 3. 反初始化
    Z_VI_DEINIT(&stViCtx);
    printf("z_mmf_demo end \n");
    return 0;
}