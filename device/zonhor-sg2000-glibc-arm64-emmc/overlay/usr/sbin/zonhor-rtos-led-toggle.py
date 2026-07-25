#!/usr/bin/env python3
"""
Intermittently toggle Zonhor RTOS status LED (GPIOA18 / rtos-led).

After Linux handoff the pin is owned by gpio-leds. This script drives
on/off via sysfs so you can leave it running, enter mem, wake, and see
whether blink/trigger recovers.

Usage:
  # foreground
  zonhor-rtos-led-toggle.py
  zonhor-rtos-led-toggle.py --on-ms 500 --off-ms 500

  # background
  nohup zonhor-rtos-led-toggle.py >/tmp/rtos-led-toggle.log 2>&1 &
  echo $! >/tmp/rtos-led-toggle.pid

  # stop
  kill $(cat /tmp/rtos-led-toggle.pid)

  # after wake, restore default blink manually if needed:
  #   zonhor-ledctl rtos blink
  #   # or: echo timer > /sys/class/leds/rtos-led/trigger
"""

from __future__ import annotations

import argparse
import os
import signal
import sys
import time

RTOS_LED = "/sys/class/leds/rtos-led"
BRIGHTNESS = f"{RTOS_LED}/brightness"
TRIGGER = f"{RTOS_LED}/trigger"

_running = True


def _log(msg: str) -> None:
    ts = time.strftime("%H:%M:%S")
    print(f"[{ts}] {msg}", flush=True)


def _write(path: str, value: str) -> bool:
    try:
        with open(path, "w", encoding="ascii") as f:
            f.write(value)
        return True
    except OSError as e:
        _log(f"write {path}={value!r} failed: {e}")
        return False


def _read(path: str) -> str:
    try:
        with open(path, "r", encoding="ascii") as f:
            return f.read().strip()
    except OSError:
        return "?"


def set_level(on: bool) -> bool:
    """Force solid on/off (clears timer/activity trigger)."""
    if not _write(TRIGGER, "none"):
        return False
    return _write(BRIGHTNESS, "1" if on else "0")


def on_signal(signum, _frame) -> None:
    global _running
    _running = False
    _log(f"signal {signum}, stopping")


def main() -> int:
    ap = argparse.ArgumentParser(description="Toggle rtos-led on/off in a loop")
    ap.add_argument("--on-ms", type=int, default=800, help="ON duration ms")
    ap.add_argument("--off-ms", type=int, default=800, help="OFF duration ms")
    ap.add_argument(
        "--restore-blink",
        action="store_true",
        help="on exit, set trigger=timer (300/700) instead of leaving last level",
    )
    args = ap.parse_args()

    if not os.path.isdir(RTOS_LED):
        _log(f"missing {RTOS_LED}")
        return 1

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    _log(
        f"start toggle rtos-led on={args.on_ms}ms off={args.off_ms}ms "
        f"pid={os.getpid()} (Ctrl-C / SIGTERM to stop)"
    )
    _log(f"initial trigger={_read(TRIGGER)!r} brightness={_read(BRIGHTNESS)!r}")

    on = True
    while _running:
        ok = set_level(on)
        level = "ON" if on else "OFF"
        if ok:
            _log(f"{level}  brightness={_read(BRIGHTNESS)}")
        else:
            # Likely mid-suspend; wait and retry without flipping.
            time.sleep(0.5)
            continue

        ms = args.on_ms if on else args.off_ms
        deadline = time.monotonic() + ms / 1000.0
        while _running and time.monotonic() < deadline:
            time.sleep(0.05)
        on = not on

    if args.restore_blink:
        _write(TRIGGER, "timer")
        for name, val in (("delay_on", "300"), ("delay_off", "700")):
            p = f"{RTOS_LED}/{name}"
            if os.path.exists(p):
                _write(p, val)
        _log("restored trigger=timer 300/700")
    else:
        _log(f"exit leaving brightness={_read(BRIGHTNESS)} trigger={_read(TRIGGER)!r}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
