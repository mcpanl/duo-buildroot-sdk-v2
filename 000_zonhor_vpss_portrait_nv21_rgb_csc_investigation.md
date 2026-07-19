# Zonhor VPSS 竖屏 NV21 → RGB888 硬件 CSC 异常调研手册

> 日期：2026-07-19  
> 状态：已用端到端落盘实验缩小范围，待深入 VPSS / VB / ION 驱动与中间件源码  
> 配套文档：  
> - [`000_zonhor_mmf_camera_refactor_plan.md`](./000_zonhor_mmf_camera_refactor_plan.md)  
> - [`000_zonhor_mmf_camera_refactor_troubleshoot.md`](./000_zonhor_mmf_camera_refactor_troubleshoot.md)  
> - [`000_zonhor_mmf_vpss_output_size_fix_plan.md`](./000_zonhor_mmf_vpss_output_size_fix_plan.md)  
> 隔离 example：[`MaixCDK/examples/camera_capture_debug_demo`](./MaixCDK/examples/camera_capture_debug_demo)

---

## 1. 问题一句话

**竖屏（ROT90 后）NV21 本身正确；CPU 转 RGB888 正确；但用 VPSS 把这份竖屏 NV21 转成 packed RGB888 时画面异常。**  
横向（不旋转）NV21 → VPSS RGB888（只缩放）在同套代码路径上是正常的。

因此后续调研应聚焦：

1. **GDC / ROT90 输出的竖屏 NV21 布局**（stride、valid、letterbox）  
2. **下游 VPSS 组把该帧当输入做 CSC 时的假设**（尺寸、crop、设备、通道）  
3. **RGB888 输出的 VB 分配 / stride / ION 映射**是否与驱动实际写入一致  

而不是继续怀疑 `fb_display`、`resize(108,108)` 或应用层 JPEG 保存。

---

## 2. 已证实事实（实验证据）

### 2.1 隔离方法

新建最小 example：`camera_capture_debug_demo`

- 只 `camera::Camera` + `cam.read()`，无 FB / HUD / `resize`
- 连续取图 ≥1s，保存最后一帧
- 额外落盘：
  - `DISPLAY` 端点（同属 Group2 硬件 RGB）
  - **诊断用** Group0 NV21 → OpenCV CPU `NV21→BGR`（**不是产品方案**）

产物目录：

```text
MaixCDK/examples/camera_capture_debug_demo/artifacts/
```

### 2.2 对照矩阵

| # | 路径 | 拓扑要点 | 产物 | 结果 |
|---|------|----------|------|------|
| A | `cam.read()` MAIN_RGB | G0 ROT90 NV21 → G2-ChB RGB 720×720 | `last.jpg` | **异常**（条纹 + 绿/品红块） |
| B | DISPLAY RGB | 同 G0 → G2-ChA RGB 172×320 | `display.jpg` | **异常**（同类花屏） |
| C | G0 NV21 + CPU 转 RGB | 取 G0-Ch0 `1088×1920` NV21，CPU 转换 | `g0_cpu_rgb.jpg` | **结构正常**（可见 24 色卡） |
| D | 临时：无旋转 CSC | G0 `1920×1080` NV21 无旋转 → G2-Ch0 RGB `960×540` 只缩放 | `last_landscape_csc.jpg` | **正常** |

### 2.3 关键推论

```text
传感器 / VI / ISP / Group0 竖屏 NV21     ✅ 正常（C）
CPU NV21→RGB（诊断）                    ✅ 正常（C）
VPSS：横屏 NV21 → RGB（只缩放）         ✅ 正常（D）
VPSS：竖屏 ROT90 NV21 → RGB             ❌ 异常（A、B）
应用层 resize / FB 合成                 ❌ 已排除（A 在无 display 时已坏）
```

**CPU 转换只能证明“YUV 源数据可读且内容正确”，绝不能作为最终修复。** 最终修复必须落在硬件 RGB 路径。

---

## 3. 当前产品拓扑 vs 临时对照拓扑

### 3.1 产品目标拓扑（异常路径）

```text
IMX678 1080p_bin landscape
  VI 1920x1080 NV21
    -> Group0 / Dev0 / Ch0
         NV21 + GDC ROT90
         logical ~1080x1920
         storage/buffer often 1088x1920 (width 64-align)
         valid 常为 (4,0,1080,1920) 一类 letterbox
    -> Group2 / Dev1
         input = Group0-Ch0（可带 SetGrpCrop 去掉 letterbox）
         Ch0 DISPLAY  RGB888  e.g. 172x320
         Ch1 MAIN_RGB RGB888  e.g. 720x720   <-- Camera::read
         Ch2 half NV21 -> Group3（可 reserved）
```

