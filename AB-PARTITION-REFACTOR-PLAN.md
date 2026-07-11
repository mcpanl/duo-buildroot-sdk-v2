# Zonhor eMMC 最小 A/B 分区重构计划

> 目标板：`zonhor-sg2000-glibc-arm64-emmc`  
> 原则：**先做最小可用 A/B**，接口与分区预留扩展位，后续可演进到带 tries/回滚/签名的专业方案。  
> 双路径：**现场「放固件 → 重启 → 更新」** + **工厂/救砖「USB 直接烧录最新固件」**。

---

## 1. 背景与问题

### 1.1 现状

当前方案为 **单 ROOTFS + 独立 OTA 暂存分区（p5）两阶段刷写**，非真 A/B：

| 分区 | 设备 | 作用 |
|------|------|------|
| BOOT | mmcblk0p1 | kernel + dtb |
| MISC | mmcblk0p2 | logo |
| ENV | mmcblk0p3 | U-Boot 环境 |
| ROOTFS | mmcblk0p4 | 唯一根文件系统 |
| OTA | mmcblk0p5 | 升级包暂存 + 临时 recovery root |

自定义运行时约 8 个脚本 / ~750 行（`zonhor-ota-*`、`S05ota`）。

### 1.2 主要痛点

- 刷 ROOTFS 窗口仍怕断电；失败后常需 USB 整盘救砖。
- 依赖 `fw_setenv` 切到 p5 做 recovery，边界多（fip 未刷则无 p5、env 写坏等）。
- SDK 虽有 SPI-NAND A/B 参考（`partition_spinand_page_2k_ab.xml`、`cvi_boot_mode_nand.c`），但 **eMMC + Buildroot 路径明确未支持**（`build/Makefile` TODO）。

### 1.3 本计划要达成的结果

| 能力 | 最小版必须 | 后续可扩展 |
|------|------------|------------|
| 双 BOOT + 双 ROOTFS | ✅ | — |
| 运行中只写「非活动槽」 | ✅ | — |
| 放包 → 重启 → 自动切槽启动 | ✅ | — |
| USB `usb_dl` 整盘烧录最新固件 | ✅ | — |
| 槽位元数据（slot / priority） | ✅ 简化版 | 完整 AvbAB |
| tries_remaining / successful_boot 自动回滚 | 预留接口 | Phase 2 |
| 在线升 FIP、差分包、签名校验 | 不做 | Phase 3+ |

---

## 2. 设计原则

1. **最小可用优先**：先保证「写对面槽 → 改启动槽 → 重启」稳定，不做完整 Android A/B。
2. **扩展点显式预留**：槽位状态用统一结构/env 键名，后续加 tries/回滚不改分区布局。
3. **USB 与 OTA 共用同一分区表**：`usb_dl` 仍读 `partition.xml`；工厂烧录两槽写相同镜像即可。
4. **废弃 p5 recovery 路径**：不再用 OTA 分区当临时 root；暂存区缩小为 DATA（可选）。
5. **不改闭源 `usb_dl` 二进制**：只改 XML、镜像命名与打包。

---

## 3. 目标分区布局（最小 A/B）

### 3.1 建议布局

假设 eMMC 用户区约 4GB 量级（按现板容量再实测微调）：

| # | Label | 建议大小 | file（USB/打包） | 说明 |
|---|-------|----------|------------------|------|
| p1 | BOOT | 8 MB | `boot.emmc` | Slot A：kernel + dtb |
| p2 | BOOT_B | 8 MB | `boot_b.emmc` | Slot B |
| p3 | MISC | 2 MB | `logo.jpg` | logo；**预留**后续可放 slot metadata 备份 |
| p4 | ENV | 128 KB | （空） | U-Boot env（槽位状态先放这里） |
| p5 | ROOTFS | ~1.5 GB | `rootfs_ext4.emmc` | Slot A |
| p6 | ROOTFS_B | ~1.5 GB | `rootfs_b_ext4.emmc` | Slot B |
| p7 | DATA | 剩余 | （空） | 用户数据 + **OTA 包暂存**（挂载 `/mnt/data`） |

