# RTC 8051 MCU (always-on domain)
#
# Build:
#   source build/envsetup_*.sh && defconfig ...
#   make mcu51
#   # or: ./build.sh zonhor-nand mcu51
#
# Optional SDCC toolchain (if not installed via apt):
#   ./mcu51/scripts/fetch_sdcc.sh
#
# On target (USER_LED = GPIOA[18]):
#   # boot auto-loads via /etc/init.d/S30mcu51
#   mcu51-up
#   mcu51-ledctl mode 0    # 300ms ON / 700ms OFF
#   mcu51-ledctl mode 1    # 1000ms ON / 1000ms OFF
#   mcu51-ledctl count     # completed blink loops
#   mcu51-ledctl status
#
# Hot update over network:
#   scp mars_mcu_fw.bin root@IP:/mnt/data/mcu51/mars_mcu_fw.bin.new
#   ssh root@IP mcu51-update
#
# RTC_INFO mailbox:
#   INFO0 0x0502601c  alive magic 0x8051
#   INFO1 0x05026020  blink mode (Linux -> MCU): 0 or 1
#   INFO2 0x05026024  loop count (MCU -> Linux)
#   INFO3 0x05026028  mode echo (MCU)
#
# Layout:
#   sdcc/mars/          firmware sources (SDCC)
#   tools/mcu51_up/     firmware loader
#   tools/mcu51_ledctl/ LED mode / count CLI
#   prebuilt/           committed default bin (<=8KB for SRAM boot)
#   scripts/            install, init, update helpers
#
# Upstream reference: https://github.com/milkv-duo/duo-8051
# Docs: https://milkv.io/zh/docs/duo/getting-started/8051core
