# Zonhor SG2000：`freeze` / `mem` 休眠与 SDIO WiFi 恢复记录

> 板型：`zonhor-sg2000-glibc-arm64-emmc`（SG2000 / CV181x + AIC8800 SDIO WiFi/BT）  
> 时间：2026-07-25  
> 相关：`SG2000_RTC_MACRO_VBAT踩坑记录.md`、`CONFIG_SUSPEND` / FSBL warmboot

本文记录一次从「能 `echo mem` 但唤醒后没 IP / 甚至硬挂」到「`freeze`/`mem` 可用且 WiFi 可自动恢复」的排查与改动，以及尚未完成的优化方向。

---

## 1. 结论摘要

| 目标 | 现状 |
|------|------|
| `echo freeze > /sys/power/state` + 尽快恢复路由器 IP | **可用**。守护进程 resume 后续 DHCP；部分情况下驱动侧关联仍在，可直接判为 healthy。 |
| `echo mem > /sys/power/state`（deep）SoC 休眠/唤醒 | **可用**。`PM: suspend entry (deep)` → `PM: suspend exit`。 |
| deep mem 后 WiFi「无感」恢复 | **可用但非固件级零丢联**：resume 后 SDIO 可 `reset_comm`+reinit；FW 关联仍丢，守护进程 **fdrv-only 重载**约 **5–7s** 内恢复（原先 ~12s）。禁止内核内 `cvi_sdio_rescan`（曾 Oops）。 |
| 多次 mem 后 IPv4 稳定（不双地址、不漂移） | **已修（2026-07-25）**。根因是 recover 用 `udhcpc` 与系统 `dhcpcd` 抢同一 MAC（不同 Client ID → 双租约，如 `.141`+`.139`）。现只走 dhcpcd + MAC `clientid`；`/mnt/data/wlan0.preferred_ip` 记录偏好地址。板测连续 3 次 mem 均回到单一 `192.168.100.141`。 |
| `wifi-bt-lpm.sh prepare-mem` 后再 deep mem | **高风险，禁止作为默认路径**。曾出现电流回升但系统卡死，软 RST 无法启动，需断全电。 |

板端快捷自测（建议串口；USB RNDIS 在 suspend 期间会断）：

```sh
# freeze
echo 0 > /sys/class/rtc/rtc0/wakealarm; echo +10 > /sys/class/rtc/rtc0/wakealarm
echo freeze > /sys/power/state

# deep mem（不要先 prepare-mem）
echo 0 > /sys/class/rtc/rtc0/wakealarm; echo +10 > /sys/class/rtc/rtc0/wakealarm
echo mem > /sys/power/state

tail -f /var/log/zonhor-wifi-pm.log
zonhor-wifi-pm-recover status
```

`wifi-bt-lpm.sh` 在 `/mnt/system/wifi-bt-lpm.sh`（overlay 已链到 `/usr/sbin/wifi-bt-lpm.sh`）。

---

## 2. 硬件 / 软件基线

### 2.1 外设归属

| 外设 | 接口 | 说明 |
|------|------|------|
| 存储 | eMMC（`mmc0`） | SD0 因 AXP INT 占 PAD 而 disabled |
| WiFi | SDIO1（`&wifisd`） | AIC8800，`non-removable` + 板级 `keep-power-in-suspend` |
| WiFi 总电 | GPIOA15 | `wifi_pin` / `poweron-gpio` |
| HOST_WAKE_WF | PWR_GPIO3（porte 3） | SoC → WiFi |
| WIFI_WAKE_HOST | PWR_GPIO5（porte 5） | WiFi → SoC wake IRQ |
| BT | UART4 + bluesleep GPIO | `aic8800_btlpm`，`wakeup-source` |

### 2.2 内核能力（defconfig）

- `CONFIG_PM=y` / `CONFIG_SUSPEND=y`
- `CONFIG_AIC8800*`、`CONFIG_AIC8800_WLAN_SUPPORT=m`、`CONFIG_AIC8800_BTLPM_SUPPORT=m`
- `mem_sleep` 实测：`s2idle [deep]`（`echo mem` 走 deep）
- MMC host：`sdhci-cv181x` 有 `SET_SYSTEM_SLEEP_PM_OPS`
- AIC fdrv：`CONFIG_GPIO_WAKEUP` / `CONFIG_WIFI_SUSPEND_FOR_LINUX` / `CONFIG_SDIO_PWRCTRL` / `CONFIG_PLATFORM_CVITEK`

