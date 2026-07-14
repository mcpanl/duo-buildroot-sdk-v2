# SG2000 Zonhor 大小核异构架构与协同设计

> 板级：`sg2000_zonhor_sg2000_glibc_arm64_emmc`  
> 文档用途：后续重构大小核分工、RTOS/Linux 协同时参考  
> 最后整理：2026-07-15

---

## 1. 芯片与板级概览

SG2000（CV181x 系列）是 **三颗逻辑 CPU 槽位** 的异构 SoC，通过 Mailbox 编号区分：

| Mailbox CPU ID | 架构 | 角色 | Zonhor 当前状态 |
|----------------|------|------|-----------------|
| **CPU0** | ARM Cortex-A53 | 大核 / 主系统 | ✅ 运行 Linux (glibc arm64) |
| **CPU1** | RISC-V C906**B** | 大核备选 | ❌ 未启用（与 A53 二选一启动 Linux） |
| **CPU2** | RISC-V C906**L** | 小核 / RTOS 专用 | ✅ 固定运行 FreeRTOS (CVIRTOS) |

**关键理解：**

- 大核 **ARM / RISC-V 互斥**：同一时刻只有一个跑 Linux（U-Boot + Kernel + 用户态）。
- 小核 C906L **永远存在**，但 **不是通用 Linux CPU**：SDK 仅提供 FreeRTOS（`cvirtos`），专做媒体/实时协处理。
- 工程上「小核只能跑 RTOS」体现为：仅 **2MB 代码区** + 预定义驱动栈 + 无 Linux 生态，而非硬件不能执行其他指令。

相关配置：

- 板级 defconfig：`build/boards/cv181x/sg2000_zonhor_sg2000_glibc_arm64_emmc/sg2000_zonhor_sg2000_glibc_arm64_emmc_defconfig`
  - `CONFIG_ENABLE_FREERTOS=y`
  - `CONFIG_ENABLE_RTOS_DUMP_PRINT=y`
- DTS：`build/boards/cv181x/sg2000_zonhor_sg2000_glibc_arm64_emmc/dts_arm64/`

Mailbox 路由定义见：`freertos/cvitek/common/include/riscv64/cvi_mailbox.h`

```
FREERTOS 侧：RECEIVE_CPU=2 (C906L)，SEND_TO_CPU0=CA53，SEND_TO_CPU1=C906B
Linux CA53：RECEIVE_CPU=0，SEND_TO_CPU=2 (C906L)
```

---

## 2. 内存布局（512MB DDR）

定义文件：`build/boards/cv181x/sg2000_zonhor_sg2000_glibc_arm64_emmc/memmap.py`

```
0x80000000 ─────────────────────────────────────┐
              Linux Kernel + 用户态 (510MB)      │
              U-Boot / ATF                       │
0x95400000 ─── ION 多媒体共享区 (170MB) ────────┤  ← 双核零拷贝关键
              ├─ H26x 码流缓冲 (2MB)             │
              ├─ ISP 缓冲 (20MB)                  │
0x9FE00000 ─── C906L FreeRTOS 固件区 (2MB) ─────┘
0xA0000000 ─── DDR 结束
```

| 区域 | 地址 | 大小 | 说明 |
|------|------|------|------|
| Linux 可用 DDR | `0x80000000` | 510MB | 内核 DTS `memory@` 节点 |
| ION 共享 | `FREERTOS_ADDR - 170MB` | 170MB | 多媒体缓冲 carveout |
| ISP 缓冲 | `ISP_MEM_BASE_ADDR` | 20MB | FSBL 写入 `transfer_config` |
| H26x 码流 | `H26X_BITSTREAM_ADDR` | 2MB | Fast Image 用 |
| FreeRTOS 固件 | `0x9FE00000` | 2MB | `blcp_2nd` 加载地址 |

双核协作的物理基础：**共享 DDR carveout（ION/ISP/H26x）+ Mailbox 8 字节命令**，而非大核给小核发可执行文件。

---

## 3. 启动时序