参考实现：

- `MaixCDK/components/zonhor_mmf/src/zonhor_graph_profile.c`
- `MaixCDK/components/zonhor_mmf/src/zonhor_graph_runtime.c`
- `MaixCDK/components/vision/port/maixcam_zonhor/z_camera_zonhor.cpp`

### 3.2 临时对照拓扑（正常路径，代码里 `#if 1 TEMP_LANDSCAPE_CSC_TEST`）

```text
VI 1920x1080 NV21
  -> Group0 / Dev0 / Ch0
       NV21 1920x1080  NO rotation
  -> Group2 / Dev1 / Ch0
       RGB888 960x540  scale only, NO rotation
```

运行时日志特征：

```text
[zonhor_profile] TEMP_LANDSCAPE_CSC_TEST: G0-Ch0 NV21 1920x1080 no-rot -> G2-Ch0 RGB 960x540 scale-only
Group0 ch0 landscape_main_yuv attr=1920x1080 ... rot=0
Group2 ch0 display_preview attr=960x540 ... rot=0
MAIN_RGB meta fmt=0 wh=960x540 stride=2880 len=1555200
```

> 注意：仓库中该临时开关可能仍为打开状态；调研正式竖屏路径前应确认 `#if` 已改回产品拓扑，或在分支上明确标注。

### 3.3 已知可工作参考：`sample_sensor_lcd`

```text
VI -> VPSS Grp0 Dev0: NV21 + ROT90（小尺寸，约 320x192 预旋转）
   -> VPSS Grp1 Dev0: NV21 -> RGB888 CSC（约 192x320，同 Device0）
   -> CPU crop / FB
```

文件：

- `MaixCDK/components/zonhor_mmf/reference/sample_sensor_lcd.c`
- `MaixCDK/components/zonhor_mmf/reference/sample_sensor_lcd_readme.md`

与当前产品图差异（后续对比重点）：

| 项 | sample_sensor_lcd | 当前 zonhor_mmf 产品图 |
|----|-------------------|------------------------|
| ROT 后分辨率 | 小（~192×320） | 大（~1088×1920） |
| CSC 所在 Group | Grp1 | Group2 |
| CSC 所在 Device | Dev0（与 ROT 同设备） | Dev1（跨设备） |
| CSC 输入 | 小竖屏 NV21 | 大竖屏 NV21 + 可能 GrpCrop |
| RGB 通道数 | 通常 1 | 最多 3（disp/main/half） |

---

## 4. 异常画面特征（便于对源码症状）

竖屏 VPSS RGB 落盘典型表现：

1. **上半部**：细密水平彩条 / zipper / combing  
2. **下半部**：大块霓虹绿、品红/洋红矩形，隐约可见色卡网格残影  
3. 观感高度类似：**把 YUV 平面数据当 packed RGB 读**，或 **stride/行起点系统性错位**

这不等于“一定是软件把 NV21 当 RGB 拷了”——因为：

- `enPixelFormat` 上报为 `PIXEL_FORMAT_RGB_888 = 0`
- `u32Length[0]` / `u32Stride[0]` 与 packed RGB 公式可自洽  
- 同拷贝逻辑在横向 CSC 路径下能出正常图  

更可能是：**硬件写入的“RGB buffer”内容本身已错**，或 **竖屏输入布局令 CSC 按错误几何解释**。

---

## 5. 运行时元数据（竖屏异常路径实测）

### 5.1 720×720 MAIN_RGB（异常）

```text
fmt=0 (PIXEL_FORMAT_RGB_888)
wh=720x720
stride=2176/0/0
len=1566720/0/0
valid=(0,0 720x720)
desc_logic=720x720 desc_buf=768x720
```

校验：

- `720 * 3 = 2160`
- `align(2160, 64) = 2176`  ← **与实测 stride 一致**
- `2176 * 720 = 1566720` ← **与实测 length 一致**
- `768 * 3 = 2304` ← **profile buffer_width×3，并非硬件 stride**

结论：

