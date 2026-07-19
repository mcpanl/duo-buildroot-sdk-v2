SHELL = /bin/bash
ifeq ($(PARAM_FILE), )
	PARAM_FILE:=../../mpi_param.mk
	include $(PARAM_FILE)
endif
include ../sample.mk

SDIR = $(CURDIR)
SRCS = $(wildcard $(SDIR)/*.c)
SRCS += $(COMMON_DIR)/sample_common_sys.c
SRCS += $(COMMON_DIR)/sample_common_platform.c
SRCS += $(COMMON_DIR)/sample_common_vi.c
SRCS += $(COMMON_DIR)/sample_common_isp.c
SRCS += $(COMMON_DIR)/sample_common_bin.c
SRCS += $(COMMON_DIR)/sample_common_sensor.c
SRCS += $(COMMON_DIR)/sample_common_vpss.c
SRCS += $(COMMON_DIR)/sample_common_venc.c
INCS = -I$(MW_INC) -I$(ISP_INC) -I../common/ -I$(KERNEL_INC) -I$(SENSOR_LIST_INC) -I$(MW_PATH)/3rdparty/inih
INCS += -I$(MW_PATH)/modules/venc
OBJS = $(SRCS:.c=.o)
DEPS = $(SRCS:.c=.d)

TARGET = sample_sensor_lcd_hevc

PKG_CONFIG_PATH = $(MW_PATH)/pkgconfig
REQUIRES = cvi_common cvi_sample

MW_LIBS = $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs --define-variable=mw_dir=$(MW_PATH) $(REQUIRES))

LIBS = $(MW_LIBS)
LIBS += -lcvi_ispd2 -lsys -lgdc -ldl -lvenc
ifeq ($(MULTI_PROCESS_SUPPORT), 1)
DEFS += -DRPC_MULTI_PROCESS
LIBS += -lnanomsg
endif

EXTRA_CFLAGS = $(INCS) $(DEFS)
EXTRA_LDFLAGS = $(LIBS) $(SNS_LIB) -lpthread -lm -lini

ifeq ($(TARGET_MACHINE), riscv64-unknown-linux-musl)
  EXTRA_LDFLAGS += $(MW_PATH)/modules/bin/prebuilt/riscv64-unknown-linux-musl/libcvi_json-c.a
else ifeq ($(TARGET_MACHINE), riscv64-unknown-linux-gnu)
  EXTRA_LDFLAGS += $(MW_PATH)/modules/bin/prebuilt/riscv64-unknown-linux-gnu/libcvi_json-c.a
else ifeq  ($(TARGET_MACHINE), aarch64-linux-gnu)
  EXTRA_LDFLAGS += $(MW_PATH)/modules/bin/prebuilt/aarch64-linux-gnu/libcvi_json-c.a
else ifeq  ($(TARGET_MACHINE), arm-linux-gnueabihf)
  EXTRA_LDFLAGS += $(MW_PATH)/modules/bin/prebuilt/arm-linux-gnueabihf/libcvi_json-c.a
endif

EXTRA_LDFLAGS += $(MW_LIB)/libvpss.a
ifneq ($(wildcard $(MW_LIB)/libvenc.a),)
EXTRA_LDFLAGS += $(MW_LIB)/libvenc.a
endif

.PHONY : clean all
all: $(TARGET)

$(SDIR)/%.o: $(SDIR)/%.c
	@$(CC) $(DEPFLAGS) $(CFLAGS) $(EXTRA_CFLAGS) -o $@ -c $<
	@echo [$(notdir $(CC))] $(notdir $@)

$(TARGET): $(OBJS) $(ISP_OBJ) $(MW_LIB)/libsys.a
	@$(CXX) -o $@ -Wl,--start-group $(OBJS) -lsys $(MW_LIB)/libsys.a -Wl,--end-group $(ELFFLAGS) $(EXTRA_LDFLAGS)
	@echo -e $(BLUE)[LINK]$(END)[$(notdir $(CXX))] $(notdir $@)

clean:
	@rm -f $(OBJS) $(DEPS) $(TARGET)

-include $(DEPS)
