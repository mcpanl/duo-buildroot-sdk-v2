#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import mmap
import struct
import subprocess
import time

# =========================
# 屏幕参数
# =========================

WIDTH = 172
HEIGHT = 320
FBDEV = "/dev/fb0"

# =========================
# RGB565颜色
# =========================

BLACK = 0x0000
WHITE = 0xFFFF
GREEN = 0x07E0
RED = 0xF800
YELLOW = 0xFFE0
CYAN = 0x07FF
ORANGE = 0xFD20
GRAY = 0x8410

# =========================
# 简易8x8字体
# 每个字符8行，每行8bit
# =========================

FONT = {
    "0": [0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x3C, 0x00],
    "1": [0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00],
    "2": [0x3C, 0x66, 0x06, 0x0C, 0x30, 0x60, 0x7E, 0x00],
    "3": [0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00],
    "4": [0x0C, 0x1C, 0x3C, 0x6C, 0x7E, 0x0C, 0x0C, 0x00],
    "5": [0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00],
    "6": [0x1C, 0x30, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00],
    "7": [0x7E, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00],
    "8": [0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00],
    "9": [0x3C, 0x66, 0x66, 0x3E, 0x06, 0x0C, 0x38, 0x00],
    ".": [0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00],
    "%": [0x62, 0x64, 0x08, 0x10, 0x26, 0x46, 0x00, 0x00],
    "V": [0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00],
    "N": [0x66, 0x76, 0x7E, 0x6E, 0x66, 0x66, 0x66, 0x00],
    "o": [0x00, 0x00, 0x3C, 0x66, 0x66, 0x66, 0x3C, 0x00],
    "B": [0x7C, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x7C, 0x00],
    "A": [0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x00],
    "T": [0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00],
    "C": [0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00],
    "H": [0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00],
    "G": [0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x3C, 0x00],
    "F": [0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x00],
    "U": [0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00],
    "L": [0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00],
    "D": [0x78, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0x78, 0x00],
    "I": [0x3C, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3C, 0x00],
    "S": [0x3C, 0x66, 0x60, 0x3C, 0x06, 0x66, 0x3C, 0x00],
    "R": [0x7C, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0x66, 0x00],
    "E": [0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00],
    "P": [0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00],
    "W": [0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00],
    "Y": [0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x00],
    "-": [0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00],
}

# =========================
# framebuffer
# =========================

