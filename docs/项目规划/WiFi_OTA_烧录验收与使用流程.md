# WiFi 局域网 OTA — 逐功能逐步骤验收手册

> 版本 **V3.1**（2026-10-08）· 对应改动记录 **§61 ~ §69**、ICD **V1.4+**
> 方案 `docs/项目规划/WiFi局域网OTA升级实施方案.md`
> 分支：**main**
>
> **V3.1 变更（2026-10-07/08 实弹验收后同步，§65~§69）**：
> ① **实弹验收通过**（见 §15 实测记录）；
> ② OTA 任务栈内化为内部 RAM（§65，OFFER 即 panic 根因）；
> ③ **设备端写槽续传证伪降级为全新下载**（§67，esp_ota API 三大约束）——E 组判据按新语义执行；
> ④ 写槽 32KB 攒块缓解升级期屏闪（§67）；
> ⑤ PC 端下载超时 180s → 600s 上限 + 120s 活动超时（§68）；
> ⑥ 自证改 **1s 周期条件驱动**，浸泡 ~10s 即转正（§69），下文凡"30s 窗口"按此理解。
>
> **V3 变更：PC 端主路线切换为 pc_host v0.3「固件升级」页（GUI）**。
> 原因（源码实证）：设备回包固定发 PC:50003（`link_manager.c:102` 端口覆写），而 C# agent
> 用临时 UDP 口等 ACCEPT **收不到设备回包**（真 bug）——C# `--ota` 降级为"不建议使用"，
> 本手册所有 PC 侧操作以 GUI 为准（§3 有开关对照表）。
>
> 本手册的写法：**每个功能 = 一个独立验收项**，每项给出
> ① 怎么操作 ② **预期 PC 日志** ③ **预期设备日志（可 grep）** ④ 内存三元组基线 ⑤ 判据。
> 出问题时查 §9 排错表和 §10 日志关键字速查。PC 侧日志同时出现在
> **GUI 日志窗**与启动 pc_host 的控制台（两者内容一致，grep 用控制台或 `docs/_serial_capture.py` 输出）。

---

## 0. 一分钟速览

```
PC: pc_host「固件升级」页（桌面 ESP32_Voice_Host.bat → 左侧第 4 页）
        │  ① 选 bin（自动解析 esp_app_desc_t → 版本/大小/SHA256）
        │  ② 版本比对结论（可升级 / 需勾选忽略比对）
        ├─ UDP 50003: OTA_OFFER（×3，10s/次）
        │
设备:   ├─ 6 条校验链 → OTA_ACCEPT（回 PC:50003，link 分发器喂会话）
        ├─ HTTP :50005 GET /firmware.bin（支持 Range 续传）
        ├─ 边收边写备用槽 + 每 256KB 段哈希落 nvs
        ├─ OTA_PROGRESS ×N → GUI 进度条 → esp_ota_end → 段数校验 → set_boot_partition
        ├─ OTA_RESULT{ok:1} → 1s → esp_restart
        │
        └─ 自证（WiFi 已连 + link 在线，1s 周期条件驱动，浸泡 ~10s 即 mark VALID）
           （不自证 / 窗口内断电 ⇒ bootloader 下次启动自动回滚旧槽）
           PC: 60s 内心跳 fw_ver == 期望版本 → 「★ 版本核对通过」
```

**零新增端口**：控制面 = 既有 UDP 50003；数据面 = 设备作 HTTP 客户端拉 PC 的 :50005。

---

## 1. 前置检查（升级前必做，5 项）

### 1.1 固件尺寸必须装得进备用槽 ⚠

main 分支分区表：

| 分区 | 偏移 | 大小 |
|---|---|---|
| `ota_0` | 0x20000 | **5 MB** (0x500000 = 5,242,880 B) |
| `ota_1` | 0x520000 | **5 MB** |
| `uext(ota_2)` | 0xA20000 | 2.875 MB（副屏固件专属，OTA 永不触碰） |
| `storage` | 0xD00000 | 3 MB（littlefs） |

```bash
ls -la build/display_audio_photo.bin          # 必须 < 5,242,880
```

> ⚠ **`build/` 若残留其他分支的产物，尺寸会不符**。实测遇到：人脸识别分支的
> bin = 6,837,312 B（6.52 MB），**装不进 5MB 槽** → 设备回 `OTA_REJECT reason=space`。
> ⇒ **OTA 前先确认在 main 分支且 `build/` 是本次构建的**（最稳：`idf.py fullclean` 后重建）。

### 1.2 两侧同一 WiFi、防火墙放行

- PC 与设备连**同一路由器同网段**（路由器不能开 AP 隔离）；
- Windows 防火墙放行 **UDP 50003 / TCP 50005**（首次运行弹窗勾"专用网络"）。
- ⚠ **PC 上 Clash(7890) TUN 会劫持设备→PC 的 LAN 入站 HTTP**（回 502 文本 ⇒ 设备端解析失败）。
  测试前给私有网段 `DIRECT`，或临时关 TUN。

### 1.3 关闭 PC 旧实例（端口冲突）

**OTA 用 pc_host（GUI）**：关掉 C# 版 `tools/pc_now_agent`（如后台残留 `pc_now_agent.exe` 进程先结束）。
pc_host 自身启动即占 50005（OTA 数据面常驻），同一 PC 只跑一份。

### 1.4 设备不在"忙"状态

录音中 / 通话中 / 推流中 / 已有升级在进行 → 设备回 `OTA_REJECT reason=busy`。

### 1.5 设备不在自证窗口内

设备刚被 OTA 上来、还没转正（PENDING_VERIFY，1s 周期条件驱动浸泡 ~10s 即转正，
30s 兜底判死）→ 回 `OTA_REJECT reason=pending`。等 ~15s 重发即可。

---

## 2. 编译与首次烧录

### 2.1 编译设备端

```bash
cd C:\Users\xiao1\Desktop\esp32\display_audio_photo
idf.py reconfigure     # ★ 必须先做：新 Kconfig 符号 + esp_app_format 新依赖
idf.py build
```

**构建后核对**：

```bash
grep "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y" sdkconfig   # 应输出该行
ls -la build/display_audio_photo.bin                        # < 5,242,880
```

