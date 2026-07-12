#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Chunked partition flash with OTA UI progress updates and console/kmsg speed logs.
"""

from __future__ import print_function

import argparse
import binascii
import fcntl
import os
import struct
import subprocess
import sys
import time

CHUNK = 4 * 1024 * 1024  # 4 MiB
UI_BIN = "/usr/sbin/zonhor-ota-ui.py"
STATUS_PATH = "/run/zonhor-ota.status"
# Print progress to console/kmsg at least this often during slow eMMC writes.
LOG_INTERVAL_S = 1.0
CIMG_MAGIC = b"CIMG"
CIMG_HEADER_SIZE = 64
CIMG_CHUNK_HEADER_SIZE = 64
CIMG_TYPE_DONT_CARE = 0
CIMG_TYPE_CRC_CHECK = 1
BLKDISCARD = 0x1277
BLKZEROOUT = 0x127F
ZERO_BUF = b"\0" * (1024 * 1024)
_ZERO_CRC_CACHE = {}


def human_mib(n):
    return n / (1024.0 * 1024.0)


def fmt_rate(bps):
    if bps is None or bps < 0:
        return "--.- MB/s"
    mb = bps / (1024.0 * 1024.0)
    if mb >= 100:
        return "%.0f MB/s" % mb
    return "%.1f MB/s" % mb


def console_log(msg, log_path=None):
    """Print to serial console and inject into kernel log (dmesg)."""
    line = "zonhor-ota: %s" % msg
    try:
        print(line, flush=True)
    except Exception:
        pass
    try:
        with open("/dev/kmsg", "w") as kmsg:
            # <6> = KERN_INFO
            kmsg.write("<6>%s\n" % line)
    except Exception:
        pass
    if log_path:
        try:
            with open(log_path, "a") as lf:
                lf.write("%s %s\n" % (time.strftime("%Y-%m-%d %H:%M:%S"), line))
        except Exception:
            pass


def ui_update(status_path, stage, pct, msg, eta_s, active, target):
    if not os.path.isfile(UI_BIN):
        return
    cmd = [
        UI_BIN,
        "--status",
        status_path,
        "update",
        "--stage",
        stage,
        "--pct",
        str(int(pct)),
        "--eta-s",
        str(int(eta_s)),
    ]
    if msg:
        cmd.extend(["--msg", msg])
    if active:
        cmd.extend(["--active", active])
    if target:
        cmd.extend(["--target", target])
    try:
        subprocess.call(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError:
        pass


def estimate_eta(t0, pct):
    if t0 <= 0 or pct < 5:
        return -1
    elapsed = time.time() - t0
    if elapsed < 0.5:
        return -1
    return int(max(0, elapsed * (100.0 - pct) / float(pct)))


def flash_eta(written, total, avg_bps):
    if avg_bps is None or avg_bps <= 0 or written >= total:
        return -1
    return int(max(0, (total - written) / avg_bps))


def is_cimg_file(src):
    try:
        with open(src, "rb") as f:
            return f.read(4) == CIMG_MAGIC
    except OSError:
        return False


def zero_crc(size):
    cached = _ZERO_CRC_CACHE.get(size)
    if cached is not None:
        return cached
    crc = 0
    remaining = size
    while remaining:
        n = min(remaining, len(ZERO_BUF))
        crc = binascii.crc32(ZERO_BUF[:n], crc)
        remaining -= n
    crc &= 0xFFFFFFFF
    _ZERO_CRC_CACHE[size] = crc
    return crc


def parent_disk_for(dev):
    base = os.path.basename(os.path.realpath(dev))
    if base.startswith("mmcblk") and "p" in base:
        return base.rsplit("p", 1)[0]
    for suffix in "0123456789":
        if base.endswith(suffix):
            base = base[:-1]
        else:
            break
    return base or None


def discard_zeroes_data(dev):
    disk = parent_disk_for(dev)
    if not disk:
        return False
    sysfs = "/sys/block/%s/queue/discard_zeroes_data" % disk
    try:
        with open(sysfs, "r") as f:
            return f.read().strip() == "1"
    except OSError:
        return False


def block_zeroout(fd, offset, length):
    if length <= 0:
        return False
    try:
        fcntl.ioctl(fd, BLKZEROOUT, struct.pack("QQ", offset, length))
        return True
    except OSError:
        return False


def block_discard(fd, offset, length):
    if length <= 0:
        return False
    try:
        fcntl.ioctl(fd, BLKDISCARD, struct.pack("QQ", offset, length))
        return True
    except OSError:
        return False


def write_zeroes(outf, offset, length):
    outf.seek(offset)
    remaining = length
    while remaining:
        n = min(remaining, len(ZERO_BUF))
        outf.write(ZERO_BUF[:n])
        remaining -= n


def zero_range(outf, dst, offset, length):
    if length <= 0:
        return "skip"
    fd = outf.fileno()
    if block_zeroout(fd, offset, length):
        return "zeroout"
    if discard_zeroes_data(dst) and block_discard(fd, offset, length):
        return "discard"
    write_zeroes(outf, offset, length)
    return "write-zero"


def scan_cimg(src):
    chunks = []
    with open(src, "rb") as f:
        hdr = f.read(CIMG_HEADER_SIZE)
        if len(hdr) != CIMG_HEADER_SIZE or hdr[:4] != CIMG_MAGIC:
            raise ValueError("%s is not a CIMG" % src)
        version, chunk_hdr_sz, total_chunks, file_sz = struct.unpack_from(
            "<IIII", hdr, 4
        )
        if chunk_hdr_sz <= 0:
            chunk_hdr_sz = CIMG_CHUNK_HEADER_SIZE
        if total_chunks <= 0:
            raise ValueError("CIMG total_chunks=%u invalid" % total_chunks)

        logical = 0
        payload = 0
        sparse = 0
        for idx in range(total_chunks):
            ch = f.read(chunk_hdr_sz)
            if len(ch) != chunk_hdr_sz:
                raise ValueError("truncated CIMG chunk header %u" % (idx + 1))
            ctype, data_sz, _prog_off, part_sz, crc = struct.unpack_from(
                "<IIIII", ch, 0
            )
            data_off = f.tell()
            if ctype == CIMG_TYPE_DONT_CARE:
                skip_len = part_sz or data_sz
                sparse += skip_len
                chunks.append((ctype, data_off, data_sz, skip_len, crc, logical))
                logical += skip_len
            elif ctype == CIMG_TYPE_CRC_CHECK:
                if data_sz <= 0:
                    raise ValueError("CIMG chunk %u has zero data size" % (idx + 1))
                chunks.append((ctype, data_off, data_sz, data_sz, crc, logical))
                logical += data_sz
                payload += data_sz
            else:
                raise ValueError("unsupported CIMG chunk type %u" % ctype)
            f.seek(data_sz, os.SEEK_CUR)

    return {
        "version": version,
        "chunk_hdr_sz": chunk_hdr_sz,
        "total_chunks": total_chunks,
        "file_sz": file_sz,
        "logical": logical,
        "payload": payload,
        "sparse": sparse,
        "chunks": chunks,
    }


def flash_cimg_image(src, dst, stage, pct_lo, pct_hi, status_path, t0, active, target, log_path):
    info = scan_cimg(src)
    total = info["logical"]
    if total <= 0:
        raise SystemExit("empty CIMG image: %s" % src)

    console_log(
        "%s CIMG start: %s -> %s (logical=%.1f MiB, payload=%.1f MiB, chunks=%u)"
        % (
            stage,
            src,
            dst,
            human_mib(total),
            human_mib(info["payload"]),
            info["total_chunks"],
        ),
        log_path,
    )

    done = 0
    written_payload = 0
    zeroed = 0
    last_pct = -1
    last_ui = 0.0
    last_log = 0.0
    last_done = 0
    t_flash0 = time.time()

    with open(src, "rb") as inf, open(dst, "r+b") as outf:
        for ctype, data_off, data_sz, skip_len, crc, logical_off in info["chunks"]:
            if ctype == CIMG_TYPE_DONT_CARE:
                zero_range(outf, dst, logical_off, skip_len)
                done += skip_len
                zeroed += skip_len
            elif crc == zero_crc(data_sz):
                zero_range(outf, dst, logical_off, data_sz)
                done += data_sz
                zeroed += data_sz
            else:
                inf.seek(data_off)
                outf.seek(logical_off)
                remaining = data_sz
                while remaining:
                    n = min(remaining, CHUNK)
                    buf = inf.read(n)
                    if len(buf) != n:
                        raise SystemExit("truncated CIMG payload at %d" % data_off)
                    outf.write(buf)
                    remaining -= n
                    done += n
                    written_payload += n

            now = time.time()
            frac = float(done) / float(total)
            pct = min(pct_hi, int(pct_lo + (pct_hi - pct_lo) * frac))
            elapsed = now - t_flash0
            avg_bps = (done / elapsed) if elapsed > 0.05 else None
            win = now - last_log if last_log > 0 else elapsed
            delta = done - last_done
            cur_bps = (delta / win) if win >= 0.2 and delta > 0 else avg_bps

            do_ui = pct != last_pct or (now - last_ui) >= 0.5
            do_log = (now - last_log) >= LOG_INTERVAL_S or done == total
            if do_ui:
                eta = flash_eta(done, total, avg_bps)
                if eta < 0:
                    eta = estimate_eta(t0, max(pct, 1))
                ui_update(
                    status_path,
                    stage,
                    pct,
                    "%dM/%dM %s"
                    % (int(human_mib(done)), int(human_mib(total)), fmt_rate(avg_bps)),
                    eta,
                    active,
                    target,
                )
                last_pct = pct
                last_ui = now
            if do_log:
                eta = flash_eta(done, total, avg_bps)
                eta_s = ("%ds" % eta) if eta >= 0 else "--"
                console_log(
                    "%s %6.1f/%6.1f MiB (%3d%%) cur=%s avg=%s eta=%s zero=%.1fMiB"
                    % (
                        stage,
                        human_mib(done),
                        human_mib(total),
                        int(100.0 * frac),
                        fmt_rate(cur_bps),
                        fmt_rate(avg_bps),
                        eta_s,
                        human_mib(zeroed),
                    ),
                    log_path,
                )
                last_log = now
                last_done = done

        t_sync0 = time.time()
        outf.flush()
        try:
            os.fsync(outf.fileno())
        except OSError:
            pass
        fsync_s = time.time() - t_sync0

    t_flush0 = time.time()
    try:
        parent = os.path.realpath(dst)
        if "mmcblk" in parent and "p" in os.path.basename(parent):
            disk = parent.rsplit("p", 1)[0]
            if os.path.exists(disk):
                console_log("%s blockdev --flushbufs %s" % (stage, disk), log_path)
                subprocess.call(
                    ["blockdev", "--flushbufs", disk],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
    except OSError:
        pass
    flush_s = time.time() - t_flush0

    elapsed = time.time() - t_flash0
    avg_bps = (done / elapsed) if elapsed > 0 else 0
    console_log(
        "%s CIMG done: logical=%.1f MiB payload_written=%.1f MiB zero=%.1f MiB "
        "in %.1fs (avg=%s, fsync=%.2fs, flush=%.2fs)"
        % (
            stage,
            human_mib(done),
            human_mib(written_payload),
            human_mib(zeroed),
            elapsed,
            fmt_rate(avg_bps),
            fsync_s,
            flush_s,
        ),
        log_path,
    )
    ui_update(
        status_path,
        stage,
        pct_hi,
        "done %s" % fmt_rate(avg_bps),
        estimate_eta(t0, pct_hi),
        active,
        target,
    )


def flash_image(src, dst, stage, pct_lo, pct_hi, status_path, t0, active, target, log_path):
    if is_cimg_file(src):
        return flash_cimg_image(
            src, dst, stage, pct_lo, pct_hi, status_path, t0, active, target, log_path
        )

    total = os.path.getsize(src)
    if total <= 0:
        raise SystemExit("empty image: %s" % src)

    console_log(
        "%s start: %s -> %s (%.1f MiB, chunk=%d MiB)"
        % (stage, src, dst, human_mib(total), CHUNK // (1024 * 1024)),
        log_path,
    )

    written = 0
    last_pct = -1
    last_ui = 0.0
    last_log = 0.0
    last_log_bytes = 0
    t_flash0 = time.time()
    fsync_s = 0.0

    with open(src, "rb") as inf, open(dst, "wb") as outf:
        while True:
            buf = inf.read(CHUNK)
            if not buf:
                break
            outf.write(buf)
            written += len(buf)
            now = time.time()
            frac = float(written) / float(total)
            pct = int(pct_lo + (pct_hi - pct_lo) * frac)
            if pct > pct_hi:
                pct = pct_hi

            elapsed = now - t_flash0
            avg_bps = (written / elapsed) if elapsed > 0.05 else None

            # Instantaneous rate over the last log window.
            win = now - last_log if last_log > 0 else elapsed
            delta = written - last_log_bytes
            cur_bps = (delta / win) if win >= 0.2 and delta > 0 else avg_bps

            do_ui = pct != last_pct or (now - last_ui) >= 0.5
            do_log = (now - last_log) >= LOG_INTERVAL_S or written == total

            if do_ui:
                eta = flash_eta(written, total, avg_bps)
                if eta < 0:
                    eta = estimate_eta(t0, max(pct, 1))
                ui_update(
                    status_path,
                    stage,
                    pct,
                    "%dM/%dM %s"
                    % (
                        int(human_mib(written)),
                        int(human_mib(total)),
                        fmt_rate(avg_bps),
                    ),
                    eta,
                    active,
                    target,
                )
                last_pct = pct
                last_ui = now

            if do_log:
                eta = flash_eta(written, total, avg_bps)
                eta_s = ("%ds" % eta) if eta >= 0 else "--"
                console_log(
                    "%s %6.1f/%6.1f MiB (%3d%%) cur=%s avg=%s eta=%s"
                    % (
                        stage,
                        human_mib(written),
                        human_mib(total),
                        int(100.0 * frac),
                        fmt_rate(cur_bps),
                        fmt_rate(avg_bps),
                        eta_s,
                    ),
                    log_path,
                )
                last_log = now
                last_log_bytes = written

        t_sync0 = time.time()
        outf.flush()
        try:
            os.fsync(outf.fileno())
        except OSError:
            pass
        fsync_s = time.time() - t_sync0

    # Also ask the block layer to flush the parent disk when possible.
    t_flush0 = time.time()
    try:
        parent = os.path.realpath(dst)
        # /dev/mmcblk0pN -> /dev/mmcblk0
        if "mmcblk" in parent and "p" in os.path.basename(parent):
            disk = parent.rsplit("p", 1)[0]
            if os.path.exists(disk):
                console_log("%s blockdev --flushbufs %s" % (stage, disk), log_path)
                subprocess.call(
                    ["blockdev", "--flushbufs", disk],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
    except OSError:
        pass
    flush_s = time.time() - t_flush0

    if written != total:
        raise SystemExit("short write: %s -> %s (%d/%d)" % (src, dst, written, total))

    elapsed = time.time() - t_flash0
    avg_bps = (written / elapsed) if elapsed > 0 else 0
    console_log(
        "%s done: %.1f MiB in %.1fs (avg=%s, fsync=%.2fs, flush=%.2fs)"
        % (
            stage,
            human_mib(written),
            elapsed,
            fmt_rate(avg_bps),
            fsync_s,
            flush_s,
        ),
        log_path,
    )

    ui_update(
        status_path,
        stage,
        pct_hi,
        "done %s" % fmt_rate(avg_bps),
        estimate_eta(t0, pct_hi),
        active,
        target,
    )


def main():
    p = argparse.ArgumentParser(description="Flash image with OTA progress")
    p.add_argument("image", help="source image file")
    p.add_argument("device", help="destination block device")
    p.add_argument("--stage", required=True, help="UI stage name")
    p.add_argument("--pct-lo", type=int, required=True)
    p.add_argument("--pct-hi", type=int, required=True)
    p.add_argument("--status", default=STATUS_PATH)
    p.add_argument("--t0", type=float, default=0.0, help="apply start unix time")
    p.add_argument("--active", default="")
    p.add_argument("--target", default="")
    p.add_argument(
        "--log",
        default="",
        help="also append progress lines to this log file",
    )
    args = p.parse_args()

    if not os.path.isfile(args.image):
        print("missing image: %s" % args.image, file=sys.stderr)
        return 1
    if not os.path.exists(args.device):
        print("missing device: %s" % args.device, file=sys.stderr)
        return 1

    try:
        flash_image(
            args.image,
            args.device,
            args.stage,
            args.pct_lo,
            args.pct_hi,
            args.status,
            args.t0,
            args.active,
            args.target,
            args.log or None,
        )
    except Exception as exc:
        console_log("flash failed: %s" % exc, args.log or None)
        print("flash failed: %s" % exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