class FrameBuffer:
    def __init__(self):
        self.fd = os.open(FBDEV, os.O_RDWR)
        self.size = WIDTH * HEIGHT * 2
        self.fb = mmap.mmap(
            self.fd,
            self.size,
            mmap.MAP_SHARED,
            mmap.PROT_WRITE | mmap.PROT_READ,
        )

    def clear(self, color=BLACK):
        pixel = struct.pack("<H", color)
        self.fb.seek(0)
        self.fb.write(pixel * (WIDTH * HEIGHT))

    def pixel(self, x, y, color):
        if x < 0 or y < 0 or x >= WIDTH or y >= HEIGHT:
            return
        # 屏幕坐标系翻转
        y = HEIGHT - 1 - y
        pos = (y * WIDTH + x) * 2
        self.fb.seek(pos)
        self.fb.write(struct.pack("<H", color))

    def text(self, x, y, text, color=WHITE, scale=2):
        for ch in text:
            if ch == " ":
                x += 8 * scale
                continue

            bitmap = FONT.get(ch)
            if not bitmap:
                x += 8 * scale
                continue

            for row in range(8):
                bits = bitmap[row]
                for col in range(8):
                    if bits & (1 << (7 - col)):
                        for sy in range(scale):
                            for sx in range(scale):
                                self.pixel(
                                    x + col * scale + sx,
                                    y + row * scale + sy,
                                    color,
                                )
            x += 8 * scale

    def text_center(self, y, text, color=WHITE, scale=2):
        width = len(text) * 8 * scale
        x = max(0, (WIDTH - width) // 2)
        self.text(x, y, text, color, scale)

    def close(self):
        self.fb.close()
        os.close(self.fd)

# =========================
# 获取电池信息
# =========================

def _parse_int_field(line, key):
    """从 'key: 123 ...' 中提取整数。"""
    if not line.startswith(key):
        return None
    parts = line.split()
    # 形如: capacity: 98 %  /  voltage_now: 4183000 uV ...
    for part in parts[1:]:
        try:
            return int(part)
        except ValueError:
            continue
    return None

def get_battery():
    """
    返回 dict:
      present, status, charging, external_power,
      voltage, capacity, charge_current
    失败时返回 None
    """
    try:
        out = subprocess.check_output(
            ["axp2101ctl", "status"],
            stderr=subprocess.DEVNULL,
        ).decode(errors="ignore")
    except Exception:
        return None

    info = {
        "present": False,
        "status": "Unknown",
        "charging": False,
        "external_power": False,
        "voltage": None,
        "capacity": None,
        "charge_current": None,
    }

    for raw in out.splitlines():
        line = raw.strip()
        if not line:
            continue

        low = line.lower()

        if low.startswith("status_code:"):
            continue

        if low.startswith("status:"):
            # status: Charging / Discharging / Full / Not charging ...
            status = line.split(":", 1)[1].strip()
            if status:
                info["status"] = status

        elif low.startswith("external_power:"):
            info["external_power"] = "yes" in low

        elif low.startswith("battery_present:"):
            info["present"] = "yes" in low

        elif low.startswith("voltage_now"):
            uv = _parse_int_field(line, "voltage_now")
            if uv is not None:
                info["voltage"] = uv / 1000000.0

        elif low.startswith("capacity"):
            # 修复: capacity: 98 %  -> 取第一个整数 98
            cap = _parse_int_field(line, "capacity")
            if cap is not None:
                info["capacity"] = max(0, min(100, cap))

        elif low.startswith("charge_current"):
            ua = _parse_int_field(line, "charge_current")
            if ua is not None:
                info["charge_current"] = ua

    if not info["present"]:
        return None

    if info["voltage"] is None:
        info["voltage"] = 0.0
    if info["capacity"] is None:
        info["capacity"] = 0

    info["charging"] = info["status"].lower() == "charging"
    return info

def status_label(info):
    st = info["status"].lower()
    if st == "charging":
        return "CHG"
    if st == "full":
        return "FULL"
    if st in ("discharging", "not charging"):
        return "BAT"
    if info["external_power"]:
        return "PWR"
    return "BAT"

def capacity_color(capacity, charging, status):
    st = status.lower()
    if st == "charging":
        return CYAN
    if st == "full":
        return GREEN
    if capacity <= 15:
        return RED
    if capacity <= 30:
        return ORANGE
    if capacity <= 60:
        return YELLOW
    return GREEN

def voltage_color(charging, status):
    st = status.lower()
    if st == "charging":
        return CYAN
    if st == "full":
        return GREEN
    return WHITE

# =========================
# 主循环
# =========================

def main():
    fb = FrameBuffer()
    try:
        while True:
            fb.clear(BLACK)
            info = get_battery()

            if info is None:
                fb.text_center(140, "No BAT", RED, 2)
            else:
                volt_text = "%.2fV" % info["voltage"]
                cap_text = "%d%%" % info["capacity"]
                label = status_label(info)

                vcolor = voltage_color(info["charging"], info["status"])
                ccolor = capacity_color(
                    info["capacity"], info["charging"], info["status"]
                )
                # 状态标签颜色
                if info["status"].lower() == "charging":
                    lcolor = CYAN
                elif info["status"].lower() == "full":
                    lcolor = GREEN
                elif info["capacity"] <= 15:
                    lcolor = RED
                else:
                    lcolor = GRAY

                fb.text_center(70, label, lcolor, 2)
                fb.text_center(120, volt_text, vcolor, 3)
                fb.text_center(190, cap_text, ccolor, 3)

            time.sleep(5)
    finally:
        fb.close()

if __name__ == "__main__":
    main()