### 2.2 首次 USB 全量烧录（铺底）

OTA 的前提是设备**已有一个能起来且带 OTA 服务的固件**：

```bash
idf.py flash                   # app + bootloader + 分区表
idf.py storage-flash           # 素材镜像（littlefs，含壁纸/字库）
```

> 单 app 槽布局下 **回滚的兜底永远是 USB 全量烧录**（无 factory 分区）。

### 2.3 确认设备联网

屏幕进主界面；串口出现：

```
LINK: ONLINE peer=192.168.1.xx
```

> ★ **排障纪律**：烧固件前先关 PC 上位机 —— PC 端周期心跳会把一次性崩溃放大成崩溃循环。

---

## 3. 启动 PC 端 OTA 服务（V3：pc_host GUI 路线）

### 3.1 启动

1. 双击桌面 **`ESP32_Voice_Host.bat`**（或 `pc_host\start_all.bat`）；
2. 侧边栏进「**固件升级**」页（第 4 项，⇪ 图标）；
3. 固件路径默认已定位 `build\display_audio_photo.bin`（也可"浏览…"另选）；
4. 页面立即显示解析结果：**固件版本 / 工程·构建时间 / 大小 / SHA256**——
   解析失败（误选 bootloader.bin 等）会显示"—"且开始按钮不亮。

### 3.2 GUI 与原 C# 开关对照表

| C# 开关（旧） | GUI 对应操作 | 用于验收项 |
|---|---|---|
| `--ota <bin>` | 选文件 → **开始升级**按钮 | 全部 |
| `--ota-limit N` | 「只发前 [N] %」数字框（0=全量） | **E 组续传** |
| `--ota-corrupt` | 勾「注入 1bit 损坏」 | **F 组坏固件** |
| `--force` | 勾「忽略版本比对」 | **D 组同版本重刷** |
| （无） | **中止**按钮（会话中可点） | **G 组主动中止** |

### 3.3 GUI 自身验收项（I 组，新增）

| 步骤 | 操作 | 判据 |
|---|---|---|
| I-1 | 侧边栏状态点 | 设备在线时变绿并显示 IP；离线"发现中" |
| I-2 | 选有效 bin | 版本/大小/SHA256 即时刷新；「比对结论」显示 `可升级 x.y.z → x.y.w` |
| I-3 | 设备版本 ≥ 固件版本 | 结论显示"版本未提升…需勾选忽略比对"，**开始按钮变灰**（勾 --force 后恢复） |
| I-4 | 会话进行中 | 开始变灰、中止可点、文件框与三开关锁定（防误操作） |
| I-5 | 升级中 | 进度条随 `OTA_PROGRESS` 走（显示 `%` + `KB/s`）；状态行随阶段切换（等待上线/OFFER/下载/自证核对） |
| I-6 | 结束 | 成功 → 进度条 100% + 日志窗 `★ 版本核对通过`；失败 → 状态行"失败"+ 原因；控件全部解锁 |
| I-7 | 中止按钮 | 日志出现 `已发送 OTA_ABORT`，设备侧 `aborted by peer` |

### 3.4 PC 端日志与判据（替代旧"退出码"）

GUI 模式无退出码，判定看**状态行 + 日志窗最后一行**（与 C# 退出码一一对应）：

| GUI 日志/状态 | 等价 C# 退出码 |
|---|---|
| `★ 版本核对通过（x.y.z）→ 升级成功 ✅` | 0 |
| `设备未上线（20s 超时）…` | 2 |
| `设备拒绝: <reason>` | 3 |
| `未收到 OTA_ACCEPT（3 次 OFFER 已用尽）` | 4 |
| `设备重启后 60s 内未恢复心跳 —— 可能已自动回滚` | 5 |
| `设备上报升级失败: <detail>` | 6 |
| `下载停滞判定失败（120s 无进度）/ 600s 超时` | 7 |
| `版本未提升（… >= …），默认不发 OFFER` | 8 |
| `版本核对不一致 … 可能已自动回滚 ❌` | 9 |

> 脚本化判定仍可用 C# 路线的退出码表（见下），但 C# 路线因收包缺陷**不建议**用于正式验收。

### 3.5 C# 路线（备用，已知缺陷）

```bash
cd C:\Users\xiao1\Desktop\esp32\display_audio_photo\tools\pc_now_agent
bin\Release\net8.0-windows10.0.19041.0\pc_now_agent.exe --ota C:\...\build\display_audio_photo.bin
```

⚠ **已知缺陷（§63 源码实证）**：设备回包固定发 PC:50003（`link_manager.c:102` 端口覆写），
而 C# 会话在临时 UDP 口等 ACCEPT ⇒ 收不到任何回包，3 次 OFFER 后必然超时退出。
除非设备侧把回包改为"回 OFFER 源地址"，否则不要用该路线验收。

---

## 4. 验收总表（7 大功能 / 18 个步骤）

| # | 功能 | 步骤 | 一键判据 |
|---|---|---|---|
| A | **正常升级闭环** | 4.1 ~ 4.5 | PC 退出码 0 |
| B | **LVGL 升级浮层（M3）** | 5.1 ~ 5.4 | 屏幕出现进度卡片 |
| C | **自证与回滚** | 6.1 ~ 6.3 | `marked VALID` / 第二次重启回旧槽 |
| D | **6 条校验链（拒绝路径）** | 7.1 ~ 7.5 | `OTA_REJECT reason=...` |
| E | **断点续传（M2）** | 8.1 ~ 8.4 | Range 206 + `RESUME from` |
| F | **坏固件注入（M2）** | 8.3 | `esp_ota_end failed` |
| G | **PC 掉线主动中止（M2）** | 8.6 | `OTA_ABORT` + `aborted by peer` |
| H | **内存纪律与三元组** | 9.1 ~ 9.2 | `OTA_MEM` 基线不劣化 |

---

## 5. 【A】正常升级闭环（5 步）

### A-1. 启动采集串口

```bash
idf.py monitor          # 另开一个终端；先按 Ctrl+] 退出后可用
# 或把日志重定向到文件便于 grep
```

### A-2. 启动 PC 端（V3：GUI）

