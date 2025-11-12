#include "lvgl/lvgl.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>

volatile sig_atomic_t stopFlag = 0;

#define ALIGN_UP(x, align) (((x) + (align) - 1) & ~((align) - 1))


#define TICK_INTERVAL_MS 25

static uint64_t get_monotonic_time_ms() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}



#define WIDTH 368
#define HEIGHT 552
#define STRIDE ALIGN_UP(WIDTH * 3, 64)

void vo_test_function();
int m_vo_init();
int m_vo_send_frame_from_rgb565(const uint16_t *pRGB565Data);
int m_vo_send_frame_from_rgb888(const uint8_t *pRGB888Data);
int m_vo_send_frame_from_argb8888(const uint32_t *pARGB8888Data);
int m_vo_send_frame_from_nv12(const uint8_t *y, const uint8_t *uv);
void m_vo_deinit();

void handle_sigint(int signum) {
	printf("Sign %d\n", signum);
    stopFlag = 1;
}

/* 渲染缓冲区 */
#define ASCII_CHARS "****  "  // 从黑到白的字符映射（你可以自行调整）
#define ASCII_LEN (sizeof(ASCII_CHARS) - 1)


lv_display_t *disp;

static uint8_t buf[WIDTH * 3 * HEIGHT];
static uint8_t buf2[STRIDE * HEIGHT];

uint8_t framebuffer[STRIDE * HEIGHT];

static uint8_t rotated_buffer[STRIDE * HEIGHT];


static void copy_rgb888_with_stride(uint8_t *dst, const uint8_t *src, int width, int height, int stride)
{
    int row_bytes = width * 3;
    for (int y = 0; y < height; y++) {
        memcpy(dst + y * stride, src + y * row_bytes, row_bytes);
        // 其余 stride - row_bytes 处的数据可以保持不动（VO通常不关心 padding 内容）
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

            // BT.601 conversion
            uint8_t y = (uint8_t)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
            y_plane[j * y_stride + i] = y;

            if ((j % 2 == 0) && (i % 2 == 0)) {
                // 取 2x2 像素块的左上像素估算 UV（快速做法）
                uint8_t u = (uint8_t)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                uint8_t v = (uint8_t)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);

                int uv_index = (j / 2) * uv_stride + i;
                uv_plane[uv_index] = u;
                uv_plane[uv_index + 1] = v;
            }
        }
    }
}

#if 1
void my_flush_cb(lv_display_t * display, const lv_area_t * area, unsigned char * color_p)
{
    const uint8_t *src = (const uint8_t *)color_p;

    // 拷贝原始 buf 到 buf2，增加对齐填充
    copy_rgb888_with_stride(buf2, src, WIDTH, HEIGHT, STRIDE);

    // 发送到 VO 层
    m_vo_send_frame_from_rgb888(buf2);

    // 通知 LVGL 刷新完成
    lv_display_flush_ready(display);
}
#endif

int count = 0;

int main(void)
{

	signal(SIGINT, handle_sigint);

	printf("Calling function from libvo_module...\n");
	vo_test_function();

	printf("Initializing VO system...\n");
	if (m_vo_init() != 0) {
		printf("VO initialization failed.\n");
		return -1;
	}


	printf("INIT LVGL\n");
	// LVGLInit
	lv_init();

	printf("INIT LVGL DONE 2\n");

	memset(buf, 0, sizeof(buf));
	memset(framebuffer, 0, sizeof(framebuffer));
	//save_rgb565_file("/root/screen.rgb565");
	//return 0;

	disp = lv_display_create(WIDTH, HEIGHT);
	lv_display_set_resolution(disp, WIDTH, HEIGHT);
	//lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270);
	lv_display_set_buffers(disp, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_FULL);
	lv_display_set_flush_cb(disp, my_flush_cb);

	// 创建一个标签
	lv_obj_t *label = lv_label_create(lv_screen_active());
	lv_label_set_text(label, "-");
	lv_obj_center(label);

	//lv_timer_handler();
	//printf("Press Enter to continue...\n");
	//getchar();
	//save_rgb565_file("/root/screen.rgb565");

	uint64_t last_tick = get_monotonic_time_ms();
	uint64_t current_tick;

#if 1
	while (stopFlag == 0) {
		current_tick = get_monotonic_time_ms();
		uint64_t elapsed = current_tick - last_tick;
		last_tick = current_tick;
		lv_timer_handler();
		lv_tick_inc(elapsed);
		//print_ascii_art(buf);
		lv_label_set_text_fmt(label, "%d", count);
		count++;

		/*
		if (count > 100) {
			break;
		}
		*/

		// 精确延迟 33ms
		struct timespec req = {
			.tv_sec = 0,
			.tv_nsec = TICK_INTERVAL_MS * 1000000L
		};

		nanosleep(&req, NULL);

	}
#endif

	//print_ascii_art(buf);
	//save_rgb565_file("/root/screen.rgb565");

	m_vo_deinit();

	return 0;
}

