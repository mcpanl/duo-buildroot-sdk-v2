################################################################################
# 8051 MCU (RTC always-on domain) targets
################################################################################

MCU51_PATH ?= ${TOP_DIR}/mcu51
MCU51_FW_PROJ := ${MCU51_PATH}/sdcc/mars/project/base_project
MCU51_OUT := ${OUTPUT_DIR}/mcu51
MCU51_PREBUILT := ${MCU51_PATH}/prebuilt/mars_mcu_fw.bin

# Prefer system sdcc, then in-tree tools/sdcc (fetch via scripts/fetch_sdcc.sh)
MCU51_SDCC_BIN := $(shell \
	if command -v sdcc >/dev/null 2>&1; then dirname $$(command -v sdcc); \
	elif [ -x ${MCU51_PATH}/tools/sdcc/bin/sdcc ]; then echo ${MCU51_PATH}/tools/sdcc/bin; \
	else echo ""; fi)

.PHONY: mcu51 mcu51-fw mcu51-up mcu51-clean mcu51-install

mcu51: mcu51-fw mcu51-up
	$(call print_target)
	${Q}mkdir -p ${MCU51_OUT}
	${Q}if [ -f ${MCU51_FW_PROJ}/output/mars_mcu_fw.bin ]; then \
		cp -f ${MCU51_FW_PROJ}/output/mars_mcu_fw.bin ${MCU51_OUT}/mars_mcu_fw.bin; \
		cp -f ${MCU51_OUT}/mars_mcu_fw.bin ${MCU51_PREBUILT}; \
	elif [ -f ${MCU51_PREBUILT} ]; then \
		cp -f ${MCU51_PREBUILT} ${MCU51_OUT}/mars_mcu_fw.bin; \
		echo "mcu51: using prebuilt firmware (SDCC not available)"; \
	else \
		echo "mcu51: no firmware produced" >&2; exit 1; \
	fi
	${Q}ls -la ${MCU51_OUT}/mars_mcu_fw.bin

mcu51-fw:
	$(call print_target)
ifeq ($(MCU51_SDCC_BIN),)
	${Q}echo "mcu51: SDCC not found; skip rebuild (use prebuilt or run mcu51/scripts/fetch_sdcc.sh)"
else
	${Q}$(MAKE) -C ${MCU51_FW_PROJ} \
		SDCC_BIN=${MCU51_SDCC_BIN} \
		clean all
endif

mcu51-up:
	$(call print_target)
	${Q}mkdir -p ${MCU51_OUT}
	${Q}$(MAKE) -C ${MCU51_PATH}/tools/mcu51_up clean all \
		CROSS_COMPILE=${CROSS_COMPILE} \
		CFLAGS="-O2 -Wall"
	${Q}cp -f ${MCU51_PATH}/tools/mcu51_up/mcu51_up ${MCU51_OUT}/mcu51_up
	${Q}$(MAKE) -C ${MCU51_PATH}/tools/mcu51_ledctl clean all \
		CROSS_COMPILE=${CROSS_COMPILE} \
		CFLAGS="-O2 -Wall"
	${Q}cp -f ${MCU51_PATH}/tools/mcu51_ledctl/mcu51_ledctl ${MCU51_OUT}/mcu51_ledctl

mcu51-install:
	$(call print_target)
	${Q}${MCU51_PATH}/scripts/install_to_rootfs.sh ${ROOTFS_DIR} ${OUTPUT_DIR}

mcu51-clean:
	$(call print_target)
	${Q}$(MAKE) -C ${MCU51_FW_PROJ} clean || true
	${Q}$(MAKE) -C ${MCU51_PATH}/tools/mcu51_up clean || true
	${Q}$(MAKE) -C ${MCU51_PATH}/tools/mcu51_ledctl clean || true
	${Q}rm -rf ${MCU51_OUT}