pc_host「固件升级」页：选 `build\display_audio_photo.bin` → 点**开始升级**。

**预期 GUI 日志窗**（与 C# 版字段一致，前缀 `[HH:MM:SS]`）：

```
==================================================
  固件    : C:\...\build\display_audio_photo.bin
  版本    : a1b2c3d  (display_audio_photo, build 22:45:11)
  大小    : 4980000 B (4.75 MB)
  SHA256  : 3f8a...c21e
[ota] 设备在线: 192.168.1.23，当前版本: 9f3e21a      ← ★ 显示设备现有版本
[ota] 通告 URL: http://192.168.1.100:50005/firmware.bin
[ota] OTA 路由上线：/firmware.json /firmware.bin（a1b2c3d）
[ota] 发送 OTA_OFFER（第 1/3 次）
```

> ✅ **A-2 判据**：出现了 `当前版本: <hex>`（非 `(未上报)`）且**比对结论=可升级**。
> 若显示 `(未上报)` ⇒ 设备固件还是老版本（`HEARTBEAT_ACK` 无 `fw_ver`），
> 本次仍可继续，但**升级后的版本核对会跳过**。

### A-3. OFFER / ACCEPT

**PC 预期**：

```
[ota] 发送 OTA_OFFER（第 1/3 次）
[ota] 设备已接受，开始下载...
```

**设备预期（`grep OTA_DIAG`）**：

```
OTA: [OTA_DIAG] OTA_OFFER 到达          (由 link_manager 分发)
OTA: [OTA_DIAG] OFFER received: ver=a1b2c3d size=4980000 sha=3f8a... url=http://192.168.1.100:50005/firmware.bin
OTA: [OTA_DIAG] check#1 link_manager offline        ← 不应出现
OTA: [OTA_DIAG] check#2 record active ...           ← 不应出现
OTA: [OTA_DIAG] check#3 session mode=...            ← 不应出现
OTA: [OTA_DIAG] check#4 already active              ← 不应出现
OTA: [OTA_DIAG] check#5 running slot ... PENDING    ← 不应出现
OTA: [OTA_DIAG] check#6 ...                          ← 不应出现
OTA: [OTA_DIAG] all 6 checks passed -> target slot ota_1 (5242880 B free)
OTA: [OTA_DIAG] OTA_ACCEPT sent -> downloading
OTA: [OTA_MEM] before-download want=4980000 INT free=... largest=...
OTA: [OTA_MEM] before download INT free=... largest=... | DMA free=... | PSRAM free=...
OTA: [OTA_MEM] http-init-ok want=0 INT free=... largest=... | DMA largest=...
```

> ✅ **A-3 判据**：设备出现 `all 6 checks passed -> target slot ota_1`，
> 且**目标槽 = 当前槽的对侧**（当前 ota_0 ⇒ 目标 ota_1）。

### A-4. 下载与进度

**PC 预期**（进度条原地刷新）：

```
[ota] [########------------------------------]  17%  398 KB/s
...
[ota] [######################################] 100%  412 KB/s
[ota] 设备上报：升级成功，正在重启…
```

**设备预期（关键日志）**：

```
OTA: [OTA_DIAG] http status=200 content-length=4980000 (expect 200, or 206 when Range)
OTA: [OTA_DIAG] download start: offset=0 / 4980000 (0%), seg_done=0
OTA: [OTA_MEM] ota-begin-ok want=4980000 INT free=... largest=... | DMA largest=...
OTA: [OTA_RESUME] seg 1 flushed (262144 B), next_offset=262144, digest=1a2b3c4d5e6f7788...
OTA: [OTA_RESUME] seg 2 flushed (262144 B), next_offset=524288, digest=...
...
OTA: [OTA_DIAG] progress 2% (99584 B, 398 KB/s)
OTA: [OTA_DIAG] progress 4% (199168 B, 400 KB/s)
...
OTA: [OTA_DIAG] progress 100% (4980000 B, 412 KB/s)
```

> ✅ **A-4 判据**：
> 1. `http status=200`（首次整包）；
> 2. 段 flush 日志**每 256KB 一条**，`next_offset` 单调递增到 `size`；
> 3. `progress` 到 100%。

> ★ **§67 攒块**：写槽走 32KB PSRAM staging（读 4KB / 满 32KB 才 `esp_ota_write`），
> `esp_ota_write` 冻结 ext-mem cache 的窗口从 1270 次/包降到 ~160 次/包 ⇒
> 升级期屏闪从"持续闪"降为"偶发轻撕"（结构性不可归零，体验优先级：宁可轻撕不黑屏）。

> ★ **排错提示**：若出现 `[OTA_ERR] http read error (-28672) ...`（`-ESP_ERR_HTTP_EAGAIN`），
> 那是**旧版本代码**的表现 —— 本版已单独分支处理该重试信号（不会误杀）。
> 若仍见错误，查 PC 是否被 Clash TUN 劫持（§1.2）。

### A-5. 校验、切槽、重启、转正

**设备预期**：

```
OTA: [OTA_DIAG] download complete, verifying (esp_ota_end)...
OTA: [OTA_RESUME] tail seg 20 flushed (fill=65536 B), digest=...      ← 尾巴段
OTA: [OTA_DIAG] esp_ota_end OK (image header + segment CRC valid)
OTA: [OTA_RESUME] digest chain: seg_count=20 (expect 20), path=FULL
OTA: [OTA_RESUME] seg[0]=1a2b3c4d...
OTA: [OTA_RESUME] seg[1]=...
...
OTA: [OTA_RESUME] whole-sha expected=3f8a...c21e (段链已覆盖)
OTA: [OTA_DIAG] set_boot_partition(ota_1) OK
OTA: [OTA_RESUME] cleared                                          ← 成功 → 断点作废
OTA: [OTA_DIAG] OTA OK, rebooting in 1000 ms
```

**重启后（自证窗口 ~10s）**：

```
OTA: [OTA_DIAG] init done: running=ota_1 next_update=ota_0 pending_verify=1
...
OTA: [OTA_DIAG] self-test OK: app marked VALID (rollback cancelled), run=ota_1
```

**PC 预期**：