```
ROM → FSBL BL2
  ├─ DDR 初始化
  ├─ load blcp_2nd (cvirtos) → reset_c906l()     ← C906L 极早启动
  │     └─ init_comm_info() 写入 transfer_config
  ├─ C906L T1：可选 start_camera() Fast Image
  └─ 加载 ATF / U-Boot / Linux (A53)

Linux 启动
  ├─ insmod cv181x_rtos_cmdqu.ko
  ├─ insmod cv181x_fast_image.ko
  └─ LINUX_INIT_DONE ↔ RTOS_INIT_DONE 握手 → T2 协同阶段
```

关键代码路径：

| 阶段 | 文件 |
|------|------|
| 释放 C906L | `fsbl/plat/cv181x/bl2/bl2_opt.c` → `reset_c906l()` |
| 共享配置 | `fsbl/plat/cv181x/bl2/bl2_main.c` → `init_comm_info()` |
| RTOS 入口 | `freertos/cvitek/task/comm/src/riscv64/comm_main.c` → `main_cvirtos()` |
| Linux 握手 | `osdrv/interdrv/fast_image/fast_image.c` → probe 时 `LINUX_INIT_DONE` |
| KO 加载顺序 | `device/zonhor-sg2000-glibc-arm64-emmc/overlay/mnt/system/ko/loadsystemko.sh` |

MCU 状态机（`fsbl/plat/cv181x/include/platform_def.h`）：

- `MCU_STATUS_RTOS_T1_*`：Linux 运行前
- `MCU_STATUS_RTOS_T2_*`：Linux 运行后
- `MCU_STATUS_LINUX_*`：Linux 侧状态

---

## 4. 双核通信机制

### 4.1 硬件 Mailbox + Spinlock

- **Mailbox**：8 个 slot，每条消息 `cmdqu_t` 仅 8 字节（`ip_id + cmd_id + param_ptr`）。
- **硬件 Spinlock**（`SPIN_MBOX`）：跨核访问 Mailbox 缓冲区时防竞态。
- Linux 设备节点：`/dev/cvi-rtos-cmdqu`（驱动 `osdrv/interdrv/rtos_cmdqu/`）。

IP 模块路由（`osdrv/interdrv/rtos_cmdqu/rtos_cmdqu.h`）：

```
IP_ISP, IP_VCODEC, IP_VIP, IP_VI, IP_RGN, IP_AUDIO, IP_SYSTEM, IP_CAMERA
```

### 4.2 Fast Image

- 设备：`/dev/cvi-fast-image`
- 用途：查询 ISP/编码缓冲地址、抓拍、trace 等
- 底层仍走 `rtos_cmdqu`

### 4.3 共享配置结构

`struct transfer_config_t`（`fsbl/plat/cv181x/include/platform_def.h`）含 ISP/编码地址、magic header、mcu/linux 状态等，由 FSBL 写入 Mailbox 字段（`MAILBOX_FIELD = 0x1900400`）。

### 4.4 RTOS 任务模型

`freertos/cvitek/task/comm/src/riscv64/comm_main.c` 中任务队列：

| 任务名 | 职责 |
|--------|------|
| CMDQU | Mailbox 调度（最高工作优先级） |
| ISP / VCODEC / VI / CAMERA | 媒体管线（可分发） |
| RGN | OSD 区域叠加（`prvRGNRunTask`） |
| AUDIO | 实时音频处理 |

RTOS 链接库（`freertos/cvitek/task/main/CMakeLists.txt`）：`comm isp vi vcodec rgn audio camera`（Fast Image 开启时）。

RTOS `cvi_sys` 已初始化 **VPSS + DWA**（`freertos/cvitek/driver/sys/src/cvi_sys.c`），硬件旋转/仿射在 RTOS 侧理论可行。

---

## 5. Zonhor 外设归属（当前 Linux DTS）

| 外设 | 总线/接口 | 当前驱动侧 |
|------|-----------|------------|
| IMX678 相机 | MIPI + I2C3 (`bus_id=3`) | Linux cvi_mpi / `sample_sensor_lcd` |
| JD9853 LCD | SPI3 + GPIO (DC/RST/TE/BL) | Linux `fbtft` + `/dev/fb0` |
| ICM42688 IMU | I2C1 @ 0x18 | Linux `icm42688p` |
| Hynitron 触摸 | I2C1 @ 0x15 + GPIOA28 IRQ | Linux `hynitron` |
| WiFi | SDIO1 | Linux |
| VO/MIPI DSI | — | DTS 中 `disabled`（本板用 SPI 屏） |

