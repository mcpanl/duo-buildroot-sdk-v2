# lcd_fb_color_bench

独立 LCD 帧缓冲颜色基准程序，**不依赖摄像头、ISP、VPSS**。

## 用途

在排查颜色颠倒问题时，先确认屏幕本身 RGB 顺序是否正确。

## 显示内容（从上到下 5 个色块）

| 序号 | 预期颜色 | RGB | 说明 |
|------|----------|-----|------|
| 1 | 红 | (255,0,0) | 纯色 R |
| 2 | 绿 | (0,255,0) | 纯色 G |
| 3 | 蓝 | (0,0,255) | 纯色 B |
| 4 | 黄 | (255,255,0) | R+G，若 R/B 互换会显示为青 |
| 5 | 青 | (0,255,255) | G+B，若 R/B 互换会显示为黄 |

若 R/B 通道颠倒：**红↔蓝**，**黄↔青**，绿色不变。

## 编译

```bash
cd cvi_mpi/sample/lcd_fb_color_bench
make
```

## 运行

先停止其它占用 `/dev/fb0` 的进程（如 `screen_demo.py`、`sample_sensor_lcd`）。

```bash
./lcd_fb_color_bench
# 或指定设备 / 自动退出秒数
./lcd_fb_color_bench /dev/fb0 30
```

程序会打印 framebuffer 分辨率、RGB bitfield 偏移，以及每个色块写入的像素值。
