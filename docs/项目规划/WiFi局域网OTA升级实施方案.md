# WiFi 局域网 OTA 升级实施方案（可执行版）

> 版本 V1（2026-09-23）· 对应《需求文档》§11 阶段六
> ✅ **状态（2026-10-08 更新）：已全部实现、实弹验收并随 v2.0 发布版定稿** ——
> 本文为设计原稿，现状以《WiFi_OTA_烧录验收与使用流程.md》（V3.3）+ ICD §8/§10 +
> 《改动记录.md》§61~§79g 为准。相对本文的主要演进：PC 路线定为 **pc_host GUI**（C# --ota 降级备用）；
> 断点续传升级为 256KB 段哈希**真续传**（§72，实机回归通过）；新增**双向 OTA**
> （心跳 fw_latest + OTA_REQ/OTA_NOUPDATE）与**手动切槽**（§79/§79e）；
> 刷新模式 bb→**AUTO** 根治升级期屏偏移（§79g / v2.0）。开工前先看 §0 的端口偏差表（仍有效）。
> 基线：pcnow_service 副屏工程（`link_manager` 心跳 + **`pc_host/` v2 上位机**已上线；
> C# 版 `tools/pc_now_agent` 为等价备选，二选一）
> 目标：PC 一条命令把 `build/*.bin` 推进设备备用槽，自动校验、切槽、重启、自证，坏固件自动回滚。

---

## 0. 与需求文档 §11 的偏差（先读，避免对着旧文档找端口）

需求文档 §11 成稿于副屏功能之前，以下 4 点**以本文档为准**：

| # | 需求文档 §11 原设计 | 本方案（现状修正） | 原因 |
|---|---|---|---|
| 1 | OTA 数据走 **TCP 50004**，PC 主动推送 | **设备从 PC 的 HTTP 50005 拉取**（新增 `/firmware` 路由） | ① 50004 已被 pcnow_service 副屏推送占用；② 设备侧已有现成 `http_get()`（esp_http_client），PC 侧已有现成 TcpListener 手写 HTTP（免 URLACL/管理员权限），**两边各加一个路由/一个拉取函数即可闭环**，不必新写 TCP 分帧协议 |
| 2 | ota_0/ota_1 各 3MB @0x20000/0x320000，storage @0x620000 | 实际 `partitions.csv`：**ota_0@0x20000 5MB / ota_1@0x520000 5MB / otadata@0x11000 / storage@0xA20000 0x5E0000(5.875MB)** | 分区表早已按 5MB 落地，app bin≈4.88MB，槽余量 ≈130KB，勉强够用（见 §7 风险） |
| 3 | PC 上位机为 PySide6 `pc_host/`（OTA Tab 拖拽） | **M1 先扩展 C# 控制台 `tools/pc_now_agent` 加 `--ota` 模式**；GUI 升级页落在统一上位机 `pc_host` v2 的"系统"页（M4，该页已预留卡片） | 2026-09-23 晚 pc_host v2 已上线（PySide6 多页框架就绪），但命令行先行仍是最快闭环路径；两条路共用同一协议，不返工 |
| 4 | 断点续传存 nvs offset | M1 不做续传（重下整包，4.9MB 局域网 <10s）；**nvs 断点 + HTTP Range 列 M2** | 复杂度后置，先验证主链路 |

**不变的部分**（照 §11 执行）：esp_ota 原生 A/B 切槽、整包 SHA256、`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` 原生回滚、自证后 `esp_ota_mark_app_valid_cancel_rollback()`、升级期间与录音/通话互斥。

---

## 1. 总体架构