对比现状：

- **删除** 1GB 的 `OTA` 专用分区（不再做 recovery root）。
- **新增** `BOOT_B`、`ROOTFS_B`。
- **DATA** 承担「先放固件」的暂存职责（`/mnt/data/upgrade.zip`）。

> 容量以实测 `lsblk` / 芯片规格为准；原则是 **A/B rootfs 等大**，DATA 吃剩余。

### 3.2 槽位约定

| 槽 | BOOT 分区 | ROOTFS 分区 | 设备节点（示意） |
|----|-----------|-------------|------------------|
| A | BOOT | ROOTFS | p1 + p5 |
| B | BOOT_B | ROOTFS_B | p2 + p6 |

**不变量**：BOOT 与 ROOTFS **必须同槽切换**，禁止只切其一。

### 3.3 槽位状态（最小 + 预留）

存放位置（Phase 1）：U-Boot ENV（`fw_setenv` / `fw_printenv`）。

| 键名 | Phase 1 | Phase 2+ |
|------|---------|----------|
| `boot_slot` | `a` / `b`（当前启动槽） | 同左 |
| `slot_a_priority` | 固定 `15` / `14` 二选一即可 | AvbAB 风格优先级 |
| `slot_b_priority` | 同上 | 同上 |
| `slot_a_tries` | **写入但暂不消费**（默认 0 或 7） | 启动失败递减并回滚 |
| `slot_b_tries` | 同上 | 同上 |
| `slot_a_successful` | **写入但暂不消费**（`1`） | 新槽首次成功启动后置 1 |
| `slot_b_successful` | 同上 | 同上 |

Phase 1 行为：OTA 成功写完对面槽后，只改 `boot_slot` + 双方 priority，然后 reboot。  
Phase 2 再启用 tries / successful 自动回滚，**无需改分区 XML**。

可选后续：把上述字段迁到 MISC 固定偏移的 binary metadata（兼容 `cvi_boot_mode_nand.c` / Android AB），ENV 仅作缓存。

---

## 4. 两条更新路径

### 4.1 路径 A：先放固件，重启后更新（现场 OTA）

```text
用户/脚本
  → 将 upgrade.zip 放到 /mnt/data/upgrade.zip
  → zonhor-ota-stage（写 upgrade.pending）
  → reboot
  → S05ota / ota-apply
       1. 校验 zip（MD5 / 板型）
       2. 解析当前 boot_slot
       3. 将 boot + rootfs 写入「非活动槽」
       4. 更新 boot_slot / priority（预写 tries/successful 字段）
       5. 清 pending，reboot
  → U-Boot 按 boot_slot 加载对应 BOOT，root= 对应 ROOTFS
  → 新系统起来（Phase 2 再 mark-successful）
```

要点：

- **永远不 dd 当前正在运行的 ROOTFS**。
- 不再切到 p5 recovery；阶段 2 整段删除。
- 包格式尽量沿用现有 `upgrade.zip`（`boot.emmc`、`rootfs_ext4.emmc`、META），应用时映射到「非活动槽」对应设备节点。
- FIP/bootloader：**Phase 1 默认不 OTA 升级**（与现策略一致，降变砖风险）。

### 4.2 路径 B：USB 直接烧录最新固件（工厂 / 救砖）

```text
PC: build_all → OUTPUT_DIR 含
      fip.bin
      boot.emmc / boot_b.emmc      （内容相同）
      rootfs_ext4.emmc / rootfs_b_ext4.emmc  （内容相同）
      logo.jpg
      partition_emmc.xml
      tools/usb_dl/...

PC: usb_dl -c 181x -s <os> -i $OUTPUT_DIR
  → 刷 fip → 按 XML 刷各分区（两槽同镜像）
  → 出厂默认 boot_slot=a
```

要点：