传感器配置：`device/zonhor-sg2000-glibc-arm64-emmc/overlay/mnt/system/usr/bin/sensor_cfg.ini.imx678_*`

---

## 6. 规划功能：RTOS / Linux 分工

### 6.1 功能一：IMU 轻量防抖（旋转/缩放/平移）

**推荐：RTOS 为主，Linux 为辅**

```
IMU (I2C1) ──► RTOS: 高频采样 + 陀螺积分
VI 帧中断  ──► RTOS: 取 u32TimeRef / u64PTS
                    ▼
              每帧仿射矩阵（平移 + 微旋转 + 可选缩放）
                    ▼
         VPSS LDC / DWA 硬件 warp（NV21，64 像素对齐）
                    ▼
         共享 ION ──► Linux 显示 / 编码 / 存储
```

| 环节 | RTOS | Linux |
|------|------|-------|
| ICM42688 驱动与 1kHz+ 采样 | ✅ | |
| 帧-IMU 时间对齐 | ✅ | |
| 轻量 EIS 算法 + 硬件 warp | ✅ | |
| 参数配置 / UI / 标定 | | ✅ |
| 高质量离线防抖重建 | | ✅ |

注意：

- 相机 I2C3、IMU I2C1 **总线不冲突**，但 IMU 防抖期间需 **独占归 RTOS**（Linux 不 probe）。
- GDC 仅支持 NV12/NV21（参见 `cvi_mpi/sample/sensor_lcd/readme.md`）。
- 当前 `sample_sensor_lcd` 全链路在 Linux，offload 需将 VPSS Grp0（旋转/warp）迁到 RTOS。

### 6.2 功能二：开机尽快 SPI 屏显示相机画面

**推荐：RTOS 负责「相机先亮」，Linux 负责「屏驱动」—— 或远期 RTOS 全接管显示**

当前瓶颈在显示链路，而非相机：

```
VI (NV21) → VPSS0 (scale+ROT90) → VPSS1 (CSC→RGB888) → CPU RGB565 → /dev/fb0 → SPI
```

参考：`cvi_mpi/sample/sensor_lcd/`

| 子步骤 | RTOS（当前/可扩展） | Linux（必须/现状） |
|--------|---------------------|-------------------|
| Sensor 上电、MIPI 出图 | ✅ `start_camera()` T1 已有 | |
| VI/ISP/VPSS 缩放旋转 | ✅ 可扩展 | 当前在此 |
| RGB888→RGB565 | ⚠️ 可共享缓冲后任一侧 | 当前 CPU 做 |
| JD9853 SPI 刷屏 | ❌ SDK 无驱动 | ✅ `fbtft` |
| `/dev/fb0` | — | ✅ 必须 |

**近期快启策略：**

1. RTOS T1：`start_camera` + VI（+ 可选 VPSS）→ 画面已在 DDR。
2. Linux 早期：优先 insmod `mipi_rx` / `vi` / `vpss` / `jd9853`。
3. 最小用户态：mmap 共享缓冲 + fb blit，不做 TPU/网络。

**远期（RTOS 支持 SPI 后）：** 见第 7 节。

### 6.3 功能三：文件系统记录 IMU + 帧 ID

**推荐：RTOS 采集对齐，Linux 落盘**

| 环节 | RTOS | Linux |
|------|------|-------|
| IMU 高频采样 | ✅ | |
| 每帧 `u32TimeRef` / `u64PTS` 绑定 | ✅（VI 中断） | |
| 写入环缓冲 | ✅（ION carveout） | |
| eMMC 文件写入 | ❌ 无 FS | ✅ |
| 后期离线防抖 | | ✅ |

建议共享结构（环缓冲，约 1–4MB）：

```c
struct imu_frame_log {
    uint32_t frame_time_ref;   /* VIDEO_FRAME_S.u32TimeRef */
    uint64_t frame_pts;
    uint32_t gyro[3];
    uint32_t accel[3];
    uint32_t imu_ts_us;
};
```

RTOS 每帧 push；Linux 低优先级 `imu_logger` 线程批量 `fwrite`。

帧标识字段见：`cvi_mpi/include/linux/cvi_comm_video.h` → `VIDEO_FRAME_S`

---

## 7. 远期架构：RTOS 接管 SPI 显示 + 触摸