### 2.3 用户态网络（改前缺口）

- `duo-init.sh` 加载 `aic8800_{bsp,fdrv,btlpm}.ko`，`hciattach` BT
- `auto.sh` 只在开机等 `wlan0`、起 `wpa_supplicant`，拿到真实 IP 后 **exit**，无 suspend/resume 钩子
- 因此即便驱动 resume 成功，DHCP lease / 关联丢失后也不会自动补

---

## 3. 现象与踩坑时间线

### 3.1 最初现象

- SoC 能进 `mem` 并唤醒（电流回 ~130mA）
- 唤醒后没有路由器 IP（有时 `wpa_state` 仍看似异常）
- 脚本 `wifi-bt-lpm.sh`「找不到」：实际在 `/mnt/system/`，不在默认 PATH

### 3.2 第一次远程 `prepare-mem` + `mem`

- 进入 deep 后电流回来，但系统无响应
- **手动 RST 无法启动**，断全电才恢复  
- 推断：AIC8800 被手动拉进 sleep 后再 deep，常电域/SDIO 状态坏掉；软复位不清模组总电，启动卡在 SDIO/`cvi_sdio_rescan`

### 3.3 串口分层实测（关键对照）

| 实验 | 结果 |
|------|------|
| 裸 `freeze`（s2idle） | 睡醒成功；驱动 resume 正常；`wpa_state=COMPLETED` 但 **IPv4 丢失**；`udhcpc` 立刻拿回 IP |
| 裸 `mem`（不做 prepare-mem） | SoC 醒；大量 `aicwf_sdio_wakeup: sdio wakeup fail` → `cmd timed-out` → `SCANNING`；重载 KO 可救 |
| `prepare-mem` + `mem` | 硬挂风险，勿用 |

结论拆分：

1. **丢 IP**：用户态缺 post-resume DHCP（freeze 即可复现）  
2. **deep 后 WiFi 死**：SDIO/固件在 host 时钟切断后无法被 `wakeup_reg` 拉活  
3. **RST 救不回**：多与模组未真正断电 + 坏状态有关，不是单纯「没 IP」

### 3.4 RTC 干扰项

- 板上常见 `RTC invalid time`、`RO_T` 仍是小秒数（MACRO/VBAT 路径见 RTC 文档）
- `echo +N > /sys/class/rtc/rtc0/wakealarm` 多数时候仍能唤醒，但 RTC 壁钟不可靠
- deep mem 真正可靠唤醒路径仍依赖 RTC/MCU；gpio-keys 更偏 freeze/light

---

## 4. 根因（技术）

### 4.1 freeze 丢 IP

- 关联可保留，地址被清或 lease 未续  
- `auto.sh` 拿到 IP 就退出，无常驻恢复  
- 修复：resume 后 `udhcpc`（必要时 reconnect）

### 4.2 deep mem 后 SDIO 失败

典型日志演进：

**改前：**

```text
PM: suspend entry (deep)
aicwf_sdio_suspend ...
aicwf_sdio_resume enter
aicwf_sdio_wakeup: sdio wakeup fail   # 重复约 20 次
rwnx_set_wifi_suspend resume
cmd timed-out / reg write failed
wpa_state=SCANNING
```

**改后（suspend 前强制 ACTIVE + 停 idle timer）：**

```text
PM: suspend entry (deep)
aicwf_sdio_suspend ...
aicwf_sdio_resume enter
rwnx_set_wifi_suspend resume
tx msg fc retry fail / cmd timed-out   # 仍可能丢关联，但少了 wakeup fail 风暴
→ 用户态重载 KO → COMPLETED + DHCP（约 12s）
```

机制要点：