- **不修改** `build/tools/common/usb_dl` 闭源工具。
- XML 中 A/B 分区都带 `file=`，工具会按序刷完。
- 构建时 `boot_b` / `rootfs_b` 由 A 槽镜像 **复制再 raw2cimg**（与 SDK NAND `AB_SYSTEM` 思路一致）。
- 文档明确：USB 烧录必须包含 `fip.bin`，否则 `blkdevparts` 不更新。

### 4.3 两路径关系

| | OTA（路径 A） | USB（路径 B） |
|--|--------------|---------------|
| 分区表 | 同一份 XML | 同一份 XML |
| 镜像格式 | CIMG / upgrade.zip | CIMG + fip |
| 写哪些槽 | 只写非活动槽 | 两槽都写（同内容） |
| 槽位状态 | 运行时更新 ENV | 烧录后默认 slot A |
| 适用场景 | 现场升级 | 产线 / 救砖 / 布局迁移 |

---

## 5. 软件架构与扩展点

### 5.1 分层

```text
┌──────────────────────────────────────────┐
│ 用户入口：zonhor-ota-stage / USB 烧录说明 │
├──────────────────────────────────────────┤
│ OTA Apply：校验、选槽、刷写、切槽         │  ← Phase 1 重写
├──────────────────────────────────────────┤
│ Slot API（shell 或小工具）                │  ← 扩展核心
│   get_active / get_inactive               │
│   flash_boot / flash_rootfs               │
│   set_boot_slot / mark_successful         │
│   (Phase2) dec_tries / rollback           │
├──────────────────────────────────────────┤
│ U-Boot：emmc 按 boot_slot 选 BOOT/ROOTFS  │  ← 新增 eMMC 逻辑
├──────────────────────────────────────────┤
│ 分区 XML + mkcvipart + 构建打包           │
└──────────────────────────────────────────┘
```

### 5.2 建议目录（设备侧）

替换现有 `zonhor-ota-*` 散落脚本，收敛为：

```text
device/zonhor-sg2000-glibc-arm64-emmc/overlay/
  etc/init.d/S05ota                 # 检测 pending，调用 apply
  etc/fw_env.config                 # 保留
  usr/sbin/
    zonhor-ota-stage                # 放包 + pending（可保留名）
    zonhor-ab-slot                  # 槽位查询/切换（新，扩展入口）
    zonhor-ota-apply                # 重启后真正刷写（重写）
    zonhor-ota-common.sh            # cimg、校验、路径（精简重写）
    zonhor-ota-led.sh               # 可复用
```

删除或停用：

- `zonhor-ota-recovery-init.sh`（p5 recovery）
- `zonhor-ota-migrate-fip`（布局迁移改为「必须 USB 整刷一次」；或保留为仅刷 fip 的救砖工具）

### 5.3 U-Boot（eMMC 选槽）

参考但 **不直接启用** `cvi_boot_mode_nand.c`：

- 新增或扩展：`loadboot_emmc` / 修改 `emmcboot`，按 `boot_slot` 选择 BOOT 或 BOOT_B 的 mmc 读偏移。
- `root=` 在启动前设为对应 `ROOTFS` / `ROOTFS_B` 设备节点（可由 `mkcvipart` 生成宏，或 env 模板）。
- Phase 1：只读 `boot_slot`。  
- Phase 2：在 bootcmd 里消费 `tries` / `successful`（与 NAND 版对齐键名，便于以后共用文档）。

### 5.4 构建系统

| 改动点 | 内容 |
|--------|------|
| `partition_emmc.xml` | 换成 3.1 布局 |
| `build/Makefile` | Buildroot 路径支持复制 `rootfs_b_*` + `boot_b`（去掉「不支持 A/B」限制，至少对本板） |
| `pack_upgrade` / `mk_package` | zip 内仍可只带一份 boot/rootfs；apply 时写入 inactive；或同时带 `_b` 供 USB 目录使用 |
| `copy_tools` | 照旧拷贝 `usb_dl` + `partition.xml` |
| `AB_SYSTEM` | 可对本板置 `y`，或板级显式复制镜像，避免影响其他板 |

---

## 6. 分阶段实施

