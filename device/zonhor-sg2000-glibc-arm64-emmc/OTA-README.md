# Zonhor SG2000 eMMC OTA 更新说明

## 概述

本板新增独立 **OTA 分区** (`/dev/mmcblk0p5`，挂载 `/mnt/ota`)，用于存放升级包。  
设备启动早期由 `S05ota` 检测待升级标记并刷写，无需再进入 USB 下载模式。

> **首次启用**：需用 USB 完整烧录一次带 OTA 分区的新固件。  
> **重要**：USB 下载默认可能**不写 fip.bin**，导致系统仍只有 4 个分区。若 `lsblk` 看不到 `mmcblk0p5`，请先执行 `zonhor-ota-migrate-fip`。

## 分区布局

| 分区 | 设备节点 | 说明 |
|------|----------|------|
| BOOT | mmcblk0p1 | kernel + dtb |
| MISC | mmcblk0p2 | logo |
| ENV  | mmcblk0p3 | U-Boot 环境 |
| ROOTFS | mmcblk0p4 | 根文件系统 ext4 |
| OTA | mmcblk0p5 | 升级暂存 ext4，挂载 `/mnt/ota` |

默认 **不刷写 `fip.bin`**（bootloader），降低变砖风险。

## 首次迁移：启用 OTA 分区 (mmcblk0p5)

分区由 U-Boot 的 `blkdevparts` 定义，不是 GPT。仅更新 rootfs 不会让 `mmcblk0p5` 出现。

检查：

```bash
lsblk    # 应看到 mmcblk0p5
```

若没有 p5，但 `/mnt/ota/upgrade.zip` 已存在：

```bash
# 1. 从 upgrade.zip 刷写 fip.bin 到 boot 区
zonhor-ota-migrate-fip

# 2. 自动重启后确认
lsblk    # 现在有 mmcblk0p5

# 3. 标记升级并重启
zonhor-ota-stage
sync && reboot
```

也可手动从 zip 提取 fip 并写入（无新脚本时的应急方案）：

```bash
mkdir -p /tmp/fip && unzip -j /mnt/ota/upgrade.zip fip.bin -d /tmp/fip
echo 0 > /sys/block/mmcblk0boot0/force_ro
echo 0 > /sys/block/mmcblk0boot1/force_ro
dd if=/tmp/fip/fip.bin of=/dev/mmcblk0boot0 bs=512 conv=fsync
dd if=/tmp/fip/fip.bin of=/dev/mmcblk0boot1 bs=512 conv=fsync
sync && reboot
```

## USER_LED 升级指示

`user-led`（USER_LED）在 OTA 各阶段以不同频率闪烁：

| 模式 | 频率 | 含义 |
|------|------|------|
| staged | 1 Hz（慢闪） | 已 `zonhor-ota-stage`，等待重启 |
| fip | 2 Hz | 正在刷写 bootloader（`zonhor-ota-migrate-fip`） |
| phase1 | 4 Hz | 阶段 1：校验/刷写 BOOT、MISC |
| recovery | 8 Hz（快闪） | 阶段 2：正在刷写 ROOTFS，**勿断电** |
| success | 常亮 3 秒 | 升级完成，即将重启 |
| error | 10 Hz（极快） | 升级失败 |

手动测试：`zonhor-ota-led.sh start phase1` / `zonhor-ota-led.sh stop`

## 升级步骤

### 1. 在 PC 上构建 OTA 包

```bash
./build.sh zonhor-sg2000-glibc-arm64-emmc
```

产物：`out/zonhor-sg2000-glibc-arm64-emmc_<时间戳>.zip`（即 `upgrade.zip` 格式）。

### 2. 上传到设备

任选一种方式将 zip 放到设备：

**SFTP/SCP（推荐）**

```bash
scp out/zonhor-sg2000-glibc-arm64-emmc_*.zip root@<设备IP>:/mnt/ota/upgrade.zip
ssh root@<设备IP> "zonhor-ota-stage && sync && reboot"
```

**已在设备上的文件**

```bash
zonhor-ota-stage /path/to/upgrade.zip
sync && reboot
```

**TF 卡 / USB 存储**

```bash
mount /dev/mmcblk1p1 /mnt
cp /mnt/upgrade.zip /mnt/ota/upgrade.zip
zonhor-ota-stage
sync && reboot
```

### 3. 自动刷写

重启后 `S05ota` 会分两阶段执行：

1. **阶段 1（从 ROOTFS 启动）**：校验 zip，仅刷写 BOOT/MISC，将 rootfs 镜像解压到 OTA 分区，然后以 `root=/dev/mmcblk0p5 init=/recovery-init.sh` 重启。
2. **阶段 2（从 OTA 分区启动）**：在独立 recovery 环境中将 ROOTFS 写入 `mmcblk0p4`，恢复 U-Boot 启动参数后再次重启。

> **切勿**在正常运行 Linux 时直接 `dd` ROOTFS 分区，否则会导致文件系统损坏（你遇到的 `ext4 checksum invalid`）。

## 相关文件

| 路径 | 作用 |
|------|------|
| `/mnt/ota/upgrade.zip` | 待刷写 OTA 包 |
| `/mnt/ota/upgrade.pending` | 升级触发标记（`zonhor-ota-stage` 创建） |
| `/mnt/ota/upgrade.log` | 最近一次升级日志 |
| `/mnt/ota/upgrade.failed` | 升级失败标记 |
| `/mnt/ota/upgrade.last.zip` | 上次成功升级的包备份 |

## 命令参考

```bash
# 一次性：从 upgrade.zip 刷 fip，启用 p5
zonhor-ota-migrate-fip

# 标记待升级（包已在 /mnt/ota/upgrade.zip）
zonhor-ota-stage

# 从任意路径复制并标记
zonhor-ota-stage /tmp/upgrade.zip

# 查看升级日志
cat /mnt/ota/upgrade.log
```

## 故障排查

| 现象 | 处理 |
|------|------|
| `mmcblk0p5` 不存在 | USB 未刷 fip.bin；运行 `zonhor-ota-migrate-fip` 后重启 |
| `/mnt/ota` 存在但无 p5 | 当前是 rootfs 目录，不是独立分区；先 migrate-fip |
| `/mnt/ota` 不存在 | 确认已烧录含 OTA 分区的固件；检查 `ls /dev/mmcblk0p5` |
| 重启后未升级 | 确认存在 `upgrade.pending`；运行 `zonhor-ota-stage` 后 reboot |
| 校验失败 | 重新上传完整 zip，检查传输是否损坏 |
| `upgrade.failed` 存在 | 查看 `/mnt/ota/upgrade.log`，修复后删除 failed 标记再试 |
| 升级中断电 | 阶段 2 刷 ROOTFS 时勿断电；若 rootfs 已损坏需 USB 重新烧录 |
| `ext4 checksum invalid` / `Segmentation fault` | 旧版 OTA 在运行中覆盖了 ROOTFS；需 USB 重刷，并使用新版两阶段 OTA |

## 限制

- 非 A/B 分区，rootfs 刷写期间请勿断电。
- 不支持在线升级 U-Boot/FIP（`fip.bin`）。
- `ramboot/recovery` 未启用（`CONFIG_SKIP_RAMDISK=y`），采用早期 init 刷写方案。

## 开发侧源码位置

- 分区表：`build/boards/cv181x/sg2000_zonhor_sg2000_glibc_arm64_emmc/partition/partition_emmc.xml`
- 板级脚本：`device/zonhor-sg2000-glibc-arm64-emmc/overlay/`