### 7.1 动机

Linux `fbtft` 虽支持 TE（`te = <&portb 11 0>`），但刷新路径经过 workqueue + 调度，A53 繁忙时易错过 TE 窗口导致撕裂。  
RTOS 高优先级 Display Task + TE 中断/同步，更适合防撕裂，且可解放 A53。

### 7.2 共享 RGB565 三缓冲

屏参：**172 × 320 RGB565**

```
单帧：172 × 320 × 2 = 110,080 字节 ≈ 108 KB
三缓冲：≈ 324 KB（建议 ION 划 512KB–1MB）
```

协议示意：

```
┌─────────────────────────────────────────┐
│ display_shm（ION carveout）              │
│   buf[0..2]  : RGB565                   │
│   write_idx  : Linux 写完待刷编号         │
│   frame_seq  : 帧序号                     │
│   dirty      : atomic 标志                │
└─────────────────────────────────────────┘
         ▲                    │
    Linux 渲染/LVGL           │ TE 同步 → SPI3 burst
                              ▼
                    RTOS DisplayTask
```

SPI 带宽（DTS `spi-max-frequency = 62500000`）：全帧理论 ~14ms，实际 **~30fps** 预览较现实。

### 7.3 触摸放 RTOS

| 资源 | 配置 | RTOS 可行性 |
|------|------|-------------|
| Hynitron @ 0x15 | I2C1 | ✅ `hal_dw_i2c` |
| IRQ | GPIOA28 | ✅ `request_irq` |
| RST | GPIOB21 | ⚠️ 需扩展 GPIO HAL |

触摸事件经共享环缓冲或 Mailbox 送 Linux/LVGL：

```c
struct touch_event {
    uint32_t seq;
    uint16_t x, y;
    uint8_t  pressed;
    uint64_t ts_us;
};
```

### 7.4 端到端 RTOS 显示管线（终极目标）

```
VI/VPSS/EIS (RTOS) → RGB565 共享缓冲 → DisplayTask (RTOS, TE+SPI)
                                              ↑
TouchTask (RTOS, I2C1+IRQ) ──────────────────┘
```

Linux 仅：业务逻辑、网络、存储、TPU 推理、UI 合成写共享缓冲。

### 7.5 硬件引脚（Zonhor JD9853）

| 信号 | GPIO / 总线 |
|------|-------------|
| SPI3 | VIVO_D5~D8（U-Boot `cvi_board_init.c` 已 pinmux） |
| DC | GPIOB20 |
| RST | GPIOB12 |
| TE | GPIOB11 |
| BL | GPIOA20 |
| Touch IRQ | GPIOA28 |
| Touch RST | GPIOB21 |

### 7.6 当前 SDK 差距

| 组件 | 现状 |
|------|------|
| SPI3 HAL/驱动 | RTOS **无**（仅有 SPI1 pinmux 定义） |
| GPIO | 极简（仅 `gpio_direction_output`），**无输入/TE 中断** |
| JD9853 驱动 | U-Boot 有（`u-boot-2021.10/cmd/jd9853_logo.c`），Linux 有 `fbtft`，RTOS **无** |
| Hynitron 触摸 | 仅 Linux，RTOS 需移植 |
| 双核争用 | Linux 须 disable `spi3`/`jd9853`/`hynitron` 或协调所有权 |

可复用参考：

- U-Boot `jd9853_logo.c`：SPI 协议、初始化序列
- Linux `fbtft_wait_te()`：`linux_5.10/drivers/staging/fbtft/fbtft-core.c`
- ESP-IDF `esp_lcd_jd9853.c`：FreeRTOS 面板驱动结构
- DTS `fbtft,skip-init`：U-Boot init 后 RTOS/Linux 可只刷像素

---

## 8. 硬件所有权划分（重构时必守）

| 资源 | 预览/防抖/RTOS 显示方案 | 纯 Linux 方案 |
|------|-------------------------|---------------|
| I2C3（IMX678） | RTOS 或 Linux（互斥） | Linux |
| I2C1（IMU / Touch） | RTOS 独占（防抖/触摸时） | Linux |
| SPI3（JD9853） | RTOS 独占（远期） | Linux |
| MIPI RX / VI / VPSS | RTOS（协同时） | Linux |
| GPIOB11 TE / SPI 控制脚 | RTOS（显示时） | Linux |

