#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import mmap
import select
import struct
import subprocess
import time

# =========================
# 屏幕参数（物理分辨率，竖屏）
# =========================

WIDTH = 172
HEIGHT = 320
FBDEV = "/dev/fb0"
TOUCH_DEV = "/dev/input/event2"

# =========================
# IMU / 方向检测
# =========================

IIO_DEV = "/sys/bus/iio/devices/iio:device0"
ACCEL_X = IIO_DEV + "/in_accel_x_raw"
ACCEL_Y = IIO_DEV + "/in_accel_y_raw"
ACCEL_Z = IIO_DEV + "/in_accel_z_raw"

# 约 1g ≈ 2100；平放时 |z| 主导且接近 1g
FLAT_AXIS_MIN = 1500
# 倾斜方向切换时，新轴需明显大于另一轴，避免抖动
ORIENT_HYSTERESIS = 400
# 连续确认次数后才切换方向
ORIENT_CONFIRM = 2

# 方向：相对“充电口朝下竖屏”的顺时针旋转角度
# 0=竖屏充电口朝下, 90=横屏充电口朝右, 180=竖屏充电口朝上, 270=横屏充电口朝左
ORIENT_0 = 0
ORIENT_90 = 90
ORIENT_180 = 180
ORIENT_270 = 270

# =========================
# 触摸 / 页面
# =========================

# Linux input
EV_SYN = 0x00
EV_ABS = 0x03
ABS_MT_POSITION_X = 0x35
ABS_MT_POSITION_Y = 0x36
ABS_MT_TRACKING_ID = 0x39

# 水平滑动阈值（触摸原始坐标像素）
SWIPE_MIN_DIST = 40
# 水平位移需明显大于垂直，才判定为左右滑
SWIPE_AXIS_RATIO = 1.2

PAGE_BATTERY = 0
PAGE_STATUS = 1
PAGE_WIFI = 2
PAGE_COUNT = 3

WPA_CONF = "/etc/wpa_supplicant.conf"
WIFI_IFACE = "wlan0"

# 点击判定：位移小于该值视为点击（非滑动）
TAP_MAX_DIST = 18

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
    "O": [0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00],
    "M": [0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x63, 0x00],
    "K": [0x66, 0x6C, 0x78, 0x70, 0x78, 0x6C, 0x66, 0x00],
    "X": [0x66, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x66, 0x00],
    "Z": [0x7E, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x7E, 0x00],
    "J": [0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x6C, 0x38, 0x00],
    "Q": [0x3C, 0x66, 0x66, 0x66, 0x6E, 0x3C, 0x0E, 0x00],
    "_": [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7E, 0x00],
    "/": [0x02, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00],
    ":": [0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00],
    "-": [0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00],
}

# =========================
# IMU 读取与方向判断
# =========================

def read_accel_axis(path):
    try:
        with open(path, "r") as f:
            return int(f.read().strip())
    except (OSError, ValueError):
        return None

def read_accel():
    """返回 (ax, ay, az)，失败返回 None。"""
    ax = read_accel_axis(ACCEL_X)
    ay = read_accel_axis(ACCEL_Y)
    az = read_accel_axis(ACCEL_Z)
    if ax is None or ay is None or az is None:
        return None
    return ax, ay, az

def classify_orientation(ax, ay, az):
    """
    根据加速度计原始值判断设备朝向。
    实测（约 1g ≈ 2100）:
      竖屏充电口朝下: y≈-2100
      横屏充电口朝右: x≈-2060
      竖屏充电口朝上: y≈+2000
      平放桌面:       z≈+2212  -> 返回 None，由调用方保持上次方向
    """
    ax_a, ay_a, az_a = abs(ax), abs(ay), abs(az)

    # 平放：Z 轴主导且接近 1g
    if az_a >= FLAT_AXIS_MIN and az_a >= ax_a and az_a >= ay_a:
        return None

    # 在 XY 平面内选主导轴
    if ay_a + ORIENT_HYSTERESIS >= ax_a:
        return ORIENT_0 if ay < 0 else ORIENT_180
    return ORIENT_270 if ax < 0 else ORIENT_90

