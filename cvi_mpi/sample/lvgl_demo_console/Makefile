# 编译器设置
CC = gcc
CFLAGS = -I. -Ilvgl -std=c99 -Wall -g

# 源文件
SRCS = main.c $(wildcard lvgl/**/*.c)

# 目标文件
OBJS = $(SRCS:.c=.o)

# 输出文件
TARGET = main

# 铅接时需要的数据
LDFLAGS =

# 依赖
DEPENDS = lv_conf.h

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c $(DEPENDS)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

