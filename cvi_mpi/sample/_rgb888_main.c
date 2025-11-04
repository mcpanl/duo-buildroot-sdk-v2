#include "lvgl/lvgl.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>


#define TICK_INTERVAL_MS 35

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
void m_vo_deinit();


/* 渲染缓冲区 */
#define LCD_WIDTH  768
#define LCD_HEIGHT 1280
#define ASCII_CHARS "****  "  // 从黑到白的字符映射（你可以自行调整）
#define ASCII_LEN (sizeof(ASCII_CHARS) - 1)


lv_display_t *disp;

static uint16_t buf[LCD_WIDTH * LCD_HEIGHT];

uint16_t framebuffer[LCD_WIDTH * LCD_HEIGHT];

// 将 RGB565 转换为灰度值
uint8_t rgb565_to_gray(uint16_t pixel) {
	uint8_t r = ((pixel >> 11) & 0x1F) << 3;  // 5位红，左移3补足8位
	uint8_t g = ((pixel >> 5) & 0x3F) << 2;   // 6位绿，左移2补足8位
	uint8_t b = (pixel & 0x1F) << 3;          // 5位蓝，左移3补足8位

	// 灰度转换（加权平均法）
	uint8_t gray = (uint8_t)(0.299 * r + 0.587 * g + 0.114 * b);
	return gray;
}

// 将灰度值映射到字符
char gray_to_char(uint8_t gray) {
	int index = gray * ASCII_LEN / 256;
	if (index >= ASCII_LEN) index = ASCII_LEN - 1;
	return ASCII_CHARS[index];
}

// 显示图像缓冲区为 ASCII 图
void print_ascii_art(uint16_t *buf) {
	for (int y = 0; y < LCD_HEIGHT; y++) {
		for (int x = 0; x < LCD_WIDTH; x++) {
			uint16_t pixel = buf[y * LCD_WIDTH + x];
			uint8_t gray = rgb565_to_gray(pixel);
			char ch = gray_to_char(gray);
			putchar(ch);
		}
		putchar('\n');
	}
}


void save_rgb565_file(const char *filename) {
	FILE *file = fopen(filename, "wb");

	if (file == NULL) {
		printf("Unable to open %s for writing\n",filename);
		return;
	}
	fwrite(buf, sizeof(uint16_t), LCD_WIDTH * LCD_HEIGHT, file);
	fclose(file);
	printf("A Screen buffer successfully saved to %s\n",filename);
}


void my_flush_cb(lv_display_t * display, const lv_area_t * area, unsigned char * color_p)
{
	const int logic_w = lv_area_get_width(area);   // e.g. 1280
	const int logic_h = lv_area_get_height(area);  // e.g. 768

	const uint16_t *src = (const uint16_t *)color_p;

	static uint16_t rotated_buf[WIDTH * HEIGHT];  // e.g. 768x1280 = 983040
	uint16_t *dst = rotated_buf;

	// 用于 LV_DISPLAY_ROTATION_270 的 flush_cb 旋转
#if 1
	for (int y = 0; y < logic_h; y++) {
		for (int x = 0; x < logic_w; x++) {
			int src_index = y * logic_w + x;

			int dst_x = logic_h - 1 - y;
			int dst_y = x;
			int dst_index = dst_y * logic_h + dst_x;

			dst[dst_index] = src[src_index];
		}
	}
#endif
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

	while (true) {
		current_tick = get_monotonic_time_ms();
		uint64_t elapsed = current_tick - last_tick;
		last_tick = current_tick;
		lv_timer_handler();
		lv_tick_inc(elapsed);
		//print_ascii_art(buf);
		lv_label_set_text_fmt(label, "%d", count);
		count++;

		if (count > 800) {
			break;
		}

		// 精确延迟 35ms
		struct timespec req = {
			.tv_sec = 0,
			.tv_nsec = TICK_INTERVAL_MS * 1000000L
		};

		nanosleep(&req, NULL);

	}

	//print_ascii_art(buf);
	//save_rgb565_file("/root/screen.rgb565");

	m_vo_deinit();

	return 0;
}

