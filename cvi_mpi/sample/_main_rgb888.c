#include "lvgl/lvgl.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>


#define TICK_INTERVAL_MS 33

static uint64_t get_monotonic_time_ms() {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}



#define WIDTH 768
#define HEIGHT 1280

void vo_test_function();
int m_vo_init();
int m_vo_send_frame_from_rgb565(const uint16_t *pRGB565Data);
int m_vo_send_frame_from_rgb888(const uint8_t *pRGB888Data);
void m_vo_deinit();


/* 渲染缓冲区 */
#define LCD_WIDTH  768
#define LCD_HEIGHT 1280
#define ASCII_CHARS "****  "  // 从黑到白的字符映射（你可以自行调整）
#define ASCII_LEN (sizeof(ASCII_CHARS) - 1)


lv_display_t *disp;

static uint8_t buf[LCD_WIDTH * LCD_HEIGHT * 3];

uint8_t framebuffer[LCD_WIDTH * LCD_HEIGHT * 3];

static uint8_t rotated_buffer[LCD_WIDTH * LCD_HEIGHT * 3];

#if 0
void my_flush_cb(lv_display_t * display, const lv_area_t * area, unsigned char * color_p)
{
	const int logic_w = lv_area_get_width(area);   // e.g. 1280
	const int logic_h = lv_area_get_height(area);  // e.g. 768

	const uint16_t *src = (const uint16_t *)color_p;

	static uint16_t rotated_buf[WIDTH * HEIGHT];  // e.g. 768x1280 = 983040
	uint16_t *dst = rotated_buf;

	// 用于 LV_DISPLAY_ROTATION_270 的 flush_cb 旋转
	for (int y = 0; y < logic_h; y++) {
		for (int x = 0; x < logic_w; x++) {
			int src_index = y * logic_w + x;

			int dst_x = logic_h - 1 - y;
			int dst_y = x;
			int dst_index = dst_y * logic_h + dst_x;

			dst[dst_index] = src[src_index];
		}
	}

	m_vo_send_frame_from_rgb565(rotated_buf);  // 发送旋转后的 RGB565 数据

	lv_display_flush_ready(display);  // 通知 LVGL 刷新完成
}



void ___my_flush_cb(lv_display_t * display, const lv_area_t * area, unsigned char * color_p)
{
	const int logic_w = lv_area_get_width(area);   // e.g. 1280
	const int logic_h = lv_area_get_height(area);  // e.g. 768

	const uint16_t *src = (const uint16_t *)color_p;

	static uint16_t rotated_buf[WIDTH * HEIGHT];  // e.g. 768x1280 = 983040
	uint16_t *dst = rotated_buf;

	// 手动把 color_p 逆时针旋转 90 度：将 LVGL 逻辑横屏 -> VO 物理竖屏
	for (int y = 0; y < logic_h; y++) {
		for (int x = 0; x < logic_w; x++) {
			int src_index = y * logic_w + x;

			// 旋转公式：dst(x, y) = src(y, W - 1 - x)
			int dst_x = y;
			int dst_y = logic_w - 1 - x;
			int dst_index = dst_y * logic_h + dst_x;

			dst[dst_index] = src[src_index];
		}
	}

	m_vo_send_frame_from_rgb565(rotated_buf);  // 发送旋转后的 RGB565 数据

	lv_display_flush_ready(display);  // 通知 LVGL 刷新完成
}


// LVGL 刷新回调函数，直接整屏刷新
void __my_flush_cb(lv_display_t * display, const lv_area_t * area, unsigned char * color_p)
{
	// color_p 是指向 RGB565 图像缓冲区的指针
	// 转为 uint16_t 指针
	const uint16_t *rgb565_data = (const uint16_t *)color_p;

	// 发送帧给视频输出（全屏）
	m_vo_send_frame_from_rgb565(rgb565_data);

	// 通知 LVGL 刷新完成（一定要加）
	lv_display_flush_ready(display);
}

void _my_flush_cb(lv_display_t * display, const lv_area_t * area, unsigned char * color_p)
{
	int32_t x, y;

	lv_display_flush_ready(display);
}
#endif

void rotate_rgb888_270ccw(const uint8_t *src, uint8_t *dst, int src_w, int src_h)
{
    int dst_w = src_h;
    int dst_h = src_w;

    for (int y = 0; y < src_h; y++) {
        for (int x = 0; x < src_w; x++) {
            int src_index = (y * src_w + x) * 3;

            // 顺时针旋转90度（270度逆时针）
            int dst_x = src_h - y - 1;
            int dst_y = x;
            int dst_index = (dst_y * dst_w + dst_x) * 3;

            dst[dst_index + 0] = src[src_index + 0]; // R
            dst[dst_index + 1] = src[src_index + 1]; // G
            dst[dst_index + 2] = src[src_index + 2]; // B
        }
    }
}




void my_flush_cb(lv_display_t * display, const lv_area_t * area, unsigned char * color_p)
{
	const uint8_t *src = (const uint8_t *)color_p;
 	
	printf("  REQ\n");

	rotate_rgb888_270ccw(src, rotated_buffer, LCD_HEIGHT, LCD_WIDTH);

	// 发送帧给视频输出（全屏）
        m_vo_send_frame_from_rgb888(rotated_buffer);

        // 通知 LVGL 刷新完成（一定要加）
        lv_display_flush_ready(display);
}

int count = 0;

int main(void)
{
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

	disp = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
	lv_display_set_resolution(disp, LCD_WIDTH, LCD_HEIGHT);
	lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270);
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
	while (true) {
		current_tick = get_monotonic_time_ms();
		uint64_t elapsed = current_tick - last_tick;
		last_tick = current_tick;
		lv_timer_handler();
		lv_tick_inc(elapsed);
		//print_ascii_art(buf);
		lv_label_set_text_fmt(label, "%d", count);
		count++;

		if (count > 300) {
			break;
		}

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

