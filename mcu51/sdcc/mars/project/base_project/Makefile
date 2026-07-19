
PROJECT       = mars_mcu_fw

BUILD_DIR     = $(CURDIR)
BUILDDIR      = $(BUILD_DIR)
OBJDIR        = $(BUILDDIR)/obj
OUTDIR        = $(BUILDDIR)/output

src_dir      := .
inc_dir      := ../../  
c_srcs       :=
asm_srcs     :=
TARGET_BIN    = $(OUTDIR)/$(PROJECT).bin
TARGET_LK     = $(OUTDIR)/$(PROJECT).lk

#put the critical source code in driver.mk, do not change the order of mk file
include $(BUILD_DIR)/../../driver/driver.mk
include $(BUILD_DIR)/src/src.mk
include $(BUILD_DIR)/../../lib/lib.mk

# Force SDCC (ignore host CC= from environment).
# Prefer: make SDCC_BIN=/path/to/bin   or mcu51/tools/sdcc (see scripts/fetch_sdcc.sh)
MCU51_ROOT := $(abspath $(BUILD_DIR)/../../../..)
ifeq ($(origin SDCC_BIN),command line)
  # keep caller-provided SDCC_BIN
else
  SDCC_BIN := $(MCU51_ROOT)/tools/sdcc/bin
  ifeq ($(wildcard $(SDCC_BIN)/sdcc),)
    SDCC_BIN := $(dir $(shell command -v sdcc 2>/dev/null))
  endif
endif
CC  := $(SDCC_BIN)/sdcc
AS  := $(SDCC_BIN)/sdas8051
LD  := $(SDCC_BIN)/sdld
SDOBJCOPY := $(SDCC_BIN)/sdobjcopy
SRCPATHS   = $(sort $(dir $(c_srcs)) $(dir $(asm_srcs)))
COBJS      = $(addprefix $(OBJDIR)/, $(notdir $(c_srcs:.c=.c.rel)))
ASMOBJS    = $(addprefix $(OBJDIR)/, $(notdir $(asm_srcs:.asm=.asm.rel)))

IINC_DIR   = $(patsubst %,-I%,$(inc_dir))
OBJS       = $(COBJS) $(ASMOBJS)

VPATH     = $(SRCPATHS)

# ------------------------------------------------------
# Memory Layout

# PRG Size = 4K Bytes
CODE_SIZE = --code-loc 0x0000 --code-size 0x1F00

# INT-MEM Size = 256 Bytes
#IRAM_SIZE = --idata-loc 0x0000  --iram-size 256

# EXT-MEM Size = 32K Bytes
XRAM_SIZE = --xram-loc 0x1900 --xram-size 0x400

# ------------------------------------------------------
# MCS51 Options

# Memory Model (small, medium, large, huge)
MODEL  = large

# LIBS    =
# LIBPATH = -L $(LIBDIR)
DEBUG =
#DEBUG = --Werror
#DEBUG = --debug

EXTRA_CFLAG = --int-long-reent
EXTRA_LFLAG = --stack-auto

AFLAGS = -plosgff
CFLAGS = -I./include $(IINC_DIR) -mmcs51 --model-$(MODEL) $(EXTRA_CFLAG) --out-fmt-ihx  $(DEBUG)
CFLAGS +=
LFLAGS = $(LIBPATH) $(LIBS) -mmcs51 --model-$(MODEL) $(CODE_SIZE) $(IRAM_SIZE) $(XRAM_SIZE) --out-fmt-ihx $(DEBUG) $(EXTRA_LFLAG)

.PHONY: all clean

all: $(BUILDDIR) $(OUTDIR) $(OBJDIR) $(OBJS) $(TARGET_BIN)

$(BUILDDIR):
	@echo create $@
	@mkdir -p $(BUILDDIR)

$(OBJDIR):
	@echo create $@
	@mkdir -p $(OBJDIR)

$(OUTDIR):
	@echo create $@
	@mkdir -p $(OUTDIR)

%bin: %ihx
	@echo Generate $@
	@$(SDOBJCOPY) -I ihex -O binary $^ $@
	split -b 8k $@ $(OUTDIR)/$(PROJECT)_

%.ihx: $(COBJS) $(ASMOBJS)
	@echo Generate $@
	@echo $(S) $(CC) -o $@ $(LFLAGS) $^
	@$(S) $(CC) -o $@ $(LFLAGS) $^
	@$(LD) -nf $(TARGET_LK)

$(COBJS): $(OBJDIR)/%.c.rel: %.c
	@echo CC $<
	@$(CC) -o $@ $(CFLAGS) -c $^

$(ASMOBJS): $(OBJDIR)/%.asm.rel: %.asm
	@$(AS) $(AFLAGS) -o $@ $^

clean:
	$(S) rm -rf $(OBJDIR) $(OUTDIR)