> 对 packed RGB888，Sophgo `COMMON_GetPicBufferSize` 一类公式是  
> `stride = ALIGN(width * 3, 64)`，  
> **不是**先把像素宽 align 到 64 再 `*3`。  
> 当前 `z_frame_layout_calc()` 的 `buffer_width = align_up_64(logical_w)` 描述的是**另一套**“像素对齐存储宽”语义，和 RGB packed 的 byte-stride 语义可能不一致。

横向正常路径下 `960×540`：

```text
stride=2880 = 960*3   （恰好已 64 对齐，无需再抬）
len=1555200 = 2880*540
```

### 5.2 DISPLAY 172×320（异常，与 MAIN 同类）

```text
wh=172x320
stride=576
len=184320
```

校验：`ALIGN(172*3, 64) = ALIGN(516,64) = 576`，`576*320 = 184320`。  
同样符合 packed RGB byte-stride 模型；内容仍花屏 → **不是“只 MAIN 拷贝写错”**。

### 5.3 Group0 竖屏 NV21（正常内容）

```text
fmt=19 (PIXEL_FORMAT_NV21)
wh=1088x1920
stride=1088/1088
len=2088960/1044480
```

CPU 转 RGB 后可见色卡 → **G0 输出可读且内容正确**。

### 5.4 ION / 虚地址注意点

`Camera::_read_rgb888_frame` 中曾观察到：若直接使用 `GetChnFrame` 返回的 `pu8VirAddr`，可能出现 **不同 phy 对应同一 stale VA**。  

当前隔离代码改为：

- **始终** `CVI_SYS_MmapCache(phy, length)`  
- `CVI_SYS_IonInvalidateCache`  
- 拷贝后 `Munmap`  

与 `zonhor_fb_lcd.c` / `sample_sensor_lcd` 的 map 方式对齐。  
在强制 mmap 后竖屏 RGB **仍然花屏** → 主因不是“偶发 stale VA”，但 **ION 映射与 cache 一致性仍应在驱动侧复核**。

---

## 6. 已做过、未修复竖屏 RGB 的实验

| 实验 | 做法 | 结果 |
|------|------|------|
| 跳过 Group2 `SetGrpCrop` | 不裁 letterbox | 仍花屏 |
| 只 Enable G2-Ch1 | 关掉 disp/half | 仍花屏 |
| RGB `SetChnAttr` 改用 buffer 宽 768 | 对齐像素宽 | metadata 变 `768 / stride=2304`，内容仍花屏 |
| Group2 放到 Dev0 | 仿 sample 同设备 | 部分配置 `SetChnAttr` 失败 `0xc0068003` |
| Dev0 + 仅 Ch0 + 对齐尺寸 | 更贴近 sample | 仍未能在竖屏大图上得到正常 RGB |
| 横屏无旋转 + 缩放 CSC | 见 §2.2 D | **成功** |

`0xc0068003`（`CVI_ERR_VPSS_ILLEGAL_PARAM`）在参考文档中常与：

- GDC 不支持对 RGB 做旋转  
- 或尺寸 / 对齐非法  

相关，见 `sample_sensor_lcd_readme.md`。

---

## 7. 后续源码调研范围（VPSS / VB / ION）

### 7.1 建议优先级

```text
P0  竖屏 NV21（G0 出）在内存中的真实布局
    vs Group2 CSC 认为的输入布局（宽高/stride/crop）

P1  Group2 RGB 输出 VB 块大小、stride、plane 数
    vs 驱动实际写入 / GetChnFrame 上报

P2  跨 VPSS Device（Dev0 ROT → Dev1 CSC）绑定与 dual mode 语义

P3  ION map / invalidate / 多通道共享池是否踩踏
```

### 7.2 本仓库应先读的文件

| 层级 | 路径 | 看什么 |
|------|------|--------|
| Profile | `zonhor_mmf/src/zonhor_graph_profile.c` | ROT90 extent、letterbox valid、G2 输入 extent |
| Runtime | `zonhor_mmf/src/zonhor_graph_runtime.c` | `fill_chn_attr`、`SetChnRotation`、`SetGrpCrop`、`Bind`、`VB` 池、`u8VpssDev` |
| Layout | `zonhor_mmf/src/zonhor_frame_layout.c` | `logical/buffer/valid`；与 RGB byte-stride 是否混用 |
| Camera 读 | `vision/port/maixcam_zonhor/z_camera_zonhor.cpp` | `_read_rgb888_frame` stride/valid/mmap |
| FB 读 | `zonhor_mmf/src/zonhor_fb_lcd.c` | `Mmap` + `IonInvalidate` + Extent 拷贝 |
| 参考金样 | `zonhor_mmf/reference/sample_sensor_lcd.c` | 小竖屏 CSC 如何配 |
| Buffer 公式 | `3rd_party/.../cvi_buffer.h` `COMMON_GetPicBufferSize` | RGB packed：`ALIGN(w*3,64)` |