```
[ota] 设备已回来，IP=192.168.1.23，fw_ver=a1b2c3d
[ota] ★ 版本核对: 期望 a1b2c3d / 实际 a1b2c3d → 升级成功 ✅
```

> ✅ **A-5 判据（4 个必须全中）**：
> 1. `esp_ota_end OK`；
> 2. `digest chain: seg_count=N (expect N)` **两数相等**；
> 3. `set_boot_partition(ota_X) OK` 且 X 已换到对侧；
> 4. 重启后自证窗口（~10s，30s 兜底）内 `self-test OK: app marked VALID`，PC 显示 `升级成功 ✅`（退出码 0）。

---

## 6. 【B】LVGL 升级浮层（M3 浮层，4 步）

浮层 = 全屏 70% 遮罩 + 居中卡片 560×300：
标题「固件升级」/ 版本行 `vA -> vB` / 青色进度条 / 状态文本 / 红字「请勿断电、勿断网」。

### B-1. 浮层出现

**观察**：设备屏上**在收到 `OTA_ACCEPT` 后立即出现**遮罩卡片。

**设备预期（`grep OTA_UI`）**：

```
OTA_UI: [OTA_UI] card built (mask 800x480, box 560x300), hidden
OTA_UI: [OTA_UI] init done (evt_cb subscribed, tick=300ms)
OTA_UI: [OTA_UI] evt=0 pct=0 detail='ota_1'
OTA_UI: [OTA_UI] overlay SHOWN
```

> ✅ **B-1 判据**：`card built`（开机时）+ `overlay SHOWN`（升级开始时）。

### B-2. 版本行正确

**观察**：卡片第二行显示 `<当前版本>  ->  <OFFER 版本>`，例如 `9f3e21a  ->  a1b2c3d`。

> ✅ **B-2 判据**：箭头左侧是**设备当前**版本（`ota_service_get_running_version`），
> 右侧是**本次 OFFER** 的 ver（`ota_service_get_offer_version`）。两者应不同。

### B-3. 进度条 + 警示语闪烁

**观察**：
1. 进度条随 PC 端同步增长；
2. 状态文本变化：`已接受请求，开始下载…` → `下载中 42%` → `校验固件完整性…` → `升级成功，正在重启…`；
3. 红字「请勿断电、勿断网」在**下载中闪烁**（300ms 相位）。

> ✅ **B-3 判据**：进度条百分比与 PC 端 `--ota` 进度条**同步**（浮层略滞后 ≤0.3s）。

### B-4. 触摸被拦截 + 失败自动收摊

**操作**：升级过程中点击屏幕任意位置。

> ✅ **B-4 判据**：
> 1. 点击**不穿透**（不切 tab、不触发底层按钮）；
> 2. 若升级失败/中止 → 浮层显示 `升级失败，断点已保留` / `升级已取消`，
>    **约 3.6s 后自动隐藏**，日志出现 `overlay HIDDEN`。

> ★ **线程纪律验证**：浮层刷新全部在 `lv_timer`（taskLVGL）；
> `ota_task` 只写 `s_pending_*`。若出现 **LVGL 断言 / 花屏 / TaskWDT**，
> 第一嫌疑就是有人在 `evt_cb` 里碰了 LVGL —— 查 `overlay_ota.c` 的 `ota_ui_evt_cb()` 是否只有赋值语句。

---

## 7. 【C/D】自证、回滚 与 校验链拒绝路径

### C-1. 自证成功（正常路径）

见 A-5。**补充判据**：自证后再手动按复位键一次，**应仍进 ota_1**（不回滚）。

```
OTA: [OTA_DIAG] init done: running=ota_1 ... pending_verify=0     ← 转正后为 0
```

### C-2. 自证失败 → 自动回滚（★ 防砖核心验证）

**构造坏固件**（在 `main.c` 里让自证 tick 直接返回）：

```c
// 临时改：main.c 的 ota_selftest_tick_cb() 开头直接 return;
// （现状 = 1s 周期 esp_timer，条件驱动 mark_valid_if_pending；让 tick 永不转正即可）
```

编译后 USB 烧进 **ota_0**（当前槽），然后 GUI 勾「忽略版本比对」推到 ota_1。

**预期时间线**：

```
[第一次重启] running=ota_1  pending_verify=1
             （自证窗口内没有任何 mark VALID）
             ↓ 手动按复位
[第二次重启] bootloader 自动回滚 → running=ota_0
             fw_ver 变回旧值
```

**设备预期（`grep rollback`）**：

```
boot: ... Rollback: current app is not verified, switching to ota_0
```

> ✅ **C-2 判据**：第二次重启后 `esp_ota_get_running_partition()` 回到 **ota_0**，
> PC 收到的心跳 `fw_ver` 是**旧版本**。

> ⚠ **这个测试必须做一次**：它是"OTA 不会变砖"的唯一硬证据。
> 做完记得**把自证定时器改回来**并重烧。

### C-3. 自证条件严格性（link 在线）

**操作**：升级成功后，**断开 PC 端**（设备失去 link 对端），然后在自证窗口内观察。

**设备预期**：

```
OTA: [OTA_DIAG] PENDING_VERIFY but wifi=1 link=0 -> NOT marking valid
     (bootloader will roll back on next boot)
```

> ✅ **C-3 判据**：`wifi=1 link=0` ⇒ **不转正**。说明自证条件（WiFi 已连 **且** link 在线）
> 两个都真的生效（M3 增强点）。

### D-1 ~ D-5. 6 条校验链（拒绝路径）

逐个构造，每次 PC 端应打印 `OTA_REJECT` 与 reason：

| 步骤 | 制造方法 | 期望 reason | 设备日志 |
|---|---|---|---|
| **D-1** | 设备录音中（REC tab 按一次）发 OFFER | `busy` | `check#2 record active` |
| **D-2** | 设备通话/推流中发 OFFER | `busy` | `check#3 session mode=N` |
| **D-3** | 升级进行中再发一次 OFFER | `busy` | `check#4 already active` |
| **D-4** | OTA 成功后**转正前**立即再推（~10s 窗口内） | `pending` | `check#5 running slot ota_1 is PENDING_VERIFY` |
| **D-5** | 推 6MB 的 bin（人脸分支残留物） | `space` | `check#6 image 6837312 > slot 5242880` |
| **D-6** | 关掉 PC 端 link 或拔设备网 | `network` | `check#1 link_manager offline` |