```
┌─────────── PC（tools/pc_now_agent --ota fw.bin）───────────┐
│ ① 解析 bin（esp_app_desc_t@0x20 → version）现算 SHA256/大小  │
│ ② 心跳 50003 已知设备 IP → 发 OTA_OFFER{ver,size,sha256,url} │
│ ③ HTTP :50005 新增路由 GET /firmware.bin（/firmware.json）   │
│ ④ 收 OTA_PROGRESS(每2%) 打印进度条；收 OTA_RESULT 收尾        │
└──────△──────────────────────────────│──────────────────────┘
       │ JSON (UDP 50003 双向，复用 link_manager)          │ HTTP GET
       │                                                  ▼
┌──────┴──────────── 设备（新增 middleware/network/ota_service）────┐
│ OTA_OFFER → 校验（联网/非录音/非通话/槽状态）→ OTA_ACCEPT          │
│ → esp_http_client GET http://<pc>:50005/firmware.bin 流式读取     │
│   4KB/块 → mbedtls SHA256 边收边算 → esp_ota_write(备用槽)        │
│   每 2% → OTA_PROGRESS JSON 回传（link_manager_send_json）        │
│ → esp_ota_end（镜像校验）→ SHA256 比对 → esp_ota_set_boot_partition│
│ → OTA_RESULT{ok} → 1s 后 esp_restart                             │
│ 重启进新固件 → PENDING_VERIFY → 30s 稳定自证 → mark_valid         │
│                    （失败则 bootloader 自动回滚旧槽）              │
└──────────────────────────────────────────────────────────────────┘
```

**数据流向**：控制面 = 既有 UDP 50003 JSON（零新增端口）；数据面 = 设备主动 HTTP 拉（复用 50005）。设备是 HTTP 客户端、PC 是服务端——与副屏封面/歌词拉取完全同构。

---

## 2. 端口与消息（ICD V1.4 增补，实施时并入 §6 之后作 §7）

端口总表（现有 + 本次）：

| 端口 | 协议 | 方向 | 用途 |
|---|---|---|---|
| 50003 | UDP | 双向 | 心跳/信令（既有），OTA 控制消息也走这里 |
| 50004 | UDP | PC→设备 | NOW_* 副屏推送（既有，**与 OTA 无关**） |
| 50005 | TCP | 设备→PC | HTTP 拉取：既有 /cover /lyrics + 新增 /firmware* |

### 2.1 OTA 控制消息（UDP 50003，`type` 字段区分，cJSON）

