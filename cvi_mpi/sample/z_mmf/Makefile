SHELL = /bin/bash
ifeq ($(PARAM_FILE), )
	PARAM_FILE:=../../mpi_param.mk
	include $(PARAM_FILE)
endif
include ../sample.mk

SDIR = $(CURDIR)
SRCS = $(wildcard $(SDIR)/*.c)
SRCS += $(COMMON_DIR)/sample_common_vo.c
SRCS += $(COMMON_DIR)/sample_common_sys.c
SRCS += $(COMMON_DIR)/sample_common_platform.c
SRCS += $(COMMON_DIR)/sample_common_vpss.c
SRCS += $(COMMON_DIR)/sample_common_vi.c
SRCS += $(COMMON_DIR)/sample_common_isp.c
SRCS += $(COMMON_DIR)/sample_common_bin.c
SRCS += $(COMMON_DIR)/sample_common_sensor.c

INCS = -I$(MW_INC) -I$(ISP_INC) -I../common/ -I$(KERNEL_INC) -I$(SENSOR_LIST_INC) -I$(PANEL_INC) -I$(MW_PATH)/3rdparty/inih
OBJS = $(SRCS:.c=.o)
DEPS = $(SRCS:.c=.d)

# 生成静态库
TARGET = libz_mmf.a

EXTRA_CFLAGS = $(INCS) $(DEFS)

# 如果需要 IVE 支持
ifeq ($(IVE_SUPPORT), 1)
CFLAGS += -DIVE_SUPPORT

IVE_PATH = $(MW_PATH)/../install/soc_cv1835_wevb_0002a_spinand/tpu_64/cvitek_ive_sdk
EXTRA_CFLAGS += -I$(IVE_PATH)/include/ive

TPU_PATH = $(MW_PATH)/../install/soc_cv1835_wevb_0002a_spinand/tpu_64/cvitek_tpu_sdk
EXTRA_CFLAGS += -I$(TPU_PATH)/include
endif

.PHONY : clean all

# 默认目标
all: $(TARGET)

# 编译规则
$(SDIR)/%.o: $(SDIR)/%.c
	@$(CC) $(DEPFLAGS) $(CFLAGS) $(EXTRA_CFLAGS) -o $@ -c $<
	@echo [$(notdir $(CC))] $(notdir $@)

# 链接为静态库
$(TARGET): $(OBJS)
	@ar rcs $@ $(OBJS)
	@echo -e $(BLUE)[AR]$(END) $(notdir $@)

clean:
	@rm -f $(OBJS) $(DEPS) $(TARGET)

-include $(DEPS)