**设备预期（统一格式）**：

```
OTA: [OTA_DIAG] REJECT reason=busy
```

**PC 预期**：

```
[ota] 设备拒绝: {"type":"OTA_REJECT","reason":"busy"}
       reason 含义: busy=录音/通话中  pending=待自证  space=槽不够  network=离线  badargs/badjson=参数问题
```

> ✅ **D 判据**：每条都命中**对应** reason（不是别的）。

---

## 8. 【E/F/G】断点续传、坏固件、主动中止（M2 三项新能力）

> ★★ **E 组语义已按 §67 实弹结论降级（2026-10-07）**：
> `esp_ota_begin` 在 begin 时**整槽擦除**（实测 5MB≈3.5s）、`esp_ota_write` 无偏移参数（顺序写）、
> 首写必过 0xE9 魔数检查（`esp_ota_ops.c:371-377`）⇒ **设备端写槽续传在 esp_ota_* API 下不可行**。
> 现行为：检测到旧断点 → 清空 + W 日志 `降级全新下载`，**PC 侧 Range/206 保留但设备从头重下**；
> 断点记录（段哈希）仍落 nvs，为将来 esp_partition 裸写自管擦除的真续传留接口。
> 下文 E-1~E-4 的 RESUME 判据**保留为设计文档**（真续传实现时启用），当前验收以"降级全新下载"为准；
> 下载总时长由 PC 端 600s 绝对超时 + 120s 活动超时兜底（§68，29KB/s 慢链路不再被误杀）。

### E-1. 制造"下不完"（GUI：只发前 40%）

「只发前 [40] %」数字框 → 开始升级（V3 GUI；等价旧 `--ota-limit 40`）。

PC 会只发 40% 的字节，且 `OTA_OFFER.size` 与 `/firmware.json` 都报**截断长度**。

**PC 预期**：

```
  ★ 测试开关: limit 40% → 只发 1992000 B
...
[ota] 设备上报 RESULT: ok=False detail=failed
```

**设备预期**：

```
OTA: [OTA_DIAG] OFFER received: ver=... size=1992000 ...
OTA: [OTA_DIAG] all 6 checks passed -> target slot ota_1 (...)
OTA: [OTA_RESUME] fresh download (resume rec magic=0x00000000 size=0 off=0)
OTA: [OTA_RESUME] seg 1 flushed (262144 B), next_offset=262144, ...
...
OTA: [OTA_ERR] size mismatch: got 1992000, expect 1992000 (offset kept for resume)
```

> ⚠ 注意：**`--ota-limit` 的 size 是截断后的**，所以设备端 `total == expect_size` 会成立，
> 失败点会落到 **`esp_ota_end`（镜像不完整）**，而不是 size mismatch：
> ```
> OTA: [OTA_ERR] esp_ota_end failed (image invalid: 镜像头/段 CRC 不过)
> ```
> 两种表现都正确 —— **关键是断点被保留**：

```
OTA: [OTA_RESUME] keeping resume record for retry (next_offset=1992000)
```

### E-2. 立即重推（数字框归 0）→ 应续传

「只发前 [0] %」→ 开始升级（等价旧不带 limit 的重推）。

**设备预期（★ 本项核心）**：

```
OTA: [OTA_RESUME] found: total=4980000 done=1992000 seg_done=7 sha=3f8a...c21e
OTA: [OTA_RESUME] RESUME from 1835008 B (seg_done=7, rollback from 1992000 for hash)
OTA: [OTA_DIAG] http status=206 content-length=3146992 (expect 200, or 206 when Range)
OTA: [OTA_DIAG] download start: offset=1835008 / 4980000 (36%), seg_done=7
OTA: [OTA_RESUME] seg 8 flushed (262144 B), next_offset=2097152, ...
...
OTA: [OTA_DIAG] set_boot_partition(ota_1) OK
```

**PC 预期**：

```
[ota] http /firmware.bin GET range=1835008- (status=206, len=3146992)
[ota] served firmware.bin: 3146992 B (offset=1835008, full) 请求 #2
```

> ✅ **E-2 判据（4 个必须全中）**：
> 1. `found: total=... done=...`（nvs 断点被读到）；
> 2. `RESUME from <段边界> B (seg_done=N, rollback from <旧offset>)` —— **回退到段边界**；
> 3. **PC 回 `206` + `Content-Range`**（`status=206`）；
> 4. `download start: offset=<段边界>` 且进度从 ~36% 起（**不是 0%**）。

### E-3. 段边界回退的数学正确性

**验证**：`RESUME from` 的偏移应等于 `seg_done × 262144`。

```
seg_done=7 → 7 × 262144 = 1835008   ✅ 与日志一致
```

> ✅ **E-3 判据**：`start_off == seg_done × OTA_SEG_SIZE`。
> 这是"**只重算 ≤256KB 尾巴**"的依据 —— 若回退量 > 256KB，说明段逻辑有 bug。

### E-4. 服务端不支持 Range 时自动降级

**构造**：把 PC 端 Range 分支临时注释（模拟老服务端），或用一个只回 200 的简易 HTTP 服务。

**设备预期**：

```
OTA: [OTA_DIAG] http status=200 content-length=4980000 (expect 200, or 206 when Range)
OTA: [OTA_RESUME] server ignored Range (200) -> downgrade to FULL download
OTA: [OTA_DIAG] download start: offset=0 / 4980000 (0%), seg_done=0
```

> ✅ **E-4 判据**：**降级整包**（offset 归 0），不会出现"槽内容错位"导致的
> `esp_ota_end failed`。

### F-1. 坏固件注入（GUI：勾「注入 1bit 损坏」）

勾选「注入 1bit 损坏」→ 开始升级（等价旧 `--ota-corrupt`）。

**PC 预期**：

```
  ★ 测试开关: corrupt → 流中点翻 1 bit
...
[ota] ★ CORRUPT 注入 @ 流偏移 2490000（预期设备端 esp_ota_end / 段哈希报错）
```

