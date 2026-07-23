# SG2000 Zonhor：`lcd_owner=rtos` 屏不刷新踩坑记录

- **日期**：2026-07-20
- **板型**：zonhor-sg2000（SG2000 / CA53 Linux + C906L FreeRTOS）
- **现象**：`lcd_owner=linux` 时 `fb_jd9853` 正常；`zonhor-lcd-check switch-rtos` 并重启后，写 `/dev/fb0` 成功但面板不更新
- **结论**：多因素叠加。最终可工作状态：`rtos_ready=1`，`prog` 心跳变化，`te_sync_cnt` 随 `fill` 递增，`dirty` 可清零

---

## 1. 架构速览

| 模式 | Linux | FreeRTOS (C906L) |
|------|--------|------------------|
| `lcd_owner=linux` | `fb_jd9853` 直接驱 SPI3 | 仅软 PWM 背光（GPIOA20） |
| `lcd_owner=rtos` | `cv181x_zonhor_lcd_proxy.ko` → `/dev/fb0` → SHM + mailbox | 拥有 SPI3 + TE，刷屏 |

关键路径（rtos 模式）：

```
Linux fb0 write
  → zonhor_lcd_proxy 拷贝到 display_shm@0x95300000，置 dirty=1
  → mailbox IP_DISPLAY / DISPLAY_CMD_FLUSH（可选）
  → RTOS Display 任务：inv 帧缓冲 → SPI3 发像素 → 清 dirty，te_sync_cnt++
```

握手：

- Linux：`linux_ready=1`
- RTOS：SPI 就绪后 `rtos_ready=1`
- 进度面包屑：`reserved_mirror`（sysfs 里叫 `prog`）

相关源码：

- RTOS：`freertos/cvitek/task/display/src/display_task.c`、`jd9853_panel.c`、`hal_spi3.c`、`jd9853_bl_pwm.c`
- Linux：`osdrv/extdrv/zonhor_lcd_proxy/`、`zonhor_lcd_bl/`
- 协议头：`osdrv/include/display_shm.h`（两侧必须一致）
- 固件：`cvirtos.bin` 打进 FIP → `/dev/mmcblk0boot0`

---

## 2. 现象时间线（简表）

| 阶段 | status 典型值 | 含义 |
|------|----------------|------|
| 初诊 | `rtos_ready=0`，`dirty=1`，`te_sync_cnt=0`，SPI3 `SSIENR=0` | RTOS 未开 SPI |
| 修 owner / BL 后 | `rtos_ready=1`，`prog=4`，SPI `CTRLR0=0x107` | SPI 起来了，但主循环未刷帧 |
| 心跳前置后 | `prog=20`（0x14）卡住 | 进了主循环一次，随后卡在 flush / 队列 |
| cache + 非阻塞循环后 | `te_sync_cnt≥1`，`prog` 会变，但 fill 常不涨 | 能刷偶发帧，heartbeat 冲掉 dirty |
| 修 false-sharing 后 | fill 后 `te_sync_cnt` 连续 +1，`dirty→0` | **刷新链路打通** |

板端验证（修复后）：

```text
cvi.lcd_owner=rtos
rtos_ready=1，prog 心跳变化
zonhor-lcd-check fill ×3 → te_sync_cnt 连续递增，dirty 清零
```

---

## 3. 踩坑清单（按杀伤力）

### 坑 A：软重启残留 `display_shm.owner=LINUX`

- **机制**：FSBL 很早拉起 C906L，U-Boot logo 才写 SHM。软重启 DRAM 里常残留上一会话的 `owner=LINUX` / `linux_ready=1`。
- **后果**：RTOS 早早锁成「仅背光」路径，永不 `open_spi`。
- **错误修法**：RTOS 里盲目 `magic=0` 清残留 → 与 U-Boot `jd9853_init_display_shm()` 竞态，Linux 报 `display_shm magic missing`。
- **正修**：`display_resolve_owner()` 迟到切换；**不要**在 wait_shm 里清 magic。

### 坑 B：背光软 PWM 忙等饿死小核

