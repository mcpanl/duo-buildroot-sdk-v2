# Zonhor SG2000：多次 `mem` 休眠唤醒后 WiFi IP 变化排查记录

> 板型：`zonhor-sg2000-glibc-arm64-emmc`（SG2000 / CV181x + AIC8800 SDIO WiFi）  
> 时间：2026-07-25  
> 相关：`ZONHOR_SG2000_mem_wifi_suspend_resume.md`（休眠/WiFi 恢复总览）

本文只记录「SoC 能睡能醒、WiFi 也能恢复关联，但 **IPv4 会变 / 出现双地址**」这一条线的现象、根因与修复。

---

## 1. 结论

| 项 | 内容 |
|----|------|
| 现象 | 多次 `echo mem > /sys/power/state` 后，`wlan0` 上出现两个 IPv4，或对外可达地址在 `.141` / `.139` 间跳 |
| 根因 | 用户态 **同时跑了两套 DHCP 客户端**：系统 `dhcpcd` + recover 脚本里的 `udhcpc`；Client ID 不同 → 路由器发两份租约 |
| 次因 | 默认 `dhcpcd.conf` 使用 `duid`；eth0/wlan0 由同一 UID 派生 MAC（仅首字节不同）→ **IAID 冲突** |
| 修复 | recover **只走 dhcpcd**；`dhcpcd.conf` 改 MAC `clientid`，`denyinterfaces eth0 usb0`；偏好地址落盘 `/mnt/data/wlan0.preferred_ip` |
| 板测 | 热更新后连续 3 次 deep `mem`，均回到 **单一** `192.168.100.141`（`addr_count=1`） |

---

## 2. 现场现象

### 2.1 内核侧（每次 mem 后仍会出现，属已知路径）

deep resume 后 AIC 命令队列仍可能超时，守护进程 fdrv 重载，phy 序号递增：

```text
cmd timed-out
tkn[38]  ... cmd:4103-SCANU_VENDOR_IE_REQ ...
cmd queue crashed
...
aicbsp: ... bus down
ieee80211 phy4 / phy5 / phy6:   # 每次重载 +1
** CAUTION: USING PERMISSIVE CUSTOM REGULATORY RULES **
```

这与「能恢复关联」不矛盾：`zonhor-wifi-pm-recover` 会 `rmmod`/`insmod` 后重新起 `wpa_supplicant`。问题出在 **IP 层**。

### 2.2 网络侧（本次要修的）

同一 `wlan0` 上两个地址：

```text
10: wlan0: ...
    inet 192.168.100.141/24 ... scope global wlan0
    inet 192.168.100.139/24 ... scope global secondary dynamic noprefixroute wlan0
```

对照：

| 地址 | 特征 | 来源 |
|------|------|------|
| `.141` | 无 `dynamic` / `noprefixroute` | busybox **`udhcpc`**（recover 脚本） |
| `.139` | `dynamic noprefixroute` | 系统常驻 **`dhcpcd`** |

默认路由 `src` 往往指向 dhcpcd 那一侧（当时为 `.139`），而人手 SSH / 文档常用 `.141`，表现为「IP 变了」或「有时连得上有时连不上」。

板上进程对照（修复前）：

```text
dhcpcd: [manager] ...
dhcpcd: [BPF ARP] wlan0 192.168.100.139
zonhor-wifi-pm-recover daemon
wpa_supplicant -B -i wlan0 ...
# recover 日志：
udhcpc: broadcasting select for 192.168.100.141 ...
udhcpc: lease of 192.168.100.141 obtained ...
```

lease 文件侧：`/var/db/dhcpcd/wlan0-*.lease` 里租约字节为 `c0 a8 64 8b` → **`.139`**，与 `udhcpc` 拿到的 `.141` 不一致。

---

## 3. 根因分析

### 3.1 双 DHCP 客户端 = 双 Client ID = 双租约

```text
                    ┌─────────────┐
   MAC 06:ac:15:… ──┤  路由器 DHCP │
                    └──────┬──────┘
               ┌───────────┴───────────┐
               │                       │
        dhcpcd (DUID+IAID)       udhcpc (MAC client id)
               │                       │
            .139 lease              .141 lease
               └───────────┬───────────┘
                        wlan0
                     （双地址）
```

系统镜像启用 `BR2_PACKAGE_DHCPCD`，开机 `S41dhcpcd`。  
`auto.sh` 只等关联/IP，不自己跑 DHCP。  
`zonhor-wifi-pm-recover` 旧逻辑在 resume 后：

```sh
ip -4 addr flush dev wlan0
udhcpc -i wlan0 -n -q -t 4
```

只 `killall udhcpc`，**不碰 dhcpcd**。flush 后 udhcpc 立刻再要地址；dhcpcd 见 carrier/新 ifindex 也会按自己的 lease/Client ID 再要一份 → 双地址。

模块重载后 `wlan0` ifindex 变了（日志里 `10:` → `11:` → `13:` → `14:`），加剧「像新客户端」的感觉，但真正拆成两个 IP 的是 **两套客户端**。

### 3.2 duid 模式下 eth0 / wlan0 IAID 冲突

`zonhor-mac-from-uid`：

- eth0：`02` + UID 后 5 字节  
- wlan0：`06` + **同一** 后 5 字节  

dhcpcd 在 `duid` 模式下 IAID 常取 MAC 后 4 字节 → eth0/wlan0 **IAID 相同**。历史日志已有：

```text
wlan0: IAID conflicts with one assigned to eth0
```

另：eth0 无 DHCP 时会落 IPv4LL（如 `169.254.155.185`），污染路由表。与「WiFi IP 漂移」相关度次之，但一并处理更干净。