**设备预期（二选一，都算正确）**：

```
# 路径 1：镜像段 CRC 不过（最常见）
OTA: [OTA_ERR] esp_ota_end failed (image invalid: 镜像头/段 CRC 不过)

# 路径 2：段数一致性校验拦截
OTA: [OTA_ERR] segment count mismatch: got N expect M
```

> ✅ **F-1 判据**：**升级必须失败**，且设备**仍运行在旧槽**（不变砖）。
> PC 端应显示 `升级失败`（退出码 6）。
>
> ★ 这条验证的是 `esp_ota_end` 的镜像校验**真的在起作用**。

### G-1. PC 掉线 → 设备主动中止？（实为 PC 主动发 ABORT）

**构造**：升级进行中，**在设备侧拔掉网线 / 关 WiFi**（设备心跳停）。

**PC 预期**（GUI 日志窗；侧边栏状态点同步变灰）：

```
[ota] ★ 设备心跳丢失 >8s，已发送 OTA_ABORT（设备应立即停手）
```

> ⚠ 设备已断网，收不到 ABORT —— 这条测试的真实价值是**验证 PC 侧的检测与发包逻辑**。

### G-2. 中止真的生效（★ 在设备侧构造）

**构造**：升级中，从**设备侧**触发 ABORT（或用 `--device <ip>` + 手动发 UDP 包）：

```python
# 在 PC 上模拟"对端主动中止"
import socket, json
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.sendto(json.dumps({"type":"OTA_ABORT","reason":"test"}).encode(), ("192.168.1.23", 50003))
```

**设备预期**：

```
OTA: [OTA_DIAG] OTA_ABORT received -> s_abort=1 (effective at next chunk)
OTA: [OTA_DIAG] abort flag seen at 1234567 B -> stop reading
OTA: [OTA_DIAG] esp_ota_abort called (slots left untouched)
OTA: [OTA_DIAG] OTA ABORTED at 24% (stayed on ota_0)
```

**浮层预期**：显示 `升级已取消` / `设备未改动`，3.6s 后隐藏。

> ✅ **G-2 判据（3 个必须全中）**：
> 1. `OTA_ABORT received -> s_abort=1`（**立即置标志**）；
> 2. `abort flag seen at <offset> B -> stop reading`（**块边界真停了**，不是等下载完）；
> 3. `stayed on ota_0`（**当前槽未变**）。
>
> ★ 关键区分点：`abort flag seen at` 的偏移应**明显小于**总大小（例如 24% 处就停）。

### G-3. 中止后断点是否保留

**设备预期**：

```
OTA: [OTA_RESUME] ABORT → 断点保留与否？
```

> 设计决策：**ABORT 属用户主动取消**，本版**不打印 `keeping resume record`**
> （`if (!ok && !s_abort)` 条件），但 nvs 里的段边界记录**仍在**（最后一次 `resume_save`）。
> ⇒ 下次同版本 OFFER 仍可续传。这是**可接受的**（省一次重下），如需彻底清断点
> 可调 `ota_service_clear_resume()`。

---

## 9. 【H】内存纪律与三元组基线（2 步）

### H-1. 三元组打点位置（本版共 7 处）

所有 `[OTA_MEM]` 行格式：

```
OTA: [OTA_MEM] <label> want=<N> INT free=<N> largest=<N> | DMA largest=<N>
OTA: [OTA_MEM] <label> INT free=<N> largest=<N> | DMA free=<N> | PSRAM free=<N>
```

| 打点 | label | 时机 | 关注 |
|---|---|---|---|
| 1 | `after ota_service_init` | 初始化后 | 常驻开销（应为 0，仅建任务+队列） |
| 2 | `before-download want=<size>` | 下载前 | **want = 固件大小**（说明不是一次性分配！） |
| 3 | `before download` | 同上（五元） | 基线 |
| 4 | `http-init-ok` / `http-init-FAIL` | HTTP 客户端创建 | 失败必有 FAIL 行 |
| 5 | `ota-begin-ok` / `ota-begin-FAIL want=<size>` | `esp_ota_begin` | 失败必打 **want** |
| 6 | `after-download` | 结束后 | 应回到基线 |
| 7 | `after download (back to IDLE)` | 同上（五元） | **对比 #3，验证零泄漏** |

**另有失败路径的三元组**（规范 A4）：

```
OTA: [OTA_ERR] OFFER copy alloc failed: want=NNN INT free=... largest=... | PSRAM free=... largest=...
```

> ✅ **H-1 判据**：`after download (back to IDLE)` 的 `INT free` / `largest`
> 应 ≈ `before download` 的值（**差值 < 2KB**）。若差很多 ⇒ 有泄漏，查 `goto out` 路径。

### H-2. 内存纪律检查清单（代码级，对照 13 号文档）

| 规范条 | 本实现 | 怎么验 |
|---|---|---|
| **S1 静态池** | 4KB 读缓冲 `s_rd_buf[4096]` 走 `.bss` | `nm build/display_audio_photo.elf \| grep s_rd_buf`（应在 `.bss`） |
| **C1 默认 PSRAM** | OFFER JSON 拷贝走 `heap_caps_malloc(SPIRAM)` | 代码审查（`ota_service.c:259`） |
| **C3 栈外置 WithCaps** | `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM\|8BIT)` | 代码审查（`:298`） |
| **C5 caps 配 caps** | `heap_caps_free(cmd.json)`（不是 `free()`） | 代码审查（`:877`） |
| **T5 外置栈失败点名** | 失败日志含 `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` | 代码审查（`:302`） |
| **A2 运行期禁 assert** | 下载/校验全走 `goto out` | 只有 `ota_service_init()` 有 `assert`（初始化期，允许） |
| **A3 用户可见功能必降级** | 失败回 IDLE，不 abort 系统 | 见 F-1 / G-2 |
| **A4 alloc 失败打三元组** | 3 处失败路径 | 见 H-1 |
| **L1 生命周期** | `resume_clear()` on success；`esp_ota_abort` on failure | 见 A-5 / G-2 |

### H-3. BOOT_DIAG 对照（开机内存账单）