### Phase 0 — 准备与冻结（0.5–1 天）

- [x] 确认 eMMC 总容量与现网设备是否允许「必须 USB 整刷一次」迁移。
- [x] 冻结现网 OTA 脚本行为；新布局与旧 p5 方案 **不兼容**，文档写明迁移步骤。
- [x] 列出验收用例（见第 8 节）。

### Phase 1 — 最小 A/B + 双路径（核心，约 1.5–2.5 周）

**实现状态（feat/zonhor-emmc-ab-partition）：代码已落地，板端 USB/OTA 验收待测。**

| 序号 | 任务 | 状态 |
|------|------|------|
| 1.1 | 新 `partition_emmc.xml` + 验证 `cvipart.h` / `blkdevparts` | 已完成 |
| 1.2 | Makefile：Buildroot 产出 `boot_b` / `rootfs_b` CIMG | 已完成 |
| 1.3 | U-Boot：`emmcboot` 按 `boot_slot` 选分区 + 默认 env | 已完成 |
| 1.4 | Slot API + 重写 `zonhor-ota-apply` / `S05ota` / `stage` | 已完成 |
| 1.5 | USB 烧录验证（两槽同镜像、能进系统） | 待板端验证 |
| 1.6 | OTA：A→B、B→A 各一轮；断电冒烟 | 待板端验证 |
| 1.7 | 文档：替换/更新 `OTA-README.md`，根目录本计划保持同步 | 已完成 |

**Phase 1 交付标准：**

1. USB 烧录后默认从 A 启动，A/B 分区均有完整系统。
2. 向 `/mnt/data` 放 `upgrade.zip` → stage → reboot → 从 B 启动新版本。
3. 再 OTA 一次回到 A。
4. OTA 过程中对活动槽只读；断电最多丢「未完成的那次升级」，旧槽仍可启动（Phase 1 靠「没改 boot_slot 则仍旧槽」保证）。

### Phase 2 — 专业回滚（预留落地，约 1 周）

- [ ] U-Boot / 启动脚本消费 `tries_remaining`：新槽启动失败 N 次后自动切回旧槽。
- [ ] userspace `mark-successful`（可挂 `S99` 或应用就绪后调用）。
- [ ] 失败日志与 LED 模式对齐。
- [ ] （可选）槽位 metadata 迁到 MISC 固定结构。

### Phase 3 — 增强（按需）

- [ ] OTA 升级 FIP（双 boot 区 fip/fip_bak + 严格校验）。
- [ ] 差分包 / 压缩包体积优化。
- [ ] 镜像签名与防回滚版本号。
- [ ] HTTP 上传 UI（现有 ramdisk otaserver，需评估是否启用）。

---

## 7. 工作量与文件面（Phase 1）

| 类别 | 文件数（约） | 行数（约） | 说明 |
|------|--------------|------------|------|
| 分区 / 构建 | 3–5 | 100–300 | XML、Makefile、板级 config |
| U-Boot eMMC 选槽 | 2–4 | 300–600 | 新逻辑或改编 NAND 参考 |
| 设备侧 OTA/Slot | 6–10 | 800–1200 | 重写为主，LED/cimg 可复用 |
| 文档 | 2 | — | 本计划 + OTA-README |
| **usb_dl 主机工具** | **0** | **0** | 闭源，不改 |
| **合计** | **~15–20** | **~1.5k–2.5k** | 最小可用 |

相对现状：现有 ~750 行 OTA 脚本大部分替换；`usb_dl` / CIMG / `upgrade.zip` 管线复用。

---

## 8. 验收用例

### 8.1 USB 烧录

1. 空片或旧布局设备进入 USB DL 模式。
2. `usb_dl ... -i $OUTPUT_DIR` 完整刷写（含 fip）。
3. 启动后 `lsblk` 可见 BOOT/BOOT_B/ROOTFS/ROOTFS_B/DATA。
4. `fw_printenv boot_slot` 为 `a`；系统正常。
5. 可选：手动 `boot_slot=b` 重启，确认 B 槽也能起（内容应与 A 相同）。