1. `MMC_PM_KEEP_POWER` + `keep-power-in-suspend`：卡不断电，但 SDHCI host 时钟会切  
2. Idle `CONFIG_SDIO_PWRCTRL` 可能已把链路置于 `SDIO_SLEEP_ST`；带着 sleep 进 deep 后，`wakeup_reg` 写失败  
3. 原 resume 顺序曾是先 `pwr_stctl(ACTIVE)` 再拉 `HOST_WAKE`；对「已 deassert HOST_WAKE」的路径不友好（`prepare-mem`）  
4. 即便 HOST_WAKE 一直为高、也不再强制 sleep，deep 后仍可能出现 FW/cmd 超时 → 需重载模组才能完全恢复

### 4.3 守护进程曾「看起来启动了但没干活」

- `start-stop-daemon` / 外层 `&` 写 PIDFILE，与脚本内 `already running` 自检竞态  
- 进程立刻退出，`/var/log/zonhor-wifi-pm.log` 为空  
- 已改为：launcher 等 daemon 自己写 PID；自检忽略「PID == 自己」

---

## 5. 已落地改动

### 5.1 内核 AIC8800 fdrv（`aicwf_sdio.c`）

路径：`linux_5.10/drivers/net/wireless/aicsemi/aic8800/aic8800_fdrv/aicwf_sdio.c`

| 点 | 改动 |
|----|------|
| `aicwf_sdio_suspend` | 进系统休眠前：`pwr_stctl(ACTIVE)` + `pwrctl_timer(0)` + 拉高 `HOST_WAKE`；**不再**强制 `SDIO_SLEEP_ST` |
| `aicwf_sdio_resume` | `HOST_WAKE` → **`sdio_reset_comm`**（SDHCI 时钟切断后重建 SDIO）→ 强制走 wakeup 路径 `ACTIVE` → `rwnx_set_wifi_suspend('0')`；失败则再试一次；仍失败则 **deferred WLAN 断电复位 + `cvi_sdio_rescan`** |
| `aicwf_sdio_wakeup` | 写 `wakeup_reg` 前再确保 `HOST_WAKE` |
| `rwnx_set_wifi_suspend('0')` | 加长 settle（约 20ms），先 ACTIVE 再清 FW LP level；**返回错误码** |

热更新：重编 `aic8800_fdrv.ko` 拷到板子 `/mnt/system/ko/` 后 `rmmod`/`insmod` 即可（无需整包刷机）。

### 5.2 用户态 post-resume 恢复

| 文件 | 说明 |
|------|------|
| `device/zonhor-*/overlay/usr/sbin/zonhor-wifi-pm-recover` | 守护进程：看 `suspend_stats/success`；freeze 偏 DHCP；deep 后非 COMPLETED 则直接重载 AIC |
| `device/zonhor-*/overlay/etc/init.d/S41wifi-pm` | 开机启动 |
| `device/zonhor-*/overlay/etc/dhcpcd.conf` | MAC `clientid`、deny `usb0`/`eth0`、wlan0 `noipv4ll`（稳定租约） |
| `.../usr/sbin/wifi-bt-lpm.sh` → `/mnt/system/wifi-bt-lpm.sh` | PATH 软链 |

恢复策略（摘要）：

1. 已 `COMPLETED` 且**仅有一个**非链路本地 IPv4 → 认为 healthy（并写入 preferred）  
2. `COMPLETED` 无 IPv4 / 双地址 → `ip -4 addr flush` + **`dhcpcd -k/-n` rebind**（**禁止 udhcpc**）  
3. `SCANNING`/`DISCONNECTED` 等 → **跳过长时间 reconnect，直接重载** fdrv(+必要时 bsp) + wpa + dhcpcd  
4. 偏好地址：`/mnt/data/wlan0.preferred_ip`（跨 resume / 模块重载）  

日志：`/var/log/zonhor-wifi-pm.log`  
调试：`zonhor-wifi-pm-recover status|once|stop`

#### 5.2.1 多次休眠后 IP 变化（已修）

现象：`wlan0` 同时出现两个地址，例如  
`192.168.100.141`（无 `dynamic`）+ `192.168.100.139`（`dynamic noprefixroute`），或默认路由 src 在两者间跳。

根因：