`main.c` 的 `[boot-diag]` 仍逐阶段打印。**关注两条新增**：

```
main: [boot-diag] after ota_service_init        internal free=... largest=...
main: [boot-diag] after ota selftest timer armed internal free=... largest=...
```

> ✅ **H-3 判据**：`after ota_service_init` 相对 `after pcnow_service_init` 的
> 内部 RAM 消耗应为 **~0**（只多了 1 个任务栈在 PSRAM + 1 个 4 项队列 + 1 个 12KB 静态结构）。
> ★ 但注意 **`s_resume`（`ota_resume_t` ≈ 24+32 段摘要 = 800 B）走 `.bss`**，
> 会从内部 RAM 切走 —— 属"缩 .bss 才能加堆"的口径，量很小（<1KB）。

---

## 10. 日志关键字速查（★ 排错主场）

```bash
# 设备端（idf.py monitor 输出重定向到文件后）
grep "OTA_DIAG"   ota.log     # 分阶段进度 + 校验链 + 三元组五元
grep "OTA_ERR"    ota.log     # 所有错误（含错误码/偏移/槽名）
grep "OTA_MEM"    ota.log     # 纯内存打点
grep "OTA_RESUME" ota.log     # 断点/段/Range/摘要链
grep "OTA_UI"     ota.log     # 浮层（卡片/显隐/事件消费）
grep "rollback"   ota.log     # bootloader 回滚
grep "boot-diag"  ota.log     # 开机内存账单
```

| 关键字 | 一行示例 | 看到它说明 |
|---|---|---|
| `OTA_DIAG` | `all 6 checks passed -> target slot ota_1` | 校验通过、开始下载 |
| `OTA_DIAG` | `progress 42% (2097152 B, 400 KB/s)` | 下载中（每 2%） |
| `OTA_DIAG` | `set_boot_partition(ota_1) OK` | 已切槽 |
| `OTA_DIAG` | `self-test OK: app marked VALID` | 转正成功 |
| `OTA_ERR` | `esp_ota_end failed (image invalid...)` | 镜像校验不过（坏固件/截断） |
| `OTA_ERR` | `segment count mismatch: got N expect M` | 段数不符（size 与内容不符） |
| `OTA_RESUME` | `RESUME from 1835008 B (seg_done=7, ...)` | 续传生效 |
| `OTA_RESUME` | `server ignored Range (200) -> downgrade to FULL` | 服务端不支持 Range，降级 |
| `OTA_UI` | `evt=1 pct=42 detail=''` | 浮层消费了一个进度事件 |
| `OTA_UI` | `overlay SHOWN` | 卡片已显示 |
| `rollback` | `Rollback: current app is not verified...` | bootloader 自动回滚 |

---

## 11. 排障速查

| 现象 | 原因 | 处理 |
|---|---|---|
| PC 卡在"等待设备心跳" | 不同网段 / 防火墙 / 设备未联网 | 查设备 `LINK: ONLINE`；放行 UDP 50003 |
| PC 立即报版本未提升（rc=8） | 设备 `fw_ver >=` 固件版本 | 加 `--force`，或给固件提版本号 |
| `OTA_REJECT reason=busy` | 设备在录音/通话/推流 | 停掉对应功能重发 |
| `OTA_REJECT reason=pending` | 设备在自证窗口内（转正前 ~10s） | 等转正后重发 |
| `OTA_REJECT reason=space` | bin 超 5MB 槽 | 确认 build 是 main 分支的（`idf.py fullclean` 重建） |
| `OTA_REJECT reason=network` | link 未在线 | 等设备联网 |
| `http status=502` / 设备解析失败 | **PC 上 Clash TUN 劫持 LAN 入站** | 私有网段 `DIRECT` 或关 TUN |
| PC 回 200 而非 206（期望续传） | PC 端代码未更新 / 简易服务端 | 重新 `dotnet build`；见 E-4 |
| 下载中断后 PC 重跑但设备从 0% 起 | nvs 断点被 size/sha 不匹配作废 | 正常（换了固件）；同固件应续传 |
| 浮层不出现 | `overlay_ota_init()` 未调用 / 无事件 | 查 `OTA_UI: init done`；查 `evt=` 日志 |
| 浮层出现但进度不动 | `lv_timer` 未创建 / taskLVGL 被饿死 | 查 `tick=300ms`；查是否 LVGL 线程被长任务占住 |
| 出现 LVGL 断言 / 花屏 | ★ **有人在 `evt_cb` 碰了 LVGL** | 查 `overlay_ota.c:ota_ui_evt_cb()` 是否只有赋值 |
| 设备重启后 60s 未回心跳 | 可能已回滚 / 未起来 | 查串口 `rollback`；兜底 USB 重烧 |
| `keeping resume record` 反复出现 | 固件本身有问题（每次都在同一偏移失败） | 检查 PC 侧文件完整性（`sha256sum`） |

---

## 12. 常见问题

**Q：能降级吗？**
能，但**默认会拦**。PC 端用 SemVer 粗比：设备版本 ≥ 固件版本时**拒发**（rc=8）。
要降级/同版本重刷 → 加 `--force`。

**Q：OTA 时能听歌吗？**
能。本地/蓝牙放歌不写内部 Flash，OTA 期间只是可能卡顿（预期）。
但**同时录音不行**（`OTA_REJECT busy`，两者抢 flash）。

**Q：升级失败会变砖吗？**
不会。**四道防线**：
① `esp_ota_end` 做镜像头 + 段 CRC 校验；
② 段摘要链 + 段数一致性硬校验；
③ 自证（WiFi + link，1s 周期条件驱动 ~10s 转正，30s 兜底判死）+ bootloader 自动回滚；
④ 最坏情况 USB 全量重刷（单 app 槽布局的兜底）。
★ 已实弹验证：升级成功后 ~11s 硬件断电（转正前）→ 正确回滚旧槽（§68 复盘，§69 修复后窗口缩短）。

**Q：断电了怎么办？**
断点（nvs）已落盘到**最近 256KB 段边界**。⚠ 当前版本**设备端续传已降级为全新下载**（§67，
esp_ota API 证伪）：重推后从 0 开始下，PC 端 600s/120s 超时兜底慢链路。

