#include "lvgl/lvgl.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>


/* 渲染缓冲区 */
#define LCD_WIDTH  120
#define LCD_HEIGHT 40
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
    int32_t x, y;
    //uint16_t * buf_ptr = framebuffer;

    // area 可能是部分区域，所以需要只更新这个区域对应的像素
    // 这里先演示一个简单版本：只允许刷新整个屏幕区域
    //if(area->x1 < 0) area->x1 = 0;
    //if(area->y1 < 0) area->y1 = 0;
    //if(area->x2 >= LCD_WIDTH) area->x2 = LCD_WIDTH - 1;
    //if(area->y2 >= LCD_HEIGHT) area->y2 = LCD_HEIGHT - 1;

    //for(y = area->y1; y <= area->y2; y++) {
    //    for(x = area->x1; x <= area->x2; x++) {
    //        // 计算color_p索引（LVGL每个像素2字节，且color_p指向起始刷新点）
    //        int idx = (y - area->y1) * (area->x2 - area->x1 + 1) + (x - area->x1);
    //        uint16_t color = ((uint16_t *)color_p)[idx];

            // 存入缓冲区对应位置
    //        framebuffer[y * LCD_WIDTH + x] = color;
    //    }
    //}

    lv_display_flush_ready(display);
}

int count = 0;

int main(void)
{
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
	lv_display_set_buffers(disp, buf, NULL, sizeof(buf), LV_DISPLAY_RENDER_MODE_FULL);
	lv_display_set_flush_cb(disp, my_flush_cb);

	// 创建一个标签
	lv_obj_t *label = lv_label_create(lv_screen_active()); 
	lv_label_set_text(label, "2025-06-18");
	lv_obj_center(label);

	lv_timer_handler();
	//printf("Press Enter to continue...\n");
    	//getchar();
	//save_rgb565_file("/root/screen.rgb565");

while (true) {
    lv_timer_handler();
    lv_tick_inc(1000);
    print_ascii_art(buf);
    lv_label_set_text_fmt(label, "%d", count);
    count++;
    usleep(1000000); // 1000ms
}

print_ascii_art(buf);
save_rgb565_file("/root/screen.rgb565");

	return 0;
}

