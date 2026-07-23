# RTC 8051 MCU (always-on domain)
#
# LED blink uses RTC DW timer one-shot + state machine (no software busy-wait).
# USER_LED (GPIOA18) shares GPIO_SWPORTA_DR with backlight soft-PWM (GPIOA20,
# owned by FreeRTOS C906L) and other LEDs; DW GPIO has no atomic bit write. While Linux is alive it
# bumps a mailbox heartbeat and MCU does not touch the GPIO bank; after ~2s
# without heartbeat MCU owns the pin. RTC_INFO2 run_ms always advances.
#
# Boot: mcu51-up probes magic + run_ms progress; if MCU already running (e.g.
# survived main-power loss on RTC VBAT) it skips firmware reload. Use -F to force.
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
#   # boot: S30mcu51 loads FW (or skips if already alive) and starts mcu51-hb
#   mcu51-up              # skip load if running
#   mcu51-up -F           # force reload
#   mcu51-hb start          # Linux heartbeat (keeps MCU off the GPIO bank)
#   mcu51-ledctl mode 0     # 300ms ON / 700ms OFF (when MCU owns)
#   mcu51-ledctl mode 1     # 1000ms ON / 1000ms OFF
#   mcu51-ledctl run-ms     # free-running ms since MCU FW start (INFO2)
#   mcu51-ledctl count      # completed blink loops (INFO3[31:8])
#   mcu51-ledctl status     # alive / mode / hb / owner / run_ms / count
#   mcu51-hb stop           # after ~2s MCU takes LED
#   /etc/init.d/S30mcu51 stop   # stop hb + hold MCU reset (safe)
#   /etc/init.d/S30mcu51 start  # reload + hb
#
# Debug stop/resume (NEVER write 0 to RST — kills RTC fabric):
#   busybox devmem 0x05025018 32 0x8107fffd   # hold reset
#   mcu51-up -F                                # reload + release (0x8107ffff)
#
# Hot update over network:
#   scp mars_mcu_fw.bin root@IP:/mnt/data/mcu51/mars_mcu_fw.bin.new
#   ssh root@IP mcu51-update
#
# RTC_INFO mailbox:
#   INFO0 0x0502601c  alive magic 0x8051 (MCU)
#   INFO1 0x05026020  [7:0] blink mode (Linux); [31:16] heartbeat (Linux)
#   INFO2 0x05026024  run_ms free-running (MCU; +phase_ms each timer fire)
#   INFO3 0x05026028  [7:0] owner (0=linux, 1=mcu); [31:8] blink loop count
#
# Layout:
#   sdcc/mars/          firmware sources (SDCC)
#   tools/mcu51_up/     firmware loader
#   tools/mcu51_ledctl/ LED mode / hb / count CLI
#   prebuilt/           committed default bin (<=8KB for SRAM boot)
#   scripts/            install, init, update, heartbeat helpers
#
# Upstream reference: https://github.com/milkv-duo/duo-8051
# Docs: https://milkv.io/zh/docs/duo/getting-started/8051core