**Q：`storage` 分区会被 OTA 覆盖吗？**
不会。OTA 只写 app 槽（ota_0/ota_1 对侧），`storage`/`nvs` 完全不碰。

**Q：怎么给固件起版本号？**
改 `main/CMakeLists.txt` 的 `project(... VERSION x.y.z)`；当前是 git 短 hash。
版本号烧进 `esp_app_desc_t.version`，也是 `HEARTBEAT_ACK.fw_ver` 的来源。

**Q：怎么清掉断点？**
正常流程成功切槽会自动清（`[OTA_RESUME] cleared`）。
调试时可临时调 `ota_service_clear_resume()`（已导出）。

**Q：`--ota-limit` 到底测什么？**
测**断点保留 + 续传**。它让 PC 少发字节，设备必然在 `esp_ota_end` 失败 ⇒
关键是失败后 nvs 断点**不被清**，重推时能从段边界续上。

---

## 13. 里程碑现状（M1~M4）

| 阶段 | 内容 | 状态 |
|---|---|---|
| **M1** | 闭环（OFFER/ACCEPT、HTTP 拉取、SHA256、切槽、自证） | ✅ 已完成（§61） |
| **M2** | `OTA_ABORT` 真中断 + HTTP Range 续传 + 256KB 分段哈希 + 断点保留 | ✅ 已完成（§61b；设备端写槽续传 §67 降级，见 E 组头注） |
| **M3** | LVGL 升级浮层 + 自证增强（WiFi **且** link） + PC 破坏性开关 | ✅ 已完成（§61b） |
| **实弹** | v1.0→v1.4 五连升全链路 + 计划外断电回滚实测 | ✅ **通过（2026-10-08，§15）** |
| **M4** | `firmware/` 版本归档 + 多设备批量 | ⏳ 未开工 |

---

## 15. 实弹验收记录（2026-10-07/08，版本线 v1.0 → v1.4）

**结论：全链路通过。** 五次真机 OTA（v1.0→v1.1→v1.2→v1.3→v1.4）逐版验证，
期间计划外踩出的四个真问题全部修复并回归：

| # | 实弹暴露的问题 | 根因（源码级） | 修复 | 回归 |
|---|---|---|---|---|
| 1 | OFFER 即 panic（`esp_cache_utils.c:96`） | ota_task 栈 WithCaps 外置 PSRAM；IDF 冻结 ext cache 前断言任务 SP 在内部 DRAM，**无 API 豁免** | 任务栈内化（`xTaskCreatePinnedToCore` 8192，§65） | ✅ |
| 2 | 断点续传 `invalid magic byte (0xE9)` | esp_ota_begin 整槽擦 + write 顺序写 + 首写魔数检查 ⇒ **写槽续传 API 级不可行** | 降级全新下载（§67），真续传留 esp_partition 裸写后续 | ✅ |
| 3 | 设备明明成功却"升级失败" | PC 端 180s 绝对超时，5.2MB@29KB/s 慢链路被误杀 | 600s 上限 + 120s 活动超时（§68） | ✅ |
| 4 | 升级成功后断电 → 回滚旧版本 | 自证窗口（34s 判定）内断电 = 判死 | 1s 周期条件驱动，浸泡 ~10s 即转正（§69） | ✅ |

**覆盖验收组**（对照 §4 总表）：

- **A 正常闭环**：✅ v1.0→v1.4 五连升，`esp_ota_end OK` / 段数一致 / 切槽 / `marked VALID` / PC `★ 版本核对通过`；
- **B LVGL 浮层**：✅ 遮罩卡 + 进度同步 + 触摸拦截；
- **C 自证回滚**：✅ **计划外实测**——v1.2 成功后 ~11s 硬件断电，POWERON 复位正确回退 v1.1（防砖机制真起效）；
- **D 校验链**：✅ 版本未提升拒发（PC SemVer）、`pending`/`space` 路径命中；
- **E 续传**：⚠ 按降级语义验收（全新下载 + PC 超时兜底），真续传挂账；
- **F 坏固件**：✅ `esp_ota_end failed`，停留旧槽不变砖；
- **G 中止**：✅ `OTA_ABORT` 块边界真停；
- **H 内存**：✅ `OTA_MEM` before/after 三元组差值 < 2KB，零泄漏；
- **附带**：升级期屏闪缓解（32KB 攒块，§67）；屏偏移与 WiFi NVS 写审计（§69补）。

**遗留挂账**：真续传（esp_partition 裸写 + 自管擦除）、屏幕偏移根治（等 IDF S31 restart 支持）。
**版本机制备忘**：`PROJECT_VER = git describe`（configure 期取值）——tag 后必须 fullclean，
bin 版本串自检：`python -c "d=open(r'build\display_audio_photo.bin','rb').read(512); print(d[0x30:0x50].split(b'\x00')[0].decode())"`。

---

## 14. 一键验收脚本（可选）

把常用检查串起来（在 ESP-IDF 终端跑）：

```bash
#!/bin/bash
# 前提：当前在 main 分支，且刚 idf.py build 过
set -e

echo "── 1. 回滚开关 ──"
grep "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y" sdkconfig

echo "── 2. 固件尺寸（必须 < 5242880）──"
ls -la build/display_audio_photo.bin

echo "── 3. 固件版本（esp_app_desc_t.version @ 0x30）──"
python -c "b=open('build/display_audio_photo.bin','rb').read(256);print(b[0x30:0x50].split(b'\x00')[0].decode())"

echo "── 4. 固件 SHA256（与 PC 端 --ota 打印值核对）──"
sha256sum build/display_audio_photo.bin

echo "── 5. 分区表确认 ota_0/ota_1 各 5MB ──"
python -c "import re;[print(l.rstrip()) for l in open('partitions.csv') if 'ota' in l]"
```

---

> **改动记录**：§61（M1）+ §61b（M2/M3）+ §65~§69（实弹修复）+ §71（PC 端 v0.4.3）
> **协议**：`docs/项目规划/通信接口规范_ICD.md` §8（V1.4）
> **内存纪律**：`docs/工程规范/13_内存管理规范.md` + `14_堆管理宝贵经验.md`