### 8.2 现场 OTA

1. 构建新版本（可改 `/etc/os-release` 或版本文件便于辨认）。
2. `scp upgrade.zip → /mnt/data/upgrade.zip`，`zonhor-ota-stage && reboot`。
3. 升级过程 LED 可辨；完成后从对面槽启动，版本号已变。
4. 再打一包 OTA，确认能切回另一槽。
5. 在「正在 dd 非活动槽」时断电：上电后仍从旧槽启动，系统可用；可重新 stage。

### 8.3 回归

- 正常应用/外设在 A、B 启动下均可用。
- DATA 分区数据在跨槽 OTA 后仍在（若 DATA 为独立分区）。

---

## 9. 风险与对策

| 风险 | 对策 |
|------|------|
| 旧设备分区不兼容 | 迁移必须 USB 整刷；文档置顶说明 |
| Buildroot 路径无 AB 复制 | Phase 1 显式改 Makefile，勿依赖未启用的全局 `AB_SYSTEM` |
| ENV 写坏导致无法启动 | 保留 USB 救砖；Phase 2 再考虑 ENV_BAK |
| 双 rootfs 容量不够 | 压缩 rootfs、缩小 DATA、或评估更大 eMMC |
| U-Boot 选槽偏移算错 | 用 `mkcvipart` 生成的偏移/宏，禁止手写魔法数 |
| USB 漏刷 fip | 烧录 checklist + 脚本检测分区个数 |

---

## 10. 明确不做（Phase 1）

- 不实现自动 tries 回滚（仅预留键与 API 空实现/写透）。
- 不 OTA 升级 FIP。
- 不改 `usb_dl` 源码/协议。
- 不启用 ramdisk recovery / otaserver（除非单独立项）。
- 不保证与旧「p5 OTA 分区」布局共存或在线迁移。

---

## 11. 关键源码索引（现状）

| 路径 | 角色 |
|------|------|
| `build/boards/cv181x/sg2000_zonhor_sg2000_glibc_arm64_emmc/partition/partition_emmc.xml` | 当前分区表 |
| `device/zonhor-sg2000-glibc-arm64-emmc/overlay/usr/sbin/zonhor-ota-*` | 现 OTA 脚本 |
| `device/zonhor-sg2000-glibc-arm64-emmc/OTA-README.md` | 现 OTA 文档 |
| `build/Makefile`（`CONFIG_BUILDROOT_FS` / `CONFIG_AB_SYSTEM`） | Buildroot A/B 镜像复制 |
| `build/boards/default/partition/partition_spinand_page_2k_ab.xml` | NAND A/B 模板参考 |
| `device/zonhor-sg2000-glibc-arm64-emmc/overlay/usr/sbin/zonhor-ab-slot` | **槽位 API**（新增） |
| `u-boot-2021.10/cmd/cvi_boot_mode_emmc.c` | **eMMC 选槽**（新增） |
| `u-boot-2021.10/cmd/cvi_boot_mode_nand.c` | NAND 选槽参考 |
| `build/tools/common/usb_dl/` | USB 烧录工具（闭源） |
| `build/tools/common/image_tool/` | CIMG / 打包 / cvipart |

---

## 12. 建议排期（一人）

| 周 | 内容 |
|----|------|
| W1 | 分区 + 构建双镜像 + USB 烧录打通 |
| W2 | U-Boot 选槽 + Slot API + OTA apply 主路径 |
| W3 | A↔B 往返、断电用例、文档与收尾 |
| 之后 | Phase 2 回滚按产品稳定性要求插入 |

---

## 13. 一句话总结

用 **双 BOOT + 双 ROOTFS + 小 DATA 暂存** 替换「单 rootfs + 大 OTA recovery 分区」；现场走 **写非活动槽再切 `boot_slot`**，工厂/救砖走 **现有 `usb_dl` 按新 XML 整盘刷**；槽位键名一次性按专业 A/B 预留，Phase 1 只消费 `boot_slot`，后续加回滚不必再改分区。