- **机制**：中等亮度用 `udelay` 位bang PWM，单核上饿死 stats / mailbox 消费者。
- **正修**：`jd9853_bl_pwm.c` 用 `vTaskDelay` 做粗粒度周期（约 10 ms）；SHM `bl_level` 也可作为 mailbox 失败时的兜底。

### 坑 C：TE / SPI 超长 busy-wait 楔死 Display

- **机制**：`jd9853_wait_te()` 的 `udelay` 依赖 `GetSysTime()`；若 timer 停滞则死循环。`hal_spi3_xfer` 无界等待同样危险。
- **正修**：TE 改为非阻塞 peek（先保证像素路径活着）；SPI FIFO 轮询加 guard；长传里偶发 `taskYIELD()`。

### 坑 D：Display 主循环阻塞在 `xQueueReceive(timeout)` / `vTaskDelay`

- **症状**：`prog` 只跳一次（如 `0x14`）后永不变；SPI 寄存器空闲。
- **机制**：若 SysTick 异常，带 timeout 的队列接收 / `vTaskDelay` 可能永久 park。
- **正修**：队列用 `xQueueReceive(..., 0)` 抽干；节奏用 `arch_usleep` + `taskYIELD()`，不要依赖 tick 才能回到 flush。

### 坑 E：SHM header **伪共享**（最终关键 bug）

`struct display_shm` 头部约 32 字节，落在 **同一 64B cache line**（含 `dirty` 与 `reserved_mirror`）。

错误模式：

```c
// heartbeat：未 inv，本地 cache 里 dirty 仍是 0
g_shm->reserved_mirror = hb;
clean_dcache_range(g_shm, 64);  // 整行写回 → 冲掉 Linux 刚写的 dirty=1
```

- **症状**：偶发 `te_sync_cnt++`，但 `zonhor-lcd-check fill` 后计数不涨 / `dirty` 粘住；心跳 `prog` 仍在变。
- **正修**：任何「只改 reserved_mirror」的路径必须先 `inv` 再改再 `clean`（`display_set_prog_raw()`）。

### 坑 F：C906 `CACHE_OP_RANGE` 写法不安全

旧实现用 `register ... asm("a0")` 跨循环，优化后 a0 不一定每轮更新。

- **正修**：`freertos/cvitek/arch/riscv64/src/cache.c` 每轮显式把地址绑进 a0，并加 `"memory"` clobber。
- Header 发布优先 `clean_dcache_range`，慎用 `flush`(cipa) 与 Linux 抢同一行。

### 坑 G：工程 / 部署类

| 坑 | 说明 |
|----|------|
| 模块工具链 | proxy/bl `.ko` 需 Linaro **7.3** 对上内核，勿用 6.3 |
| FIP 刷写 | `dd` 到 `mmcblk0boot0`；`md5sum` 的 `count` 必须等于写入扇区数 |
| SSH | 本环境 `ssh root@192.168.42.1` **免密**；用 expect 输密码反而易卡死并行会话 |
| 软重启 | 本平台软 `reboot` 会重载 FIP 中的 cvirtos；可用 DRAM `0x9FE00000` 与 `cvirtos.bin` 比对确认 |
| U-Boot logo SHM | logo 失败时也应 publish SHM（源码已改，若未重编进 FIP 则仍靠 proxy 初始化） |

---

## 4. 诊断面包屑（`prog` / `reserved_mirror`）

| 值 | 含义 |
|----|------|
| 1–3 | wait_shm / wait_linux / open_spi |
| 4 | `DISP_PROG_SPI_OK`（低半字节）；高半字节为心跳 |
| `0xA0`–`0xA5` | flush 阶段（进入 → inv → orient → addr_win → pixels） |
| `0xE1` / `0xE2` / `0xE3` | orientation / addr_win / pixel xfer 失败 |

读状态：

```bash
cat /sys/devices/platform/zonhor-lcd-proxy/status
# dirty / te_sync_cnt / prog / rtos_ready
busybox devmem 0x041B0008   # SSIENR，期望 1
busybox devmem 0x041B0000   # CTRLR0，期望 0x107
```

