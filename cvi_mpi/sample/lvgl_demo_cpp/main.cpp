#include <cstdio>
#include <cstring>
#include <csignal>
#include <ctime>
#include <unistd.h>

// LVGL 是 C 库，需要 extern "C"
extern "C" {
    #include "lvgl/lvgl.h"

    // 假设这些 C 函数存在于 libvo_module.a / libvo.a 里
    void vo_test_function();
    int m_vo_init();
    int m_vo_send_frame_from_rgb565(const uint16_t *pRGB565Data);
    int m_vo_send_frame_from_rgb888(const uint8_t *pRGB888Data);
    int m_vo_send_frame_from_argb8888(const uint32_t *pARGB8888Data);
    int m_vo_send_frame_from_nv12(const uint8_t *y, const uint8_t *uv);
    void m_vo_deinit();
}

// ===================== 全局定义 =====================

volatile sig_atomic_t stopFlag = 0;

#define ALIGN_UP(x, align) (((x) + (align) - 1) & ~((align) - 1))
#define TICK_INTERVAL_MS 25

#define WIDTH  368
#define HEIGHT 552
#define STRIDE ALIGN_UP(WIDTH * 3, 64)

#define ASCII_CHARS "****  "
#define ASCII_LEN (sizeof(ASCII_CHARS) - 1)

static uint8_t buf[WIDTH * 3 * HEIGHT];
static uint8_t buf2[STRIDE * HEIGHT];
static uint8_t framebuffer[STRIDE * HEIGHT];
static uint8_t rotated_buffer[STRIDE * HEIGHT];

lv_display_t *disp = nullptr;

int count = 0;

// ===================== 工具函数 =====================

static uint64_t get_monotonic_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}

void handle_sigint(int signum) {
    std::printf("Sign %d\n", signum);
    stopFlag = 1;
}

static void copy_rgb888_with_stride(uint8_t *dst, const uint8_t *src, int width, int height, int stride)
{
    int row_bytes = width * 3;
    for (int y = 0; y < height; y++) {
        std::memcpy(dst + y * stride, src + y * row_bytes, row_bytes);
    }
}

void rgb888_to_nv12(const uint8_t* rgb, uint8_t* y_plane, uint8_t* uv_plane, int width, int height)
{
    int y_stride = width;
    int uv_stride = width;

    for (int j = 0; j < height; j++) {
        for (int i = 0; i < width; i++) {
            int idx_rgb = (j * width + i) * 3;

            uint8_t r = rgb[idx_rgb];
            uint8_t g = rgb[idx_rgb + 1];
            uint8_t b = rgb[idx_rgb + 2];

            uint8_t y = static_cast<uint8_t>(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
            y_plane[j * y_stride + i] = y;

            if ((j % 2 == 0) && (i % 2 == 0)) {
                uint8_t u = static_cast<uint8_t>(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                uint8_t v = static_cast<uint8_t>(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
                int uv_index = (j / 2) * uv_stride + i;
                uv_plane[uv_index] = u;
                uv_plane[uv_index + 1] = v;
            }
        }
    }
}

// ===================== LVGL flush 回调 =====================

void my_flush_cb(lv_display_t * display, const lv_area_t * area, unsigned char * color_p)
{
    const uint8_t *src = reinterpret_cast<const uint8_t *>(color_p);

    copy_rgb888_with_stride(buf2, src, WIDTH, HEIGHT, STRIDE);
    m_vo_send_frame_from_rgb888(buf2);
    lv_display_flush_ready(display);
}

// ===================== 主函数 =====================

int main()
{
    signal(SIGINT, handle_sigint);

    std::printf("Calling function from libvo_module...\n");
    vo_test_function();

    std::printf("Initializing VO system...\n");
    if (m_vo_init() != 0) {
        std::printf("VO initialization failed.\n");
        return -1;
    }

    std::printf("INIT LVGL\n");
    lv_init();
    std::printf("INIT LVGL DONE\n");

    std::memset(buf, 0, sizeof(buf));
    std::memset(framebuffer, 0, sizeof(framebuffer));

    disp = lv_display_create(WIDTH, HEIGHT);
    lv_display_set_resolution(disp, WIDTH, HEIGHT);
    lv_display_set_buffers(disp, buf, nullptr, sizeof(buf), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, my_flush_cb);

    lv_obj_t *label = lv_label_create(lv_screen_active());
    lv_label_set_text(label, "-");
    lv_obj_center(label);

    uint64_t last_tick = get_monotonic_time_ms();

    while (!stopFlag) {
        uint64_t current_tick = get_monotonic_time_ms();
        uint64_t elapsed = current_tick - last_tick;
        last_tick = current_tick;

        lv_timer_handler();
        lv_tick_inc(elapsed);
        lv_label_set_text_fmt(label, "Count = %d", count);
        count++;

        struct timespec req = {
            .tv_sec = 0,
            .tv_nsec = TICK_INTERVAL_MS * 1000000L
        };
        nanosleep(&req, nullptr);
    }

    m_vo_deinit();
    return 0;
}