### 7.3 中间件 / 内核侧建议检索方向

在 BSP / `cvi_mpi` / `vpss` / `gdc` / `vb` / `ion` 源码中按关键词推进：

1. **GDC ROT90 NV21**  
   - 旋转后 `u32Width/Height/Stride` 谁为准？  
   - letterbox / `ASPECT_RATIO_AUTO` 黑边写在左侧还是居中？  
   - `valid` 是否保证在 NV21 的 Y/UV 平面一致？

2. **VPSS CSC（NV21 → RGB888）**  
   - 输入 crop（`SetGrpCrop`）与旋转后 padding 的坐标系  
   - 大分辨率（1088×1920）CSC 是否有未文档化限制  
   - Dev0→Dev1 bind 时，输入帧是否必须满足特定 align

3. **VB**  
   - RGB pool：`vb_blk(buffer_w, buffer_h, PIXEL_FORMAT_RGB_888)`  
     若按像素 `buffer_w=768` 计算，而硬件按 `ALIGN(720*3,64)` 写，是否仍安全（通常更大安全），或是否存在**池规格与通道属性不一致导致驱动选错 pool**  
   - 多通道同时 Enable 时 pool 不足 / 复用错误（本实验单通道仍失败，优先级低于布局）

4. **ION**  
   - `MmapCache` vs `Mmap`  
   - `IonInvalidateCache` 长度是否必须覆盖全部 plane  
   - `GetChnFrame` 是否保证 `pu8VirAddr` 有效（用户态已规避，驱动语义仍值得确认）

### 7.4 推荐的源码级验证实验（下一步）

按“只改一个变量”原则：

1. **固定横屏 CSC 成功配置**，仅把 G0 改为 ROT90，RGB 仍输出小尺寸（如 192×320），看是否立刻复现花屏。  
   - 若是 → 强指向“旋转后 NV21 作为 CSC 输入”问题。  
2. **竖屏 NV21 保持不变**，CSC 输出改到与 sample 相同的 192×320 / Dev0 / Group1，看是否恢复。  
   - 用于区分“大分辨率” vs “跨 Device / Group 编号”。  
3. **Dump G0 一帧 raw NV21 + G2 RGB raw**，在 PC 上分别按：  
   - NV21 官方布局  
   - RGB `stride=ALIGN(w*3,64)`  
   - 错误假设 `stride=align(w,64)*3`  
   做离线可视化，确认花屏更像“内容错”还是“读法错”。  
4. 在 `CVI_VPSS_GetChnFrame` 返回后立刻打印并断言：  
   - `enPixelFormat`  
   - `u32Width/Height/Stride/Length`  
   - 与 `SetChnAttr` / VB blk size 的关系表  

---

## 8. 工作假设（按当前证据排序）

### H1（最优先）：旋转后 NV21 几何与 Group2 CSC 输入假设不一致

- G0：`ASPECT_RATIO_AUTO` + ROT90 → 1088 宽中有 1080 有效 + 两侧 padding  
- G2：`SetGrpCrop` 或默认全 buffer 输入  
- CSC 若按错误宽/ stride 解释 Y/UV，输出“像 RGB 的垃圾”

支持证据：横屏无旋转成功；竖屏 G0 CPU 成功；竖屏 G2 RGB 失败。

### H2：大尺寸竖屏 CSC 或跨 Device CSC 路径有缺陷 / 未覆盖配置

- sample 成功路径是小图 + 同 Dev0  
- 产品路径是大图 + Dev1  

### H3：RGB VB / stride 双语义混用放大了偶发问题

- `buffer_width=align(logical,64)` vs `stride=ALIGN(logical*3,64)`  
- 对 720：768×3 vs 2176；对 960：两者碰巧一致  

横屏成功说明“混用”不是充分条件，但竖屏路径上 VB 规格与 attr 组合仍可能触发错误 pool / 错误内部 stride。

### H4：ION/cache 不是主因，但是加固点

强制 mmap+invalidate 后仍失败 → 降为次要。

### 明确否决