class OrientationTracker:
    """带确认次数的方向跟踪；平放时保持最后一次有效方向。"""

    def __init__(self, initial=ORIENT_0):
        self.orientation = initial
        self._pending = None
        self._count = 0

    def update(self, accel):
        if accel is None:
            return self.orientation

        ax, ay, az = accel
        detected = classify_orientation(ax, ay, az)

        # 平放：保持当前方向
        if detected is None:
            self._pending = None
            self._count = 0
            return self.orientation

        if detected == self.orientation:
            self._pending = None
            self._count = 0
            return self.orientation

        if detected == self._pending:
            self._count += 1
        else:
            self._pending = detected
            self._count = 1

        if self._count >= ORIENT_CONFIRM:
            self.orientation = detected
            self._pending = None
            self._count = 0

        return self.orientation

# =========================
# 触摸输入
# =========================

def touch_to_logical(tx, ty, orientation):
    """
    触摸坐标 -> 面向用户的逻辑坐标。
    触摸报告的是 framebuffer 物理坐标（与 pixel() 写入一致，Y 已翻转）；
    实测触摸 X 与显示左右相反，需再镜像 X。
    """
    # 还原 Y 翻转，并镜像 X，得到与绘制一致的面板坐标
    px, py = WIDTH - 1 - tx, HEIGHT - 1 - ty

    if orientation == ORIENT_0:
        return px, py
    if orientation == ORIENT_90:
        return HEIGHT - 1 - py, px
    if orientation == ORIENT_180:
        return WIDTH - 1 - px, HEIGHT - 1 - py
    # ORIENT_270
    return py, WIDTH - 1 - px

class TouchInput:
    """
    读取 /dev/input/event* 多点触摸，识别左右滑动或点击。
    返回值: ("left",) / ("right",) / ("tap", lx, ly) / None
    """

    # timeval(long,long) + type + code + value；32/64 位 long 均可
    _EVENT_FMT = "llHHi"
    _EVENT_SIZE = struct.calcsize(_EVENT_FMT)

    def __init__(self, path=TOUCH_DEV):
        self.path = path
        self.fd = None
        self._x = None
        self._y = None
        self._start_x = None
        self._start_y = None
        self._tracking = False
        try:
            self.fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        except OSError:
            self.fd = None

    @property
    def available(self):
        return self.fd is not None

    def fileno(self):
        return self.fd

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    @staticmethod
    def _classify(x0, y0, x1, y1):
        if x0 is None or y0 is None or x1 is None or y1 is None:
            return None
        dx = x1 - x0
        dy = y1 - y0
        adx, ady = abs(dx), abs(dy)
        if adx <= TAP_MAX_DIST and ady <= TAP_MAX_DIST:
            return ("tap", x1, y1)
        if adx < SWIPE_MIN_DIST:
            return None
        if adx < ady * SWIPE_AXIS_RATIO:
            return None
        # 手指右移 -> 上一页；左移 -> 下一页
        return ("right",) if dx > 0 else ("left",)

    def poll(self, orientation):
        """读取事件；用逻辑坐标判定左右滑/点击，适配旋转。"""
        if self.fd is None:
            return None

        gesture = None
        while True:
            try:
                data = os.read(self.fd, self._EVENT_SIZE)
            except BlockingIOError:
                break
            except OSError:
                break
            if len(data) < self._EVENT_SIZE:
                break

            _sec, _usec, etype, code, value = struct.unpack(self._EVENT_FMT, data)

            if etype != EV_ABS:
                continue

            if code == ABS_MT_TRACKING_ID:
                if value >= 0:
                    self._tracking = True
                    self._start_x = self._start_y = None
                    self._x = self._y = None
                else:
                    if (
                        self._tracking
                        and self._start_x is not None
                        and self._start_y is not None
                        and self._x is not None
                        and self._y is not None
                    ):
                        lx0, ly0 = touch_to_logical(
                            self._start_x, self._start_y, orientation
                        )
                        lx1, ly1 = touch_to_logical(self._x, self._y, orientation)
                        gesture = self._classify(lx0, ly0, lx1, ly1)
                    self._tracking = False
                    self._start_x = self._start_y = None
                    self._x = self._y = None
            elif code == ABS_MT_POSITION_X:
                self._x = value
                if self._tracking and self._start_x is None:
                    self._start_x = value
            elif code == ABS_MT_POSITION_Y:
                self._y = value
                if self._tracking and self._start_y is None:
                    self._start_y = value

            # 若 X/Y 分开发送，补全起点
            if (
                self._tracking
                and self._start_x is None
                and self._x is not None
                and self._start_y is not None
            ):
                self._start_x = self._x
            if (
                self._tracking
                and self._start_y is None
                and self._y is not None
                and self._start_x is not None
            ):
                self._start_y = self._y

        return gesture

