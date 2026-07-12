# Zonhor SG2000 eMMC A/B OTA 更新说明

## 概述

本板使用 **最小 A/B 分区**：双 BOOT + 双 ROOTFS，OTA 包暂存在 **DATA 分区**（`/dev/mmcblk0p7`，挂载 `/mnt/data`）。  
`zonhor-ota-stage` 在现系统上完成解压/校验/转换；重启后 `S05ota` 检测 pending 并只刷写 **非活动槽**，再切换 `boot_slot` 重启。

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

## 屏幕 UI（JD9853 `/dev/fb0`）

刷写期间 `zonhor-ota-apply` 会启动 `zonhor-ota-ui.py`，在 172x320 竖屏上显示：

- 标题 `OTA UPDATE`
- 槽位 `A -> B`（当前活动 → 目标）
- 阶段：`CHECK` / `UNPACK` / `VERIFY` / `CONVERT` / `FLASH BOOT` / `FLASH ROOT` / …
- 总进度条与百分比、预估剩余时间 `ETA`
- 成功：`UPDATE OK` + `REBOOTING...`（约 3 秒后重启）
- 失败：`FAILED` + 短错误信息

重启后刷写阶段（`zonhor-ota-apply` / `zonhor-ota-flash.py`）还会向**串口控制台**和 **`/dev/kmsg`（dmesg）** 输出进度，慢步骤带实时/平均速度，例如：

```text
zonhor-ota: FLASH_ROOT  512.0/1500.0 MiB ( 34%) cur=18.2 MB/s avg=15.1 MB/s eta=65s
```

`zonhor-ota-stage` 成功后也会短暂显示 `STAGED` / `REBOOT TO APPLY`。  
无 `/dev/fb0` 或 python3 时 UI 自动跳过，不影响刷写。

状态文件：`/run/zonhor-ota.status`（`key=value`）。

## 现场 OTA 升级步骤

### 1. 在 PC 上构建 OTA 包

```bash
./build.sh zonhor-sg2000-glibc-arm64-emmc
```

产物：`out/zonhor-sg2000-glibc-arm64-emmc_<时间戳>.zip`（`upgrade.zip` 格式，内含**单份** boot/rootfs，不含 BOOT_B/ROOTFS_B）。  
OTA 包内的 CIMG 会在打包阶段把全零 chunk 转成 `DONT_CARE` sparse chunk；USB 工厂烧录使用的 install 镜像保持原始 CIMG 格式不变。

### 2. 上传到设备并触发

```bash
scp out/zonhor-sg2000-glibc-arm64-emmc_*.zip root@<设备IP>:/mnt/data/upgrade.zip
ssh root@<设备IP> "zonhor-ota-stage && sync && reboot"
```

或设备本地：

```bash
# 包已在 /mnt/data/upgrade.zip 时不要再带同路径参数
zonhor-ota-stage
# 或从其他路径拷入：
# zonhor-ota-stage /path/to/upgrade.zip
sync && reboot
```

`zonhor-ota-stage` 在**当前系统仍运行时**完成：解压 zip、MD5 校验、保留 CIMG/sparse 镜像（写入 `/mnt/data/work`），然后才打 `upgrade.pending`。  
耗时主要花在 stage，不占用重启后的刷写窗口。

### 3. 自动刷写流程

重启后 `S05ota` → `zonhor-ota-apply`：

1. 检测 `upgrade.pending`；优先使用已准备好的 `/mnt/data/work`
2. （兼容）若 work 未就绪但 zip 仍在，则现场再解压并准备 work
3. 将 boot + rootfs 写入非活动槽（**永不写入当前活动 ROOTFS**）；CIMG/sparse rootfs 会按 chunk 直刷，全零块用 zeroout/discard/写零兜底处理
4. 更新 `boot_slot` 与 priority 等预留键
5. 清除 pending / work，reboot

断电安全：刷写完成并切换 `boot_slot` 前断电，仍从旧槽启动。  
stage 阶段断电不影响活动槽；清掉半成品后重新 `zonhor-ota-stage` 即可。

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
| `/mnt/data/work/` | stage 预解压后的 boot、rootfs（优先保留 CIMG/sparse，兼容 raw） |
| `/mnt/data/upgrade.pending` | 升级触发标记（prepare 成功后才创建） |
| `/mnt/data/upgrade.log` | 最近一次升级日志（含 stage prepare） |
| `/mnt/data/upgrade.failed` | 升级失败标记 |
| `/mnt/data/upgrade.last.zip` | 上次成功升级的包备份 |
| `/run/zonhor-ota.status` | 屏幕 UI 进度状态 |
| `/usr/sbin/zonhor-ota-ui.py` | framebuffer 进度界面 |
| `/usr/sbin/zonhor-ota-flash.py` | 带进度的分块刷写 |

## 故障排查

| 现象 | 处理 |
|------|------|
| 分区不是 7 个 | 需 USB 整刷含 fip 的新固件 |
| `/mnt/data` 未挂载 | 检查 `S04data`；确认 `mmcblk0p7` 存在 |
| `cp: ... are the same file` | 包已在 `/mnt/data/upgrade.zip`，直接 `zonhor-ota-stage`（无参数） |
| 重启后未升级 | 确认 `upgrade.pending` 存在；重新 `zonhor-ota-stage` |
| `cannot unpack rootfs image` | 多为 DATA 空间不足或 CIMG 写到 `/tmp`；清 `rm -rf /mnt/data/work`，同步修复后的 `zonhor-ota-*` 脚本再试 |
| `rootfs image failed ext4 magic check` | rootfs 镜像损坏或 CIMG/sparse 格式异常；重新生成并上传完整 OTA 包 |
| `JBD2: no valid journal` / 无法挂载 ROOTFS_B | 同上，inactive 槽 rootfs 被写坏；先 `fw_setenv boot_slot a` 回 A 槽，再 OTA |
| 软重启停在 `Starting kernel` | 多为大块 eMMC 写后 warm reset；断电冷启可继续；脚本已改为 `sync`+`blockdev --flushbufs`+普通 `reboot` |
| 校验失败 | 重新上传完整 zip |
| `upgrade.failed` 存在 | 查看 `/mnt/data/upgrade.log`，修复后删除 failed 再试 |
| 升级中断电 | 若 `boot_slot` 未切换，旧槽仍可用；可重新 stage |
| 需仅救砖 FIP | `zonhor-ota-migrate-fip`（布局迁移仍须 USB 整刷） |

## 限制（Phase 1）

- 不自动 tries 回滚（键已预留，Phase 2 启用）
- 不支持在线升级 U-Boot/FIP
- 不支持与旧 p5 OTA 布局共存
- USB 工厂烧录暂不启用 OTA sparse 包；当前 U-Boot USB 写入路径未确认完整支持 `DONT_CARE` chunk，install 镜像仍保持兼容 CIMG

## 开发侧源码位置

- 分区表：`build/boards/cv181x/sg2000_zonhor_sg2000_glibc_arm64_emmc/partition/partition_emmc.xml`
- U-Boot 选槽：`u-boot-2021.10/cmd/cvi_boot_mode_emmc.c`
- 板级脚本：`device/zonhor-sg2000-glibc-arm64-emmc/overlay/`
- 重构计划：`AB-PARTITION-REFACTOR-PLAN.md`
