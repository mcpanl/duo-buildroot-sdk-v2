#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>  // for sleep()

#define WIDTH 768
#define HEIGHT 1280

void vo_test_function();
int m_vo_init();
int m_vo_send_frame_from_rgb565(const uint16_t *pRGB565Data);
void m_vo_deinit();

int main() {
    printf("Calling function from libvo_module...\n");
    vo_test_function();

    printf("Initializing VO system...\n");
    if (m_vo_init() != 0) {
        printf("VO initialization failed.\n");
        return -1;
    }

    printf("Allocating RGB565 buffer...\n");
    size_t pixel_count = WIDTH * HEIGHT;
    uint16_t *pRGB565Data = (uint16_t *)malloc(pixel_count * sizeof(uint16_t));
    if (!pRGB565Data) {
        printf("Failed to allocate RGB565 buffer.\n");
        m_vo_deinit();
        return -1;
    }

    // 填充红色背景（RGB565: 0b1111100000000000）
    uint16_t red565 = 0xF800;
    for (size_t i = 0; i < pixel_count; i++) {
        pRGB565Data[i] = red565;
    }

    printf("Sending frame to VO...\n");
    int ret = m_vo_send_frame_from_rgb565(pRGB565Data);
    if (ret != 0) {
        printf("Failed to send frame. Error code: %d\n", ret);
        free(pRGB565Data);
        m_vo_deinit();
        return -1;
    }

    printf("Frame sent. Waiting for 2 seconds...\n");
    sleep(2);  // 等待2秒以查看效果

    printf("Deinitializing VO...\n");
    free(pRGB565Data);
    m_vo_deinit();

    printf("Done.\n");
    return 0;
}