- ❌ 最终用 CPU NV21→RGB 当产品修复  
- ❌ 主因是 `fb_display_camera_demo` 的 `resize(108,108)`  
- ❌ 主因是 JPEG `Image::save` 颜色颠倒（横向 CSC 同 save 路径正常）

---

## 9. 复现步骤（调研用）

### 9.1 竖屏异常路径（产品拓扑）

```bash
# 确认 TEMP_LANDSCAPE_CSC_TEST 已关闭（#if 0）
export MAIXCDK_PATH=/home/satuo/zonhor/maix_arm64/MaixCDK/
cd $MAIXCDK_PATH/examples/camera_capture_debug_demo
maixcdk build --build-type=Debug -p zonhor --arch arm64
scp -r dist/ root@192.168.100.141:/root/

ssh root@192.168.100.141
imx678-mode 1080p
cd /root/dist/camera_capture_debug_demo_debug_arm64
export LD_LIBRARY_PATH=./dl_lib:/root/dist/fb_display_camera_demo_debug_arm64/dl_lib:$LD_LIBRARY_PATH
./camera_capture_debug_demo 720 720 1000 /root/camera_capture_debug/last.jpg
```

期望：`last.jpg` / `display.jpg` 花屏；`g0_cpu_rgb.jpg` 结构正常。

### 9.2 横屏对照路径（临时开关）

打开 `TEMP_LANDSCAPE_CSC_TEST` 后：

```bash
./camera_capture_debug_demo 960 540 1000 /root/camera_capture_debug/last_landscape.jpg
```

期望：硬件 RGB 落盘正常。

---

## 10. 关键日志锚点

竖屏异常路径应能看到类似：

```text
[z_extent] rotated_main logical=1080x1920 buffer=1088x1920 valid=(4,0 1080x1920)
Group0 ... rot=90 pre_rot_attr=1
Group2 input crop enable=(4,0 1080x1920) from storage=1088x1920
Group2 ch1 main_venc_input attr=720x720 ... storage=768x720
MAIN_RGB meta ... fmt=0 wh=720x720 stride=2176 ... len=1566720
```

横屏成功路径：

```text
TEMP_LANDSCAPE_CSC_TEST: G0-Ch0 NV21 1920x1080 no-rot -> G2-Ch0 RGB 960x540 scale-only
Group0 ... rot=0
Group2 ch0 ... attr=960x540
MAIN_RGB meta ... stride=2880 len=1555200
```

---

## 11. 调研产出检查清单

完成一轮 VPSS/VB/ION 源码调研后，文档/补丁应能回答：

- [ ] 旋转后 NV21 的 **权威** `width/height/stride/valid` 以谁为准（GDC？VPSS GetFrame？）  
- [ ] Group2 CSC 输入 crop 坐标系是否与 G0 letterbox 一致  
- [ ] RGB888 packed 的 VB 应按 `ALIGN(w*3,64)*h` 还是 `align(w,64)*3*h` 分配  
- [ ] Dev0→Dev1 bind 对 CSC 是否有额外约束  
- [ ] 为何小竖屏 sample 成功、大竖屏产品失败（分辨率 / device / group / crop 哪一项是开关）  
- [ ] 修复后：`camera_capture_debug_demo` 在 720×720 与 172×320 上，硬件 RGB 落盘应接近 `g0_cpu_rgb` 的结构质量（允许曝光/WB 差异）

---

## 12. 相关代码入口（速查）

```text
MaixCDK/components/zonhor_mmf/src/zonhor_graph_profile.c      # 拓扑与 extent
MaixCDK/components/zonhor_mmf/src/zonhor_graph_runtime.c      # Create/Bind/Crop/VB/SetCamSize
MaixCDK/components/zonhor_mmf/src/zonhor_frame_layout.c       # logical/buffer/valid
MaixCDK/components/zonhor_mmf/src/zonhor_fb_lcd.c             # ION map + RGB 读
MaixCDK/components/vision/port/maixcam_zonhor/z_camera_zonhor.cpp  # Camera::read RGB 拷贝
MaixCDK/components/zonhor_mmf/reference/sample_sensor_lcd.c   # 已知可工作小竖屏 CSC
MaixCDK/examples/camera_capture_debug_demo/                   # 落盘隔离复现
```

---

## 13. 修订记录

| 日期 | 内容 |
|------|------|
| 2026-07-19 | 首版：基于 camera_capture_debug_demo 竖屏失败 / 横屏成功对照，整理 VPSS·VB·ION 后续调研提纲 |
