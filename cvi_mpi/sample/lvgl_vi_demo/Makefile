# 编译器设置（改为交叉编译器）
CC = aarch64-linux-gnu-gcc

# 编译选项
CFLAGS = -I. -Ilvgl -I../../include -I../common -I../../modules/isp/include -std=c99 -Wall -g

# 源文件
SRCS = main.c $(wildcard lvgl/**/*.c)

# 目标文件
OBJS = $(SRCS:.c=.o)

# 输出文件
TARGET = main_static

# 链接参数（添加静态库和系统库）
LDFLAGS = ../demo-module/libvo_module.a ../../lib/libvo.a ../../lib/libsys.a ../../lib/libmipi_tx.a ../../lib/libmisc.a -lpthread -latomic -lm -static

# 依赖头文件
DEPENDS = lv_conf.h

# 默认目标
all: $(TARGET)

# 链接目标
$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

# 编译目标
%.o: %.c $(DEPENDS)
	$(CC) $(CFLAGS) -c $< -o $@

# 清理
clean:
	rm -f $(OBJS) $(TARGET)

