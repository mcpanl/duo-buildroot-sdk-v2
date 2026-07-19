SHELL = /bin/bash
ifeq ($(PARAM_FILE), )
	PARAM_FILE:=../../mpi_param.mk
	include $(PARAM_FILE)
endif
include ../sample.mk

TARGET = lcd_fb_color_bench
SRCS = lcd_fb_color_bench.c

CFLAGS += -Wall -Wextra -O2
LDFLAGS := -static

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(SRCS)
	@$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)
	@echo -e $(BLUE)[LINK]$(END)[$(notdir $(CC))] $(notdir $@)

clean:
	@rm -f $(TARGET)