### 3.3 与「没 IP」问题的区别

| 问题 | 文档 | 性质 |
|------|------|------|
| freeze/mem 后没地址 / 关联死 | `ZONHOR_SG2000_mem_wifi_suspend_resume.md` | SDIO/FW + 缺 post-resume 恢复 |
| 有地址但会变 / 双地址 | **本文** | 用户态 DHCP 策略错误 |

---

## 4. 修复内容

### 4.1 `zonhor-wifi-pm-recover`

路径（eMMC / NAND overlay 同步）：

- `device/zonhor-sg2000-glibc-arm64-emmc/overlay/usr/sbin/zonhor-wifi-pm-recover`
- `device/zonhor-sg2000-glibc-arm64-nand/overlay/usr/sbin/zonhor-wifi-pm-recover`

要点：

1. **`renew_dhcp` 只调用 dhcpcd**（`dhcpcd -k wlan0` → `dhcpcd -n wlan0`）；若误有 `udhcpc` 则杀掉  
2. `wifi_ok`：要求 `COMPLETED` + **恰好一个** 非链路本地 IPv4（双地址视为不健康）  
3. `normalize_ipv4`：多余地址删掉  
4. 偏好地址：`/mnt/data/wlan0.preferred_ip`（DATA 分区，跨 resume 保留）  
5. 仅当镜像没有 dhcpcd 时才 fallback `udhcpc -r <preferred>`

### 4.2 `dhcpcd.conf`（新建 overlay）

路径：

- `device/zonhor-*/overlay/etc/dhcpcd.conf`

相对原 sample（`duid` + 仅 `denyinterfaces usb0`）的变更：

| 项 | 原 | 现 |
|----|----|----|
| Client ID | `duid` | **`clientid`**（整段 MAC，eth0/wlan0 自然不同） |
| deny | `usb0` | **`usb0 eth0`** |
| wlan0 | （无） | `ipv4only` + `noipv4ll` |
| SLAAC | `slaac private` | `slaac hwaddr`（配合固定 MAC） |

### 4.3 运维注意

- 从 `duid` 切到 `clientid` 时，路由器可能 **一次性** 换发新地址（本次板子从 dhcpcd 旧租约 `.139` 收到 MAC 侧已占用的 `.141`，之后稳定在 `.141`）  
- 排障时 **不要再手动跑 `udhcpc`**，否则问题会复发  
- 正式镜像需把上述 overlay 编进 rootfs；当前板子已 SCP 热更新

---

## 5. 验证记录（2026-07-25 板测）

热更新 `/usr/sbin/zonhor-wifi-pm-recover` + `/etc/dhcpcd.conf` 后：

1. 停 dhcpcd / recover → flush → 重启 dhcpcd  
2. 曾短暂只剩单一地址；clientid 切换后租约落在 **`192.168.100.141`**  
3. 连续 deep `mem`（RTC `wakealarm +10/~12`）：

| 次数 | suspend_success | 恢复后 IPv4 | addr_count | 备注 |
|------|-----------------|-------------|------------|------|
| mem1 | 7→8 | `192.168.100.141` | 1 | fdrv-only reload + dhcpcd |
| mem2 | 8→9 | `192.168.100.141` | 1 | 日志 `preferred IPv4=192.168.100.141` → `dhcp ok` |
| mem3 | 9→10 | `192.168.100.141` | 1 | 同上；无 `udhcpc` 进程 |

自检命令：

```sh
ip -4 -o addr show wlan0
# 期望恰好一行，无 secondary

ip -4 route | head -5
# default ... src 应与上面同一地址

ps | grep -E 'udhcpc|dhcpcd' | grep -v grep
# 应有 dhcpcd，不应有 udhcpc

cat /mnt/data/wlan0.preferred_ip
tail -20 /var/log/zonhor-wifi-pm.log
zonhor-wifi-pm-recover status
```

---

## 6. 「不要做」清单（本文相关）

1. **不要**在 recover / 手工排障时再起 `udhcpc`（与 dhcpcd 双租约）  
2. **不要**只看 `ip addr`「有地址」——确认无 `secondary`，且默认路由 `src` 一致  
3. **不要**把 phy 序号递增当成 IP 漂移原因（那是模块重载的正常副作用）  
4. **不要**只改仓库 overlay 不更新板上文件就判定「IP 已稳」

---

## 7. 路径索引

```
device/zonhor-sg2000-glibc-arm64-emmc/overlay/usr/sbin/zonhor-wifi-pm-recover
device/zonhor-sg2000-glibc-arm64-emmc/overlay/etc/dhcpcd.conf
device/zonhor-sg2000-glibc-arm64-nand/overlay/usr/sbin/zonhor-wifi-pm-recover
device/zonhor-sg2000-glibc-arm64-nand/overlay/etc/dhcpcd.conf
device/zonhor-*/overlay/etc/init.d/S41wifi-pm
device/zonhor-*/overlay/usr/sbin/zonhor-mac-from-uid
buildroot/package/dhcpcd/S41dhcpcd
buildroot/package/dhcpcd/0001-ignore-interface-usb0-for-duo.patch   # 仅 deny usb0；板级 conf 覆盖更严

板上：
  /usr/sbin/zonhor-wifi-pm-recover
  /etc/dhcpcd.conf
  /var/db/dhcpcd/
  /mnt/data/wlan0.preferred_ip
  /var/log/zonhor-wifi-pm.log
```

相关总览：`ZONHOR_SG2000_mem_wifi_suspend_resume.md` §5.2 / §5.2.1。

---

文档维护：若再改 DHCP 策略、Client ID 或 preferred IP 路径，请同步更新 §1 结论表与 §4。
