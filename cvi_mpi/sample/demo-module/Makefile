SHELL = /bin/bash

ifeq ("$(CHIP_ARCH)", "CV180X")
all:
clean:
else
ifeq ($(PARAM_FILE), )
        PARAM_FILE:=../../mpi_param.mk
        include $(PARAM_FILE)
endif
include ../sample.mk

SDIR = $(CURDIR)
SRCS = $(wildcard $(SDIR)/demo.c)
INCS = -I$(MW_INC) -I$(ISP_INC) -I$(KERNEL_INC) -I$(PANEL_INC) -I../common -I$(SENSOR_LIST_INC)
OBJS = $(SRCS:.c=.o)
DEPS = $(SRCS:.c=.d)

TARGET = demo
ifeq ($(CONFIG_ENABLE_SDK_ASAN), y)
TARGET = demo_asan
endif

LIBS = -lvo -lsys -lmipi_tx -lmisc

ifeq ($(MULTI_PROCESS_SUPPORT), 1)
DEFS += -DRPC_MULTI_PROCESS
LIBS += -lnanomsg
LIBS += -lvenc -lvdec -lmisc
endif

EXTRA_CFLAGS = $(INCS)
EXTRA_LDFLAGS = $(LIBS) -lm -lpthread -latomic
EXTRA_LDFLAGS += -lcvi_bin -lcvi_bin_isp $(ISP_LIB)
EXTRA_LDFLAGS += -L../../lib


# ======================== 新增部分开始 ================================
# 模块库输出名
MODULE_SO  = libvo_module.so
MODULE_A   = libvo_module.a

# 仅用于构建库的源文件（排除带有 main 的）
LIB_SRCS = $(filter-out $(SDIR)/main.c, $(SRCS))
LIB_OBJS = $(LIB_SRCS:.c=.o)

# 共享库目标
$(MODULE_SO): $(LIB_OBJS)
	$(CC) -fPIC -shared -o $@ $^ $(EXTRA_LDFLAGS)
	@echo -e "\033[32m[BUILD]\033[0m $@"

# 静态库目标
$(MODULE_A): $(LIB_OBJS)
	$(AR) rcs $@ $^
	@echo -e "\033[32m[AR]\033[0m $@"

.PHONY: libso liba
libso: $(MODULE_SO)
liba: $(MODULE_A)

# ======================== 新增部分结束 ================================



.PHONY : clean all lt9611 clean_lt9611
all: $(TARGET) lt9611

$(SDIR)/%.o: $(SDIR)/%.c
	@$(CC) $(DEPFLAGS) $(CFLAGS) $(EXTRA_CFLAGS) -o $@ -c $<
	@echo [$(notdir $(CC))] $(notdir $@)

$(TARGET): $(OBJS) $(MW_LIB)/libvo.a $(MW_LIB)/libsys.a $(MW_LIB)/libmipi_tx.a
	@$(CXX) -o $@ $(OBJS) $(ELFFLAGS) $(EXTRA_LDFLAGS)
	@echo -e $(BLUE)[LINK]$(END)[$(notdir $(CC))] $(notdir $@)

lt9611:
	cd $(SDIR)/lt9611;make clean;make

clean_lt9611:
	cd $(SDIR)/lt9611;make clean;

clean: clean_lt9611
	@rm -f $(OBJS) $(DEPS) $(TARGET)

-include $(DEPS)
endif