---

## 5. 构建与刷写

```bash
source build/envsetup_soc.sh
defconfig sg2000_zonhor_sg2000_glibc_arm64_emmc

cd freertos/cvitek && ./build_cv181x.sh

# 打 FIP（BLCP = cvirtos，LOADER = u-boot-raw）
make -C $FSBL_PATH O=$FSBL_PATH/build/${PROJECT_FULLNAME} \
  LOG_LEVEL=2 \
  BLCP_2ND_PATH=$FREERTOS_PATH/cvitek/install/bin/cvirtos.bin \
  LOADER_2ND_PATH=$TOP_DIR/u-boot-2021.10/build/${PROJECT_FULLNAME}/u-boot-raw.bin \
  RTOS_ENABLE_FREERTOS=y

scp fsbl/build/${PROJECT_FULLNAME}/fip.bin root@192.168.42.1:/tmp/fip.bin
ssh root@192.168.42.1 '
  echo 0 > /sys/block/mmcblk0boot0/force_ro
  dd if=/tmp/fip.bin of=/dev/mmcblk0boot0 bs=512 conv=fsync
  echo 1 > /sys/block/mmcblk0boot0/force_ro
  sync; reboot
'
```

内核模块（若改了 proxy/bl）：

```bash
# KERNEL_DIR=linux_5.10/build/sg2000_zonhor_...
# PATH=.../gcc-linaro-7.3.1-.../bin
# 产物拷到板上 /mnt/system/ko/
```

切换 owner：

```bash
zonhor-lcd-check          # 看当前模式
zonhor-lcd-check switch-rtos   # 改 env + cmdline，需 reboot
zonhor-lcd-check fill          # 写蓝屏测刷新
```

---

## 6. 改动文件一览（本次）

| 文件 | 改动要点 |
|------|----------|
| `freertos/.../display_task.c` | late owner；心跳/非阻塞队列；prog 面包屑；header inv→clean；flush 分阶段 |
| `freertos/.../jd9853_panel.c` | TE 非阻塞；全量 panel init |
| `freertos/.../hal_spi3.c` | xfer 超时 guard；周期性 yield |
| `freertos/.../jd9853_bl_pwm.c` | 可让出的软 PWM |
| `freertos/.../arch/riscv64/src/cache.c` | 安全的 `CACHE_OP_RANGE` |
| `osdrv/.../zonhor_lcd_proxy.c` | 偏好 `MEMREMAP_WT`；status 显示 prog/bl |
| `osdrv/.../zonhor_lcd_bl.c` | 同步写 SHM `bl_level` |
| `u-boot-2021.10/cmd/jd9853_logo.c` | logo 失败仍 publish SHM（需编进 FIP 才生效） |

---

## 7. 经验法则（异构 SHM）

1. **同一 cache line 上不要「只改半边再 clean」**——先 inv 合并对端写入，再改己方字段，再 clean。
2. RTOS 实时任务里 **少用无界 udelay / 无界轮询**；更不要在可能丢 tick 时 `vTaskDelay` 当唯一节奏源。
3. 软重启 + 双核早起启动 = **默认不信任 DRAM 握手残留**，但清 magic 比残留更危险时，用迟到 resolve。
4. 诊断优先看：`rtos_ready` → SPI 寄存器 → `prog` 是否心跳 → `te_sync_cnt` 是否随 fill 涨。
5. 刷 FIP 后用 **boot0 md5 == 本地 fip**（`count` 对齐）+ DRAM `0x9FE00000` 与 `cvirtos.bin` 比对，确认跑的是新固件。

---

## 8. 仍可选的后续

- 正式重编 U-Boot 进 FIP，保证 logo 路径始终 publish SHM。
- 将 `dirty` / `reserved_mirror` **拆到不同 cache line**（改 `display_shm` version），从结构上消灭伪共享。
- TE 对齐在像素路径稳定后，用短超时 + yield 的版本加回。
- mailbox「No valid mailbox」若仍偶发，可继续依赖 SHM `dirty` / `bl_level` 轮询兜底（当前 Display 主循环已 poll dirty）。