- 系统常驻 **`dhcpcd`**（`S41dhcpcd`），lease 在 `/var/db/dhcpcd/`  
- recover 旧逻辑另起 **`udhcpc`** → 与 dhcpcd **Client ID 不同**（duid+IAID vs MAC），路由器发**两份租约**  
- eth0/wlan0 同 UID 后缀 → duid 模式下 **IAID 冲突**（`wlan0: IAID conflicts with eth0`）加重不稳定  

修复：recover 只调用 dhcpcd；`dhcpcd.conf` 改 `clientid` 并 `denyinterfaces eth0 usb0`。

### 5.3 板端已验证过的组合（2026-07-25）

- `freeze`：可醒；守护进程或驱动侧保持 healthy  
- 裸 `mem`：SoC 醒；守护进程 `recover ok: module reload`，约 12s 内恢复 `COMPLETED` + `192.168.100.x`  
- **未再验证** `prepare-mem` + `mem`（故意跳过）

---

## 6. 未完成 / 以后可继续优化

按优先级大致排列：

### P0 — 产品化与安全

1. **正式镜像固化**  
   当前多为 SCP 热更新；需完整编一次 eMMC/NAND，确认 `system`/`rootfs` overlay 带上新 KO 与 `S41wifi-pm`。

2. **明确禁止默认 `prepare-mem` + deep mem**  
   文档/脚本注释已提示；可考虑在 `prepare-mem` 打印强警告，或拆成仅用于实验室的 `prepare-mem-danger`。

3. **软复位后仍起不来时的模组断电策略**  
   评估 FSBL/U-Boot/复位路径是否应脉冲 GPIOA15，避免「RST 无效只能断全电」。

### P1 — 真正「无感」WiFi（驱动级）

4. **deep resume 后仍 `cmd timed-out` 的根因** — *已基本定位（2026-07-25 板测）*  
   - `sdio_reset_comm` + func reinit → **SDIO 字节通路 OK**（`sdio ready after reinit`）  
   - FW `me_set_lp_level` 在 bare deep 后会死等 ~3s 并 **毒化 cmd queue** → 已改为 `wifi_suspend_active==0` 时跳过  
   - 关联仍丢：FW WiFi 栈在 deep 后不可用，需重载 fdrv（重下固件语义）  
   - 剩余：能否在不卸模组情况下原地重载 FW（真无感）  

5. **resume 失败时内核内 power-cycle** — *已回退*  
   `WLAN_POWER`+`cvi_sdio_rescan` 在模组仍加载时会 Oops（`aicbsp_get_feature` NULL）。  
   硬恢复交给用户态 `rmmod`/`insmod`（bsp `power_on` 路径）。`aicbsp_get_feature` 已加 NULL 防护。  

6. **缩短恢复时间** — *板测 ~5–7s（目标 2–3s）*  
   已做：跳过 doomed LP clear、跳过无效 soft reconnect、**fdrv-only** 优先重载。  
   下一步：进一步压缩 fdrv probe/关联/DHCP；或内核内安全 FW 重载。

### P2 — 体验与体验

7. **RTC 壁钟 / wakealarm 可靠性**  
   与 `SG2000_RTC_MACRO_VBAT踩坑记录.md` 对齐：保证 `rtc_mode`、RO_T、alarm 在 deep 唤醒场景稳定（当前 `+N` 相对闹钟可用但时间常为 1970）。

8. **BT 与 mem 同测**  
   `aic8800_btlpm` 的 `CONFIG_AUTO_PM`、`hciattach` 在 resume 后是否需重跑；与 WiFi 重载是否互相踩踏。

9. **USB gadget（RNDIS `usb0`）resume**  
   suspend 期间 SSH over 192.168.42.1 会断；评估是否要在 resume 后自动重绑 configfs gadget，缩短调试断连时间。

10. **`auto.sh` 与守护进程职责边界**  
    开机 IP UI vs 常驻 PM recover 可合并成一个「wlan 健康看门狗」，避免两套逻辑漂移。

11. **功耗数据**  
    对比：idle / freeze / deep mem（WiFi ACTIVE 保电）电流；评估 deep 时是否允许 WiFi 真 sleep（需先解决 4/5，否则会回到硬挂）。

12. **串口/`no_console_suspend`**  
    量产可关；调试 deep 时打开便于抓 resume 早期日志。

---

## 7. 运维速查

