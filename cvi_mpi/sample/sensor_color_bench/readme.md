# sample_sensor_color_bench

三阶段颜色对比基准：同一条 VI→VPSS 管线上，把 **VI / VPSS-YUV / VPSS-RGB888** 三路输出按「正常理解」转成 RGB565，三等分显示在 LCD 上，用于定位颜色偏差出现在哪一步。

## 屏幕布局（172×320，从上到下）

| 区域 | 顶栏颜色 | 数据来源 | CPU 转换 |
|------|----------|----------|----------|
| 上 1/3 | 红 (2px) | `CVI_VI_GetChnFrame` NV21 | BT.601，NV21 V-U 顺序 |
| 中 1/3 | 绿 (2px) | `CVI_VPSS_GetChnFrame` Grp0 NV21 (scale+ROT90) | 同上 |
| 下 1/3 | 蓝 (2px) | `CVI_VPSS_GetChnFrame` Grp1 RGB888 (HW CSC) | byte0=R, byte1=G, byte2=B |

**刻意不做** SG2000 B-G-R 等 workaround，完全按 API 名称的字面含义处理。

## 管线

与 `sample_sensor_lcd` 相同：

`VI (NV21) → VPSS0 (320×192 NV21 + ROT90) → VPSS1 (192×320 RGB888 HW CSC)`

VI channel depth=1，VPSS 两路 depth=1，可同时 `GetChnFrame`。

## 编译

```bash
make -C cvi_mpi/sample/sensor_color_bench clean all
```

## 运行

```bash
imx678-mode status
killall sample_sensor_lcd sample_sensor_color_bench 2>/dev/null
sample_sensor_color_bench
```

## 如何读结果

- **仅最下行 (VPSS-RGB) 偏色** → HW CSC 或 RGB888 内存布局问题
- **中行起偏、上行正常** → VPSS scale/rotate/CSC 前 YUV 路径
- **三行都偏且一致** → 更可能在 VI/ISP 或环境/白平衡
- **仅下行 R/B 对调、上中行正常** → 典型 RGB888 字节序与枚举不符

首帧会打印每路分辨率、stride、pixel format。
