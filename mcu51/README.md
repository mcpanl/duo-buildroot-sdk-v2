# RTC 8051 MCU (always-on domain)
#
# Status LED blink uses RTC DW timer one-shot + state machine (no busy-wait).
# Hardware: MCU LED is on GPIOE0 / PWR_GPIO[0] in the RTC power domain
# (active-high). MCU never touches main-domain GPIOA.
#
# Default boot behavior: MCU LED is OFF (mode 3). Linux can override via
# RTC_INFO mailbox: on / off / blink / release (back to default OFF).
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
# On target (MCU LED = GPIOE[0] / PWR_GPIO[0]):
#   # boot: S30mcu51 syncs newer factory FW over stale /mnt/data runtime,
#   # loads FW (or skips if already alive) and starts with LED OFF
#   mcu51-up              # skip load if running
#   mcu51-up -F           # force reload
#
# WARNING: mcu51-up prefers /mnt/data/mcu51/mars_mcu_fw.bin over factory.
# After an image upgrade, S30mcu51 copies factory -> runtime when factory is
# newer so GPIOE0 firmware is not shadowed by an old A18 blink binary.
#   mcu51-ledctl mode 0   # blink 300ms ON / 700ms OFF
#   mcu51-ledctl mode 1   # blink 1000ms ON / 1000ms OFF
#   mcu51-ledctl on       # constant ON
#   mcu51-ledctl off      # constant OFF (default / release)
#   mcu51-ledctl blink 0  # same as mode 0
#   mcu51-ledctl release  # restore default OFF
#   mcu51-ledctl run-ms   # free-running ms since MCU FW start (INFO2)
#   mcu51-ledctl count    # completed blink loops (INFO3[31:8])
#   mcu51-ledctl status   # alive / mode / hb / run_ms / count
#   /etc/init.d/S30mcu51 stop   # stop hb + hold MCU reset (safe)
#   /etc/init.d/S30mcu51 start  # reload + default LED OFF
#
# Board status LEDs (Zonhor SG2000):
#   Linux  sys-led  GPIOA29  (gpio-leds, default activity)
#   RTOS   status   GPIOA18  (FreeRTOS task, Linux via rtos_cmdqu / zonhor-ledctl)
#   MCU    status   GPIOE0   (this firmware, Linux via mcu51-ledctl / zonhor-ledctl)
#
# Mem wake (Zonhor), active-low (pull-up, press to GND):
#   GPIOE1 / PWR_GPIO1 — only production path: MCU polls in ST_SUSP and
#     arms RTC alarm + RTC_EN_PWR_WAKEUP=0x30. Flywire + pull-up required.
#   Shared with AXP2101 PWRON: external pull-up to VDDIO_RTC 1.8V; MCU must
#     keep GPIOE1 as input only (never drive).
#   PWR_WAKEUP0 / PWR_BUTTON1 — reserved, unconnected; U-Boot must NOT mux
#     them as wake (float + active-low caused instant mem resume). Left as
#     PWR_GPIO_6/8; mask is 0x30 only (no 0x173F).
#   DTS: only gpio-keys-rtc (PWR_GPIO1) keeps wakeup-source; user-button /
#     AXP / BT host-wake are not system wake sources. True mem still needs MCU.
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
#   INFO1 0x05026020  [7:0] LED mode (Linux); [31:16] heartbeat (Linux)
#   INFO2 0x05026024  run_ms free-running (MCU; +phase_ms each timer fire)
#   INFO3 0x05026028  [7:0] applied mode echo; [31:8] blink loop count
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
