#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Convert raw H.265 / HEVC elementary stream (.h265/.hevc/.265) to MP4 (HEVC copy).

Requires ffmpeg on PATH. Example:
  ./h265_to_mp4.py rec_20260717_150826.h265
  ./h265_to_mp4.py /mnt/data/rec_*.h265 -o ~/Videos/
  ./h265_to_mp4.py clip.h265 -o out.mp4 --fps 30
"""

from __future__ import annotations

import argparse
import glob
import shutil
import subprocess
import sys
from pathlib import Path


def find_ffmpeg() -> str:
    path = shutil.which("ffmpeg")
    if not path:
        print(
            "error: ffmpeg not found on PATH.\n"
            "  Ubuntu/Debian: sudo apt install ffmpeg\n"
            "  Or install a static build and ensure 'ffmpeg' is in PATH.",
            file=sys.stderr,
        )
        sys.exit(1)
    return path


def default_out_path(src: Path, out_arg: Path | None) -> Path:
    if out_arg is None:
        return src.with_suffix(".mp4")
    if out_arg.is_dir() or str(out_arg).endswith(("/", "\\")):
        return out_arg / (src.stem + ".mp4")
    return out_arg


def convert_one(
    ffmpeg: str,
    src: Path,
    dst: Path,
    fps: float | None,
    overwrite: bool,
) -> int:
    if not src.is_file():
        print(f"error: input not found: {src}", file=sys.stderr)
        return 1

    dst.parent.mkdir(parents=True, exist_ok=True)

    cmd = [
        ffmpeg,
        "-hide_banner",
        "-loglevel",
        "error",
        "-stats",
        "-y" if overwrite else "-n",
        "-f",
        "hevc",
    ]
    if fps and fps > 0:
        # Hint container timing when the elementary stream has no timing.
        cmd.extend(["-r", str(fps)])
    cmd.extend(
        [
            "-i",
            str(src),
            "-c:v",
            "copy",
            "-tag:v",
            "hvc1",  # better QuickTime / iOS compatibility than hev1
            "-movflags",
            "+faststart",
            str(dst),
        ]
    )

    print(f"mux: {src} -> {dst}")
    try:
        subprocess.run(cmd, check=True)
    except subprocess.CalledProcessError as e:
        print(f"error: ffmpeg failed (exit {e.returncode})", file=sys.stderr)
        return e.returncode or 1

    size = dst.stat().st_size if dst.is_file() else 0
    print(f"ok:   {dst} ({size} bytes)")
    return 0


def expand_inputs(patterns: list[str]) -> list[Path]:
    files: list[Path] = []
    for pat in patterns:
        matched = sorted(glob.glob(pat))
        if matched:
            files.extend(Path(p) for p in matched)
        else:
            # literal path (may not exist yet; convert_one will report)
            files.append(Path(pat))
    # de-dupe while preserving order
    seen = set()
    out: list[Path] = []
    for p in files:
        rp = p.resolve() if p.exists() else p
        key = str(rp)
        if key not in seen:
            seen.add(key)
            out.append(p)
    return out


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Mux raw H.265 elementary stream into an MP4 (HEVC, no re-encode)."
    )
    parser.add_argument(
        "inputs",
        nargs="+",
        help="Input .h265/.hevc/.265 file(s) or glob patterns",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=None,
        help="Output .mp4 file, or a directory (multi-input). Default: same name as input.",
    )
    parser.add_argument(
        "--fps",
        type=float,
        default=30.0,
        help="Assumed frame rate for timing (default: 30). Use 0 to omit -r.",
    )
    parser.add_argument(
        "-f",
        "--force",
        action="store_true",
        help="Overwrite existing output",
    )
    args = parser.parse_args()

    ffmpeg = find_ffmpeg()
    inputs = expand_inputs(args.inputs)
    if not inputs:
        print("error: no inputs", file=sys.stderr)
        return 1

    if args.output is not None and len(inputs) > 1:
        out = args.output
        if out.suffix.lower() == ".mp4" and not out.is_dir():
            print(
                "error: with multiple inputs, -o must be a directory",
                file=sys.stderr,
            )
            return 1
        out.mkdir(parents=True, exist_ok=True)

    fps = args.fps if args.fps and args.fps > 0 else None
    rc = 0
    for src in inputs:
        dst = default_out_path(src, args.output)
        if dst.exists() and not args.force:
            print(f"skip: {dst} exists (use -f to overwrite)", file=sys.stderr)
            rc = rc or 1
            continue
        r = convert_one(ffmpeg, src, dst, fps, args.force)
        if r != 0:
            rc = r
    return rc


if __name__ == "__main__":
    sys.exit(main())