**原则：同一外设同一时刻只能一个核驱动。**

Cache：跨核共享 DDR 必须 flush/invalidate（参考 RGN 中 `inv_dcache_range`）。

---

## 9. 实施优先级建议

| 优先级 | 内容 | 收益 |
|--------|------|------|
| P0 | RTOS IMU 采样 + 帧 ID 环缓冲 + Linux 落盘 | 后期高质量防抖数据基础 |
| P1 | VPSS 旋转/warp + EIS 迁 RTOS；Linux 只 blit/编码 | 减轻 A53 实时负担 |
| P2 | Linux 启动优化（KO 顺序、复用 RTOS 已拉起的 VI） | 缩短首帧时间 |
| P3 | RTOS SPI3 + JD9853 + TE + 触摸驱动 | 防撕裂、快启、A53 彻底减负 |

---

## 10. 关键文件索引

| 类别 | 路径 |
|------|------|
| 板级 DTS | `build/boards/cv181x/sg2000_zonhor_sg2000_glibc_arm64_emmc/dts_arm64/` |
| 内存映射 | `build/boards/cv181x/sg2000_zonhor_sg2000_glibc_arm64_emmc/memmap.py` |
| FSBL / C906L 启动 | `fsbl/plat/cv181x/bl2/` |
| FreeRTOS 主任务 | `freertos/cvitek/task/comm/src/riscv64/comm_main.c` |
| rtos_cmdqu 驱动 | `osdrv/interdrv/rtos_cmdqu/` |
| fast_image 驱动 | `osdrv/interdrv/fast_image/` |
| Linux KO 加载 | `device/zonhor-sg2000-glibc-arm64-emmc/overlay/mnt/system/ko/loadsystemko.sh` |
| 相机+LCD 示例 | `cvi_mpi/sample/sensor_lcd/` |
| U-Boot JD9853 | `u-boot-2021.10/cmd/jd9853_logo.c` |
| U-Boot SPI3 pinmux | `build/boards/cv181x/sg2000_zonhor_sg2000_glibc_arm64_emmc/u-boot/cvi_board_init.c` |
| cvi_mpi RTOS 接口 | `cvi_mpi/include/rtos_cmdqu.h` |
| Mailbox 定义 | `freertos/cvitek/common/include/riscv64/cvi_mailbox.h` |

---

## 11. 架构总览图

```
                    ┌─────────────────────────────────────┐
                    │           SG2000 SoC (CV181x)        │
                    │                                      │
   Boot 选择 ──────►│  ┌──────────────┐                   │
   (Zonhor: ARM)    │  │ ARM A53      │  Linux            │
                    │  │ (CPU0)       │  Buildroot/glibc  │
                    │  │              │  cvi_mpi/UI/网络   │
                    │  └──────┬───────┘                   │
                    │         │ Mailbox + ION 共享 DDR     │
                    │  ┌──────▼───────┐                   │
                    │  │ RISC-V C906L │  FreeRTOS CVIRTOS │
                    │  │ (CPU2)       │  VI/ISP/RGN/AUDIO │
                    │  │              │  [远期] SPI显示/触摸│
                    │  └──────────────┘                   │
                    │  ┌──────────────┐                   │
                    │  │ RISC-V C906B │  (未用)           │
                    │  └──────────────┘                   │
                    └─────────────────────────────────────┘
```

---

## 12. 三项功能分工速查表

| 功能 | RTOS 可承担 | Linux 必须承担 | 防卡顿/help |
|------|-------------|----------------|-------------|
| 轻量防抖 | IMU、帧同步、仿射、VPSS/DWA | 配置/UI | ⭐⭐⭐ |
| 快启预览 | T1 sensor/VI/VPSS、共享帧 | fb/SPI（现状）或远期仅写共享缓冲 | ⭐⭐ |
| IMU+帧ID 日志 | 采样、绑定、环缓冲 | 文件落盘、离线算法 | ⭐（数据质量） |
| SPI 显示+触摸（远期） | TE 同步刷屏、触摸 IRQ | UI 合成写 RGB565 缓冲 | ⭐⭐⭐ |

---

*本文档由异构架构调研整理，随板级驱动与 RTOS 移植进展可增量更新。*