```sh
# 休眠能力
cat /sys/power/state          # freeze mem
cat /sys/power/mem_sleep      # s2idle [deep]
cat /sys/power/suspend_stats/success

# WiFi LPM / GPIO（手动，非系统 mem 必需）
wifi-bt-lpm.sh status
# 危险：wifi-bt-lpm.sh prepare-mem   # 勿接 deep mem

# 自动恢复
zonhor-wifi-pm-recover status
tail -f /var/log/zonhor-wifi-pm.log
/etc/init.d/S41wifi-pm restart

# 手动重载 AIC（与守护进程 hard path 等价；DHCP 只用 dhcpcd）
killall wpa_supplicant udhcpc
rmmod aic8800_btlpm aic8800_fdrv aic8800_bsp
insmod /mnt/system/ko/aic8800_bsp.ko
insmod /mnt/system/ko/aic8800_fdrv.ko
insmod /mnt/system/ko/aic8800_btlpm.ko
zonhor-mac-from-uid wlan0
wpa_supplicant -B -i wlan0 -c /etc/wpa_supplicant.conf
dhcpcd -n wlan0
# 看是否双地址：ip -4 addr show wlan0
```

重编 fdrv 模块示例：

```sh
export ARCH=arm64
export CROSS_COMPILE=.../aarch64-linux-gnu-
KDIR=linux_5.10/build/sg2000_zonhor_sg2000_glibc_arm64_emmc
make -C "$KDIR" M=$PWD/linux_5.10/drivers/net/wireless/aicsemi/aic8800/aic8800_fdrv modules
# 产物：.../aic8800_fdrv/aic8800_fdrv.ko → scp 到 /mnt/system/ko/
```

---

## 8. 关键路径索引

```
linux_5.10/drivers/net/wireless/aicsemi/aic8800/aic8800_fdrv/aicwf_sdio.c
linux_5.10/drivers/soc/cvitek/wifi_pin/cvi_wifi_pin.c
linux_5.10/drivers/mmc/host/cvitek/sdhci-cv181x.c
build/boards/cv181x/sg2000_zonhor_sg2000_glibc_arm64_emmc/dts_arm64/*.dts   # wifisd / wifi_pin / btlpm
device/zonhor-sg2000-glibc-arm64-emmc/overlay/usr/sbin/zonhor-wifi-pm-recover
device/zonhor-sg2000-glibc-arm64-emmc/overlay/etc/init.d/S41wifi-pm
device/zonhor-sg2000-glibc-arm64-emmc/overlay/etc/dhcpcd.conf
device/zonhor-sg2000-glibc-arm64-emmc/overlay/mnt/system/wifi-bt-lpm.sh
device/zonhor-sg2000-glibc-arm64-emmc/overlay/mnt/system/duo-init.sh
device/zonhor-sg2000-glibc-arm64-emmc/overlay/mnt/system/auto.sh
fsbl/plat/cv181x/platform.c          # CONFIG_SUSPEND / warmboot / RTC_POR_RST_CTRL
SG2000_RTC_MACRO_VBAT踩坑记录.md
```

调试串口日志样例（若仍保留）：`zonhor_mem_serial_20260725.log`。

---

## 9. 「不要做」清单

1. **不要**在未接串口、未准备断全电的情况下测 `prepare-mem` + `echo mem`  
2. **不要**假设 soft RST 能清掉 AIC8800 坏状态  
3. **不要**以为 `wpa_state=COMPLETED` 或接口上还有旧 IPv4 就等于网络可用——要以能 ping 网关 / 新 lease 为准  
4. **不要**把 `date`/1970 当 RTC 健康；看 `RO_T` / `rtc_mode`（见 RTC 文档）  
5. **不要**只改源码不更新板上 `/mnt/system/ko/aic8800_fdrv.ko` 就判定「驱动已修」  
6. **不要**在 recover / 手工排障时再跑 `udhcpc`（会与 dhcpcd 双租约导致 IP「变化」）  
7. **不要**只看 `ip addr` 有地址就认为单一路径——确认 `addr_count==1` 且默认路由 `src` 与之一致

---

文档维护：若再改 AIC suspend/resume、PM 守护进程或默认休眠策略，请同步更新 §1 结论表与 §6 未完成项。
