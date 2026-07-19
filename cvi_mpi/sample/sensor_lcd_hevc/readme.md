# sample_sensor_lcd_hevc

IMX678 camera preview on Zonhor JD9853 SPI LCD with on-demand **hardware H265** recording.

Target board: **Zonhor SG2000** (CV181X), same as `sample_sensor_lcd`.

## Pipeline

```
IMX678 (mode from sensor_cfg.ini / imx678-mode)
  -> VI NV21
  -> VPSS Grp0
       Chn0: 320x192 NV21 + ROT90 ---------> VPSS Grp1 RGB888 -> LCD 172x320
       Chn1: sensor WxH NV12 + ROT90 -> HxW --on-demand--> VENC H265
            (preview Chn0 paused while recording to keep encode FPS)
```

Only **two VPSS groups** (same as the preview sample). Encode channel is enabled only while recording. Encode size follows the **active sensor mode**:
- **1080p**: native portrait **1080×1920** (sensor 1920×1080 + ROT90)
- **5MP**: preview uses full **2880×1620** sensor; recording defaults to **9:16 max 1602×2848** (pre-scale 2848×1602 + ROT90). See [imx678_doc](../../../imx678_doc/IMX678_HEVC_record_resolution_SG2000.md).

Threads:

| Thread | Priority | Role |
|--------|----------|------|
| VENC GetStream | SCHED_RR 80 | Pull bitstream, write `.h265` (highest priority) |
| Touch | normal | Swipe / tap gestures |
| sys_status | normal | Battery / temperature |
| Preview (main) | low | LCD refresh; frozen + REC HUD while recording |

## UI

- **Top-left**: `REC MM:SS` while recording
- **Top-right / bottom-left**: battery % and temperature (same as `sample_sensor_lcd`)
- **Swipe up**: half-screen menu with **RECORD** / **STOP**
- **Swipe down** or **tap upper half**: hide menu

## Prerequisites

1. Sensor mode (default is 1080p 2x2 binning):

```bash
imx678-mode 1080p
# or: imx678-mode 5m
```

2. Stop other `/dev/fb0` / touch users:

```bash
killall screen_demo.py sample_sensor_lcd 2>/dev/null
```

## Build

From SDK root (after MMF toolchain env is set):

```bash
make -C cvi_mpi/sample/sensor_lcd_hevc clean
make -C cvi_mpi/sample/sensor_lcd_hevc
```

Or with all samples:

```bash
make -C cvi_mpi sample
```

## Deploy

```bash
cd cvi_mpi
scp sample/sensor_lcd_hevc/sample_sensor_lcd_hevc root@192.168.42.1:/mnt/system/usr/bin/
```

## Run

```bash
sample_sensor_lcd_hevc
# optional:
sample_sensor_lcd_hevc -b 8000 -o /mnt/data
sample_sensor_lcd_hevc -m -f   # mirror / flip
```

| Flag | Description |
|------|-------------|
| `-m` | VPSS horizontal mirror |
| `-f` | VPSS vertical flip |
| `-b` | H265 CBR bitrate in kbps (default 6000) |
| `-o` | Output directory (default `/mnt/data`, fallback `/tmp`) |
| `-r` | Auto-record N seconds then exit (headless test) |
| `-e` | Force 5MP encode downscale `WxH` (debug; see imx678_doc) |
| `-h` | Help |

## Verify recording

On the board after Stop:

```bash
ls -lh /mnt/data/rec_*.h265
```

On a PC (needs `ffmpeg`: `sudo apt install ffmpeg`):

```bash
# helper script (HEVC remux, no re-encode)
./h265_to_mp4.py rec_YYYYMMDD_HHMMSS.h265
# or:
ffplay rec_YYYYMMDD_HHMMSS.h265
ffmpeg -f hevc -i rec_YYYYMMDD_HHMMSS.h265 -c:v copy -tag:v hvc1 out.mp4
```

## Notes

- Output is a **raw H.265 elementary stream** (no MP4 mux in this sample).
- **5MP recording limits**: see [IMX678_HEVC_record_resolution_SG2000.md](../../../imx678_doc/IMX678_HEVC_record_resolution_SG2000.md).
- Encode path uses `SAMPLE_COMM_VPSS_Bind_VENC`; preview path is unchanged CPU pull.
- If touch `/dev/input/event2` is missing, preview and recording still work; only the on-screen menu is disabled (you can still Ctrl+C to exit).
- `Cannot open '/dev/cvi-vo'` is expected; this sample does not use VO.
