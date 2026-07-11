# Zonhor SG2000 eMMC A/B OTA 更新说明

## 概述

本板使用 **最小 A/B 分区**：双 BOOT + 双 ROOTFS，OTA 包暂存在 **DATA 分区**（`/dev/mmcblk0p7`，挂载 `/mnt/data`）。  
设备启动早期由 `S04data` 挂载 DATA、`S05ota` 检测待升级标记并刷写 **非活动槽**，然后切换 `boot_slot` 重启。

> **从旧版（单 ROOTFS + OTA recovery 分区）迁移**：必须 **USB 整盘烧录一次（含 fip.bin）**。  
> 新布局与旧 p5 OTA 方案 **不兼容**，无法在线迁移。

## 分区布局

| 分区 | 设备节点 | 说明 |
|------|----------|------|
| BOOT | mmcblk0p1 | Slot A：kernel + dtb |
| BOOT_B | mmcblk0p2 | Slot B |
| MISC | mmcblk0p3 | logo |
| ENV | （偏移） | U-Boot 环境，含 `boot_slot` |
| ROOTFS | mmcblk0p5 | Slot A 根文件系统 |
| ROOTFS_B | mmcblk0p6 | Slot B 根文件系统 |
| DATA | mmcblk0p7 | 用户数据 + OTA 暂存，挂载 `/mnt/data` |

出厂 USB 烧录后默认 `boot_slot=a`。两槽写入相同镜像。

## USB 工厂 / 救砖烧录

```bash
./build.sh zonhor-sg2000-glibc-arm64-emmc
cd install/soc_sg2000_zonhor_sg2000_glibc_arm64_emmc
# 必须包含 fip.bin、boot.emmc、boot_b.emmc、rootfs_ext4.emmc、rootfs_b_ext4.emmc
tools/usb_dl/usb_dl -c 181x -s <os> -i .
```

烧录后检查：

```bash
lsblk
fw_printenv boot_slot    # 应为 a
zonhor-ab-slot show
```

手动验证 B 槽（内容与 A 相同）：

```bash
fw_setenv boot_slot b
sync && reboot
```

## USER_LED 升级指示

| 模式 | 频率 | 含义 |
|------|------|------|
| staged | 1 Hz（慢闪） | 已 `zonhor-ota-stage`，等待重启 |
| fip | 2 Hz | 正在刷写 bootloader（`zonhor-ota-migrate-fip`） |
| phase1 | 4 Hz | 正在刷写非活动槽 BOOT/ROOTFS |
| success | 常亮 3 秒 | 升级完成，即将重启 |
| error | 10 Hz（极快） | 升级失败 |

## 现场 OTA 升级步骤

### 1. 在 PC 上构建 OTA 包

```bash
./build.sh zonhor-sg2000-glibc-arm64-emmc
```

产物：`out/zonhor-sg2000-glibc-arm64-emmc_<时间戳>.zip`（`upgrade.zip` 格式，内含单份 boot/rootfs）。

### 2. 上传到设备并触发

```bash
scp out/zonhor-sg2000-glibc-arm64-emmc_*.zip root@<设备IP>:/mnt/data/upgrade.zip
ssh root@<设备IP> "zonhor-ota-stage && sync && reboot"
```

或设备本地：

```bash
zonhor-ota-stage /path/to/upgrade.zip
sync && reboot
```

### 3. 自动刷写流程

重启后 `S05ota` → `zonhor-ota-apply`：

1. 校验 `upgrade.zip`（MD5、板型）
2. 解析当前活动槽，确定 **非活动槽**
3. 将 boot + rootfs 写入非活动槽（**永不写入当前活动 ROOTFS**）
4. 更新 `boot_slot` 与 priority 等预留键
5. 清除 pending，reboot

断电安全：刷写完成并切换 `boot_slot` 前断电，仍从旧槽启动。

## 槽位命令

```bash
zonhor-ab-slot show
zonhor-ab-slot get-active
zonhor-ab-slot get-inactive
zonhor-ab-slot set-boot-slot a|b
```

## 相关文件

| 路径 | 作用 |
|------|------|
| `/mnt/data/upgrade.zip` | 待刷写 OTA 包 |
| `/mnt/data/upgrade.pending` | 升级触发标记 |
| `/mnt/data/upgrade.log` | 最近一次升级日志 |
| `/mnt/data/upgrade.failed` | 升级失败标记 |
| `/mnt/data/upgrade.last.zip` | 上次成功升级的包备份 |

## 故障排查

| 现象 | 处理 |
|------|------|
| 分区不是 7 个 | 需 USB 整刷含 fip 的新固件 |
| `/mnt/data` 未挂载 | 检查 `S04data`；确认 `mmcblk0p7` 存在 |
| 重启后未升级 | 确认 `upgrade.pending` 存在；重新 `zonhor-ota-stage` |
| 校验失败 | 重新上传完整 zip |
| `upgrade.failed` 存在 | 查看 `/mnt/data/upgrade.log`，修复后删除 failed 再试 |
| 升级中断电 | 若 `boot_slot` 未切换，旧槽仍可用；可重新 stage |
| 需仅救砖 FIP | `zonhor-ota-migrate-fip`（布局迁移仍须 USB 整刷） |

## 限制（Phase 1）

- 不自动 tries 回滚（键已预留，Phase 2 启用）
- 不支持在线升级 U-Boot/FIP
- 不支持与旧 p5 OTA 布局共存

## 开发侧源码位置

- 分区表：`build/boards/cv181x/sg2000_zonhor_sg2000_glibc_arm64_emmc/partition/partition_emmc.xml`
- U-Boot 选槽：`u-boot-2021.10/cmd/cvi_boot_mode_emmc.c`
- 板级脚本：`device/zonhor-sg2000-glibc-arm64-emmc/overlay/`
- 重构计划：`AB-PARTITION-REFACTOR-PLAN.md`