| type | 方向 | 字段 | 说明 |
|---|---|---|---|
| `OTA_OFFER` | PC→设备 | `ver`(目标版本串) `size`(字节数) `sha256`(hex64) `url`(完整 http://ip:50005/firmware.bin) | 升级提议 |
| `OTA_ACCEPT` | 设备→PC | `size` | 接受，即将开始拉取 |
| `OTA_REJECT` | 设备→PC | `reason`(busy/pending/space/network) | 拒绝原因 |
| `OTA_PROGRESS` | 设备→PC | `percent`(0-100) `speed`(KB/s) | 每 2% 或每 2s 一条 |
| `OTA_RESULT` | 设备→PC | `ok`(0/1) `detail` | ok=1 后设备 1s 内重启；PC 靠心跳 fw_ver 复核 |
| `OTA_ABORT` | PC→设备 | — | 中止（M1 仅在下载中生效，设备擦掉半成品回 IDLE） |

### 2.2 HTTP 路由（PC :50005，TcpListener 新增两条）

| 路由 | 响应 | 说明 |
|---|---|---|
| `GET /firmware.json` | 200 JSON `{"ver","size","sha256"}` | 设备二次核对（防 UDP 丢包拿到错信息） |
| `GET /firmware.bin` | 200 `Content-Length: N`，body=bin | M1 不支持 Range；M2 支持 `Range: bytes=off-` → `206 Partial Content` |

---

## 3. 设备端改动

### 3.1 新增 `main/middleware/network/ota_service.{c,h}`（核心，~350 行）

```c
/* ota_service.h 公开 API（风格对齐 record_service） */
typedef enum { OTA_EVT_OFFERED, OTA_EVT_DOWNLOADING, OTA_EVT_VERIFYING,
               OTA_EVT_DONE, OTA_EVT_FAILED, OTA_EVT_ROLLBACK_ARMED } ota_evt_t;
typedef struct { ota_evt_t event; uint8_t percent; char detail[64]; } ota_event_t;
typedef void (*ota_evt_cb_t)(const ota_event_t *ev);

void ota_service_init(void);                 /* main.c 在 pcnow_service_init 后调用 */
bool ota_service_is_active(void);            /* 互斥判断 */
void ota_service_set_evt_cb(ota_evt_cb_t cb);/* UI/自证订阅（置标志，不碰 LVGL） */
void ota_service_on_json(const char *json);  /* link_manager 分发 OTA_* 消息入口 */
```

**状态机**：`IDLE → OFFERED → DOWNLOADING → VERIFY → REBOOT_PENDING → (重启) → PENDING_VERIFY → VALID / ROLLBACK`

**实现要点**（逐条都是本项目已踩过的坑）：

1. **任务骨架照抄 record_service**：`xTaskCreatePinnedToCoreWithCaps(..., 8192, ..., MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)`（F35 PSRAM 外置栈）+ 命令队列；下载在任务内同步做，4KB 静态缓冲放 .bss（不放栈，栈在 PSRAM 且 DMA 无关）。
2. **拉取照抄 pcnow_service 的 `http_get()` 骨架**，但**不能整包缓冲**（bin 4.9MB > PSRAM 余量）：`esp_http_client_open()` 后循环 `esp_http_client_read()` 4KB → `esp_ota_write()`，边读边喂 mbedtls SHA256。
3. **写槽流程**（app_update 组件）：
   ```c
   const esp_partition_t *upd = esp_ota_get_next_update_partition(NULL); /* 自动=ota_1 */
   esp_ota_handle_t h;
   esp_ota_begin(upd, offer_size, &h);        /* 传入精确 size，只擦需要的簇 */
   ... 循环 esp_ota_write(h, buf, n) ...
   esp_ota_end(h);                            /* 内部做镜像头/分段校验 */
   if (sha256 匹配) esp_ota_set_boot_partition(upd);
   ```
4. **`esp_ota_write` 会短暂关 cache**：下载期间互斥表（§3.3）必须先过；下载循环里**禁止**再触发其它 Flash 写（录音、壁纸写盘）。
5. **进度回传**：`link_manager_send_json()` 每 2% 发 `OTA_PROGRESS`；同时置 `ota_event_t` 给 UI（M3 接浮层）。
6. **自证（防砖核心，放 main.c）**：
   ```c
   /* 30s 一次性 esp_timer：WiFi 已连 + UI 正常跑 → 自证 */
   const esp_partition_t *run = esp_ota_get_running_partition();
   if (esp_ota_get_state_partition(run, &st) == ESP_OK &&
       st == ESP_OTA_IMG_PENDING_VERIFY) {
       esp_ota_mark_app_valid_cancel_rollback();   /* 不过这一关，下次重启自动回滚 */
   }
   ```
   M1 自证条件 = 30s 内无崩溃 + `wifi_manager_is_connected()`；M3 增强 = 再加 link_manager 在线 + 音频任务水位正常。**注意**：处于 PENDING_VERIFY 时设备必须拒绝新的 OTA_OFFER（IDF 规定 pending 槽未验证前不能写另一槽）。
7. **OFFER 校验顺序**（任一不过回 `OTA_REJECT`）：`link_manager_is_online()` → `!record_service_is_active()` → 无通话会话（`session_manager` 状态）→ `!ota_service_is_active()` → 运行槽非 PENDING_VERIFY → `size <= 备用槽剩余`。

### 3.2 `link_manager.c` 两处小改（~15 行）

1. **fw_ver 真实化**（版本上报是 PC 版本比对的基础）：
   ```c
   #include "esp_app_desc.h"   /* 组件 esp_app_format，IDF 自带 */
   ... snprintf(ack, sizeof(ack),
        "{\"type\":\"HEARTBEAT_ACK\",\"ver\":1,\"proto_ver\":1,"
        "\"sample_rate\":16000,\"fw_ver\":\"%s\"}",
        esp_app_get_description()->version);   /* 替换硬编码 "phase2" */
   ```
2. **OTA 消息分发**：在现有 `strstr(packet, ...)` 分发链后加一路——**不做 bluetooth→network 反向 include**，改为在 link_manager.h 加一个通用 JSON 回调槽：
   ```c
   typedef void (*link_json_cb_t)(const char *json);   /* 收到含 "type" 的包时调用 */
   void link_manager_set_json_cb(link_json_cb_t cb);   /* 单槽；ota_service_init 注册 */
   ```
   `ota_service_on_json()` 里自己 `strstr("OTA_OFFER"/"OTA_ABORT")` + cJSON 解析。这与 pcnow 用 observer 槽的风格一致，且不动 UDP 收包主循环结构。

### 3.3 互斥表（升级期间谁必须停）

| 业务 | OTA 期间 | 机制 |
|---|---|---|
| 录音（record_service，写 littlefs） | **拒绝启动 OTA**（发起时检查） | `record_service_is_active()` |
| 通话（udp_voice） | **拒绝启动 OTA** | session_manager 无活动会话 |
| 本地/蓝牙放歌 | 允许（SD/蓝牙流不写内部 Flash；背景音可能卡顿属预期） | 无 |
| BT 连接 | 保持 | 不动 |
| 副屏推送（pcnow） | 保持 | 不冲突 |
| 新的 OTA_OFFER | 拒绝 | `ota_service_is_active()` |

### 3.4 其余接线（各 ~2 行）

- `main.c`：`#include "middleware/network/ota_service.h"` → `pcnow_service_init();` 之后加 `ota_service_init();`；`app_main` 尾部加自证 30s 定时器（§3.1-6）。
- `main/CMakeLists.txt`：SRCS 加 `"middleware/network/ota_service.c"`；PRIV_REQUIRES 追加 `app_update esp_partition app_format`。
- `sdkconfig.defaults` 追加（★ 必须进 defaults，只改 sdkconfig 重配置会掉，链接期才炸的老坑）：
  ```
  CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
  ```
  改后需全量重编译（bootloader 配置变化）。

---

## 4. PC 端改动（tools/pc_now_agent，~200 行 C#）

新增 `--ota <bin路径>` 模式（与推流模式互斥执行，心跳发现逻辑复用）：

1. **解析固件元信息**：读 bin 偏移 `0x20`（esp_image_header 24B + 段头 8B 之后）256B 的 `esp_app_desc_t`：magic `0xABCD5432` 校验 → `version`（+16，char[32]）/ `project_name`(+48) / `time`/`date`；现算 SHA256 与文件大小。解析失败（magic 不对）直接报"不是 app bin"退出——挡住误拖 partition bin / bootloader.bin。
2. **版本比对**：与最近一次 `HEARTBEAT_ACK` 的 `fw_ver` 比（M1 字符串相等比较 + SemVer 数字比较升级到 M4）：目标新→绿色可升；同版→黄色"重复刷"；旧版→红色"降级确认"需再传 `--force`。
3. **HTTP 路由注册**：在既有 `HttpLoopAsync` 的路由分发里加两支：
   - `/firmware.json` → 序列化 `{ver,size,sha256}`；
   - `/firmware.bin` → 200 + `Content-Length` + `FileStream` 分段写出（64KB 缓冲）。
   仅在 `--ota` 会话期间注册，会话结束即摘除，不影响副屏路由。
4. **会话流程**：等心跳确认设备在线 → 发 `OTA_OFFER` → 等 `OTA_ACCEPT/REJECT`（10s 超时重发 3 次）→ 收 `OTA_PROGRESS` 打印 `[####----] 42%  312KB/s ETA 8s` → `OTA_RESULT`：ok=1 则等设备重启 + 心跳恢复，读新 `fw_ver` 判定 **升级成功**（版本一致）或 **已回滚**（版本没变 + 设备日志有 rollback）。
5. **保真 STM32 调试利器**（迁移自 IAP 上位机，M2 落地）：`--ota-limit N` 只发前 N%（测断电/回滚）、`--ota-corrupt` 打错一个块（测 SHA256 拦截）。

---

## 5. 实施里程碑

| 阶段 | 内容 | 验收 |
|---|---|---|
| **M1 闭环**（先做） | 设备 ota_service + link_manager 两处小改 + sdkconfig/CMake 接线；PC `--ota` 基础会话（无续传/无 UI/无纠错注入） | PC 一条命令完成"OFFER→拉取→校验→重启→心跳报新版本"；手动断电重试不变砖（重下整包） |
| **M2 健壮性** | nvs 断点 offset + PC `Range` 206 + `OTA_ABORT` + `--ota-limit/--ota-corrupt` | 传输中拔电→重发→从断点续传成功；坏块固件被 SHA256 拦截不切槽 |
| **M3 体验** | 设备 LVGL 升级进度浮层（居中卡片：版本 from→to + 进度条 + "请勿断电"）；自证条件增强（link 在线 + 音频水位） | 升级全程屏上可见；拔掉 PC 后设备 10s 内 ABORT 回 IDLE |
| **M4 工程化** | `firmware/` 版本归档 + manifest 自动生成（build 后脚本）；升级历史 jsonl；SemVer 比对；（可选）PySide6 OTA Tab | 任意旧版本可重推回退 |

---

## 6. 测试与验收清单（对应 §11.7）

1. **正常升级**：`--ota xxx.bin` → 新版本上线、`esp_ota_get_running_partition()` 变为 ota_1、旧槽内容保留；
2. **自证**：新固件跑满 30s 后 `PENDING_VERIFY → VALID`（日志确认），再重启不回滚；
3. **坏固件回滚**：推一个能过镜像校验但启动即崩/不调 mark_valid 的固件（用 `--ota-limit 100` + 临时改版本号做）→ 第二次重启自动回旧槽，`OTA_RESULT`/心跳显示旧版本；
4. **断电**：下载中断电（或 `--ota-limit`）→ 重新 `--ota` → M1 重下成功 / M2 续传成功；
5. **互斥**：录音中发 OFFER → `OTA_REJECT busy`；通话中同；
6. **版本链路**：`HEARTBEAT_ACK.fw_ver` == `esp_app_desc_t.version` == PC 解析值三方一致；
7. **素材不擦**：OTA 后壁纸/字库/录音均在（只写 app 槽，storage 分区不触碰）。

---

## 7. 风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| app bin ≈4.88MB，5MB 槽仅剩 ~130KB | 稍加功能就写不进槽 | 构建脚本加"bin > 0x4F000 即报错"检查；长期看砍媒体组件或减 1MB（见小智方案分区论证） |
| `esp_ota_write` 关 cache 与音频/LVGL 抢 Flash | 偶发卡顿甚至 cache error | 互斥表（§3.3）+ 下载块间 `vTaskDelay(1)` 让出 CPU；不复现 STM32 时代的"长关中断" |
| PENDING_VERIFY 期间误发 OFFER | esp_ota_begin 报 ESP_ERR_OTA_VALIDATE_FAILED | OFFER 校验链第 4 条直接 REJECT(pending) |
| UDP 50003 丢 OFFER/PROGRESS | PC 卡在等 ACCEPT | OFFER 10s×3 重发；PROGRESS 丢失只影响显示（HTTP 拉取本身可靠） |
| Windows 防火墙拦 50005（新场景大量下行） | 设备拉不到 | README 已写防火墙放行；OTA 模式启动时自检端口可绑定 |
| 回滚后 OTADATA 混乱（反复升级失败） | 起不来 | 兜底永远可用 USB `idf.py flash` 重刷 otadata+ota_0（不设 factory 的既定策略） |

---

## 8. 涉及文件清单（实施时逐一勾选）

**设备端**：
- 新增 `main/middleware/network/ota_service.c` / `ota_service.h`
- 改 `main/middleware/bluetooth/link_manager.{c,h}`（fw_ver 真实化 + json 回调槽）
- 改 `main/app/main.c`（init 接线 + 自证定时器）
- 改 `main/CMakeLists.txt`（SRCS + PRIV_REQUIRES: app_update/esp_partition/app_format）
- 改 `sdkconfig.defaults`（BOOTLOADER_APP_ROLLBACK_ENABLE）

**PC 端**：
- 改 `tools/pc_now_agent/Program.cs`（--ota 会话：bin 解析/SHA256/路由/进度/结果判定）
- 改 `tools/pc_now_agent/README.md`（用法章节）

**文档**：
- 本方案落地后：ICD 追加 V1.4（§2 的消息与路由）；改动记录追加 §28.3x