# =========================
# framebuffer（支持逻辑旋转）
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
        self.orientation = ORIENT_0
        self.logical_w = WIDTH
        self.logical_h = HEIGHT

    def set_orientation(self, orientation):
        self.orientation = orientation
        if orientation in (ORIENT_90, ORIENT_270):
            self.logical_w = HEIGHT
            self.logical_h = WIDTH
        else:
            self.logical_w = WIDTH
            self.logical_h = HEIGHT

    def clear(self, color=BLACK):
        pixel = struct.pack("<H", color)
        self.fb.seek(0)
        self.fb.write(pixel * (WIDTH * HEIGHT))

    def _to_physical(self, lx, ly):
        """逻辑坐标（面向用户正立）-> 物理 framebuffer 坐标。"""
        o = self.orientation
        if o == ORIENT_0:
            return lx, ly
        if o == ORIENT_90:
            # 设备顺时针转 90°：用户顶边 = 面板左边
            return ly, HEIGHT - 1 - lx
        if o == ORIENT_180:
            return WIDTH - 1 - lx, HEIGHT - 1 - ly
        # ORIENT_270
        return WIDTH - 1 - ly, lx

    def pixel(self, x, y, color):
        px, py = self._to_physical(x, y)
        if px < 0 or py < 0 or px >= WIDTH or py >= HEIGHT:
            return
        # 屏幕坐标系翻转
        py = HEIGHT - 1 - py
        pos = (py * WIDTH + px) * 2
        self.fb.seek(pos)
        self.fb.write(struct.pack("<H", color))

    def fill_rect(self, x, y, w, h, color):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                self.pixel(xx, yy, color)

    def text(self, x, y, text, color=WHITE, scale=2):
        for ch in text:
            if ch == " ":
                x += 8 * scale
                continue

            bitmap = FONT.get(ch)
            if not bitmap and "a" <= ch <= "z":
                bitmap = FONT.get(ch.upper())
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
        x = max(0, (self.logical_w - width) // 2)
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

def get_wlan0_ip():
    """返回 wlan0 的 IPv4 地址字符串；无地址时返回 None。"""
    try:
        out = subprocess.check_output(
            ["ip", "-4", "-o", "addr", "show", "dev", WIFI_IFACE],
            stderr=subprocess.DEVNULL,
        ).decode(errors="ignore")
    except Exception:
        return None

    for line in out.splitlines():
        parts = line.split()
        # 形如: N: wlan0    inet 192.168.1.2/24 ...
        if "inet" in parts:
            idx = parts.index("inet")
            if idx + 1 < len(parts):
                return parts[idx + 1].split("/")[0]
    return None

def get_mem_info():
    """返回 (used_kb, total_kb)；失败返回 None。"""
    try:
        mem_total = None
        mem_available = None
        with open("/proc/meminfo", "r") as f:
            for line in f:
                if line.startswith("MemTotal:"):
                    mem_total = int(line.split()[1])
                elif line.startswith("MemAvailable:"):
                    mem_available = int(line.split()[1])
                if mem_total is not None and mem_available is not None:
                    break
        if mem_total is None:
            return None
        if mem_available is None:
            # 旧内核无 MemAvailable 时退化为 MemFree
            with open("/proc/meminfo", "r") as f:
                for line in f:
                    if line.startswith("MemFree:"):
                        mem_available = int(line.split()[1])
                        break
        if mem_available is None:
            return None
        used = max(0, mem_total - mem_available)
        return used, mem_total
    except Exception:
        return None

def get_disk_info(path="/"):
    """返回 (used_bytes, total_bytes)；失败返回 None。"""
    try:
        st = os.statvfs(path)
        total = st.f_blocks * st.f_frsize
        free = st.f_bavail * st.f_frsize
        used = max(0, total - free)
        return used, total
    except Exception:
        return None

def format_size(num, unit_base=1024):
    """把字节/KB 格式化为短字符串，如 128M / 1.5G。"""
    if num is None:
        return "- -"
    n = float(num)
    units = ("K", "M", "G", "T")
    # 传入已是 KB 时从 K 开始；字节则先转 KB
    if unit_base == 1:
        n = n / 1024.0
    for u in units:
        if n < 1024.0 or u == units[-1]:
            if n >= 100 or u == "K":
                return "%d%s" % (int(n), u)
            return "%.1f%s" % (n, u)
        n /= 1024.0
    return "%dK" % int(num)

def _parse_conf_ssid(path=WPA_CONF):
    """从 wpa_supplicant.conf 读取配置的 ssid。"""
    try:
        with open(path, "r") as f:
            for line in f:
                s = line.strip()
                if s.startswith("ssid="):
                    val = s.split("=", 1)[1].strip()
                    if val.startswith('"') and val.endswith('"'):
                        val = val[1:-1]
                    return val or None
    except Exception:
        pass
    return None

def get_wifi_associated_ssid():
    """返回当前已关联的 SSID；未关联返回 None。"""
    # iwgetid -r
    try:
        out = subprocess.check_output(
            ["iwgetid", WIFI_IFACE, "-r"],
            stderr=subprocess.DEVNULL,
        ).decode(errors="ignore").strip()
        if out:
            return out
    except Exception:
        pass

    # wpa_cli status
    try:
        out = subprocess.check_output(
            ["wpa_cli", "-i", WIFI_IFACE, "status"],
            stderr=subprocess.DEVNULL,
        ).decode(errors="ignore")
        state = None
        ssid = None
        for line in out.splitlines():
            if line.startswith("wpa_state="):
                state = line.split("=", 1)[1].strip()
            elif line.startswith("ssid="):
                ssid = line.split("=", 1)[1].strip()
        if state == "COMPLETED" and ssid:
            return ssid
    except Exception:
        pass

    # iw dev link
    try:
        out = subprocess.check_output(
            ["iw", "dev", WIFI_IFACE, "link"],
            stderr=subprocess.DEVNULL,
        ).decode(errors="ignore")
        for line in out.splitlines():
            s = line.strip()
            if s.startswith("SSID:"):
                ssid = s.split(":", 1)[1].strip()
                if ssid:
                    return ssid
    except Exception:
        pass
    return None

def get_wifi_status():
    """
    返回 dict:
      connected: bool
      ssid: 显示用 SSID（已连用关联名，未连用配置名）
      busy: 正在连接中
    """
    assoc = get_wifi_associated_ssid()
    conf = _parse_conf_ssid()
    return {
        "connected": assoc is not None,
        "ssid": assoc if assoc else (conf if conf else None),
    }

def _run_quiet(cmd):
    try:
        subprocess.call(
            cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except Exception:
        pass

def wifi_open():
    """等效: wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf"""
    _run_quiet(["killall", "wpa_supplicant"])
    time.sleep(0.3)
    try:
        ret = subprocess.call(
            ["wpa_supplicant", "-B", "-i", WIFI_IFACE, "-c", WPA_CONF],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return ret == 0
    except Exception:
        return False

def wifi_close():
    """断开 WiFi：结束 wpa_supplicant。"""
    _run_quiet(["killall", "wpa_supplicant"])
    return True

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

def layout_positions(logical_h):
    """按逻辑高度分配竖/横屏文字位置。"""
    # 竖屏 320 / 横屏 172，按比例大致居中排布
    return (
        logical_h * 22 // 100,  # label
        logical_h * 38 // 100,  # voltage
        logical_h * 60 // 100,  # capacity
    )

def fit_scale(text, logical_w, prefer=2, margin=4):
    scale = prefer
    while scale > 1 and len(text) * 8 * scale > logical_w - margin:
        scale -= 1
    return scale

def draw_page_dots(fb, page):
    """底部页面指示点。"""
    n = PAGE_COUNT
    r = 3
    gap = 12
    total_w = n * (r * 2) + (n - 1) * gap
    x0 = (fb.logical_w - total_w) // 2
    y = fb.logical_h - 14
    for i in range(n):
        x = x0 + i * (r * 2 + gap)
        color = WHITE if i == page else GRAY
        fb.fill_rect(x, y, r * 2, r * 2, color)

def draw_battery(fb, info):
    y_label, y_volt, y_cap = layout_positions(fb.logical_h)
    # 横屏高度较矮，略缩小字号
    scale_label = 2
    scale_value = 2 if fb.logical_h < 200 else 3

    if info is None:
        fb.text_center(fb.logical_h // 2 - 8, "No BAT", RED, scale_label)
        return

    volt_text = "%.2fV" % info["voltage"]
    cap_text = "%d%%" % info["capacity"]
    label = status_label(info)

    vcolor = voltage_color(info["charging"], info["status"])
    ccolor = capacity_color(
        info["capacity"], info["charging"], info["status"]
    )
    if info["status"].lower() == "charging":
        lcolor = CYAN
    elif info["status"].lower() == "full":
        lcolor = GREEN
    elif info["capacity"] <= 15:
        lcolor = RED
    else:
        lcolor = GRAY

    fb.text_center(y_label, label, lcolor, scale_label)
    fb.text_center(y_volt, volt_text, vcolor, scale_value)
    fb.text_center(y_cap, cap_text, ccolor, scale_value)

def draw_status(fb, ip, mem, disk):
    """第二屏：STATUS — wlan0 IP / 可用内存 / 可用硬盘。"""
    h = fb.logical_h
    w = fb.logical_w
    compact = h < 200

    # mem/disk: (used, total) -> 显示可用量
    mem_free = (mem[1] - mem[0]) if mem else None
    disk_free = (disk[1] - disk[0]) if disk else None
    mem_text = format_size(mem_free, 1024) if mem_free is not None else "- -"
    disk_text = format_size(disk_free, 1) if disk_free is not None else "- -"
    ip_text = ip if ip else "- -"

    if compact:
        rows = [
            (h * 6 // 100, "STATUS", GRAY, 2),
            (h * 28 // 100, ip_text, WHITE if ip else GRAY, 2),
            (h * 52 // 100, "M " + mem_text, WHITE if mem else GRAY, 2),
            (h * 72 // 100, "D " + disk_text, WHITE if disk else GRAY, 2),
        ]
    else:
        rows = [
            (h * 8 // 100, "STATUS", GRAY, 2),
            (h * 24 // 100, "IP", GRAY, 1),
            (h * 32 // 100, ip_text, WHITE if ip else GRAY, 2),
            (h * 48 // 100, "MEM", GRAY, 1),
            (h * 56 // 100, mem_text, WHITE if mem else GRAY, 2),
            (h * 72 // 100, "DISK", GRAY, 1),
            (h * 80 // 100, disk_text, WHITE if disk else GRAY, 2),
        ]

    for y, text, color, prefer in rows:
        scale = fit_scale(text, w, prefer)
        fb.text_center(y, text, color, scale)

# WiFi 按钮在逻辑坐标系中的矩形（每次绘制时更新）
_wifi_btn_rect = None

def wifi_button_rect(fb):
    """计算 Open/Close 按钮区域。"""
    bw = min(fb.logical_w - 20, 120)
    bh = 28 if fb.logical_h < 200 else 36
    bx = (fb.logical_w - bw) // 2
    by = fb.logical_h * 62 // 100
    return bx, by, bw, bh

def point_in_rect(x, y, rect):
    if rect is None:
        return False
    bx, by, bw, bh = rect
    return bx <= x < bx + bw and by <= y < by + bh

def draw_wifi(fb, wifi, busy=False):
    """第三屏：SSID + Open/Close 按钮。"""
    global _wifi_btn_rect
    h = fb.logical_h
    w = fb.logical_w

    fb.text_center(h * 10 // 100, "WIFI", GRAY, 2)

    ssid = wifi["ssid"] if wifi and wifi.get("ssid") else "- -"
    # 过长则截断
    max_chars = max(4, (w - 8) // 8)
    if len(ssid) > max_chars:
        ssid = ssid[: max_chars - 1] + "-"
    scale = fit_scale(ssid, w, 2)
    color = GREEN if (wifi and wifi.get("connected")) else WHITE
    if ssid == "- -":
        color = GRAY
    fb.text_center(h * 32 // 100, ssid, color, scale)

    bx, by, bw, bh = wifi_button_rect(fb)
    _wifi_btn_rect = (bx, by, bw, bh)

    if busy:
        label = "WAIT"
        btn_color = ORANGE
        txt_color = BLACK
    elif wifi and wifi.get("connected"):
        label = "CLOSE"
        btn_color = RED
        txt_color = WHITE
    else:
        label = "OPEN"
        btn_color = GREEN
        txt_color = BLACK

    fb.fill_rect(bx, by, bw, bh, btn_color)
    # 边框
    fb.fill_rect(bx, by, bw, 2, WHITE)
    fb.fill_rect(bx, by + bh - 2, bw, 2, WHITE)
    fb.fill_rect(bx, by, 2, bh, WHITE)
    fb.fill_rect(bx + bw - 2, by, 2, bh, WHITE)

    tw = len(label) * 8 * 2
    tx = bx + max(0, (bw - tw) // 2)
    ty = by + max(0, (bh - 16) // 2)
    fb.text(tx, ty, label, txt_color, 2)

def draw_page(fb, page, battery, ip, mem, disk, wifi, wifi_busy=False):
    fb.clear(BLACK)
    if page == PAGE_BATTERY:
        draw_battery(fb, battery)
    elif page == PAGE_STATUS:
        draw_status(fb, ip, mem, disk)
    else:
        draw_wifi(fb, wifi, wifi_busy)
    draw_page_dots(fb, page)

# =========================
# 主循环
# =========================

def main():
    fb = FrameBuffer()
    tracker = OrientationTracker(ORIENT_0)
    touch = TouchInput(TOUCH_DEV)

    poll_s = 0.05
    battery_interval_s = 5.0
    status_interval_s = 5.0
    wifi_interval_s = 2.0
    last_battery_t = 0.0
    last_status_t = 0.0
    last_wifi_t = 0.0
    info = None
    ip = None
    mem = None
    disk = None
    wifi = {"connected": False, "ssid": None}
    wifi_busy = False
    wifi_busy_until = 0.0
    last_orient = None
    page = PAGE_BATTERY

    try:
        while True:
            now = time.time()
            need_redraw = False

            if wifi_busy and now >= wifi_busy_until:
                wifi_busy = False
                need_redraw = True
                # 操作结束后立刻刷新 WiFi 状态
                last_wifi_t = 0.0

            # 触摸优先：短超时等待，保证滑动响应
            if touch.available:
                try:
                    select.select([touch], [], [], poll_s)
                except (OSError, ValueError):
                    time.sleep(poll_s)
                gesture = touch.poll(
                    last_orient if last_orient is not None else ORIENT_0
                )
                if gesture is not None:
                    kind = gesture[0]
                    if kind == "left":
                        page = (page + 1) % PAGE_COUNT
                        need_redraw = True
                    elif kind == "right":
                        page = (page - 1) % PAGE_COUNT
                        need_redraw = True
                    elif kind == "tap" and page == PAGE_WIFI and not wifi_busy:
                        _, tx, ty = gesture
                        if point_in_rect(tx, ty, _wifi_btn_rect):
                            if wifi.get("connected"):
                                wifi_close()
                            else:
                                wifi_open()
                            wifi_busy = True
                            wifi_busy_until = now + 3.0
                            need_redraw = True
            else:
                time.sleep(poll_s)

            orient = tracker.update(read_accel())
            if orient != last_orient:
                fb.set_orientation(orient)
                need_redraw = True

            if info is None or (now - last_battery_t) >= battery_interval_s:
                new_info = get_battery()
                last_battery_t = now
                if new_info != info:
                    info = new_info
                    if page == PAGE_BATTERY:
                        need_redraw = True
                elif info is None:
                    info = new_info
                    need_redraw = True

            if (now - last_status_t) >= status_interval_s or last_status_t == 0.0:
                new_ip = get_wlan0_ip()
                new_mem = get_mem_info()
                new_disk = get_disk_info("/")
                last_status_t = now
                changed = (
                    new_ip != ip
                    or new_mem != mem
                    or new_disk != disk
                )
                ip, mem, disk = new_ip, new_mem, new_disk
                if changed and page == PAGE_STATUS:
                    need_redraw = True

            if (now - last_wifi_t) >= wifi_interval_s or last_wifi_t == 0.0:
                new_wifi = get_wifi_status()
                last_wifi_t = now
                if new_wifi != wifi:
                    wifi = new_wifi
                    if page == PAGE_WIFI:
                        need_redraw = True

            if need_redraw:
                draw_page(fb, page, info, ip, mem, disk, wifi, wifi_busy)
                last_orient = orient
    finally:
        touch.close()
        fb.close()

if __name__ == "__main__":
    main()
