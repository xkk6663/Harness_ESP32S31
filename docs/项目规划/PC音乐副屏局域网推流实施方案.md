# PC 音乐副屏局域网推流实施方案

> 状态：**✅ 已完成并实机验收通过（2026-09-24）** —— 决策落定：歌词 **v1.1**（全量下发设备自算）、旋转 **方案 B**（LVGL 软件旋转）。首轮代码落盘后经 §28.37~§28.56 逐轮排错，整条链路 **2026-09-24 实机验收通过**（F44~F47，ICD V1.3 §7）。
>
> **PC 端实现更新（2026-09-23 晚）**：除 §5 的 C# 控制台（`tools/pc_now_agent`）外，
> 副屏能力已移植进统一上位机 **`pc_host` v2**（PySide6，音乐副屏页，见 `pc_host/README.md`）。
> 两套协议等价（ICD V1.3），**二选一运行**（50004/50005 冲突）。以下 §5 保留为 C# 版设计记录。
> 版本：**V2（2026-09-23 复用传声机链路修订）** —— 相比 V1 的最大变化：砍掉自建发现/重连/TCP 通道，全部挂到现有 link_manager 体系上
> 日期：2026-09-23
> 关联文档：`通信接口规范_ICD.md`（本次已追加 **V1.3** 增量，见其 §6）、`小智AI对话接入实施方案.md`（互不影响）
> 复用基础：`link_manager`（心跳/对端学习/JSON 信令）、`udp_voice`（帧模式先例）、`overlay_wallpaper.c`（JPEG 解码）、`tab_btsink.c`（歌词窗 UI）、`ui_binfont`

---

## 0. 结论摘要

- **纯显示功能**：设备不发声、不解码音频，只收 JSON/JPEG 刷 UI。不动分区表、不动音频链路。
- **V2 关键修订——复用传声机（低延迟传声）项目的整套链路层**：
  - 对端发现/在线状态 → **`link_manager`（UDP :50003）现成**：PC 每 200ms 发 HEARTBEAT，设备 `recvfrom` 自动学习 PC IP，5s 超时判离线，事件 observer 上抛。V1 方案里自建的"UDP 广播发现 + TCP 重连状态机"整块删除；
  - 设备→PC 上行 → **`link_manager_send_json()` 现成**（未来的"设备遥控 PC 播放"零新增通道代码）；
  - 新增数据面只用一个 UDP 端口：**50004（PC → 设备，NOW_* JSON 推送）**，由新服务 `pcnow_service` 独占（不侵入 link_manager 的信令分发）；
  - 封面（几十 KB 的 JPEG）走 **HTTP 拉取**：PC 用 `HttpListener` 起一个只读小服务，设备 `esp_http_client`（已在 `PRIV_REQUIRES`）按封面哈希变更时 GET 一次——大块数据不塞 UDP，也不必维护长连接。
- 设备端新增仅 2 个文件（`pcnow_service` + `tab_pcnow`），**估计比 V1 少 300+ 行**连接管理代码；PC 端 TCP server 换成心跳线程 + HttpListener，更简单。
- **封面旋转的关键事实**：ESP32-S31 的 PPA 硬件旋转**只支持 0°/90°/180°/270°**，连续平滑旋转走软件（方案 B）或预渲染帧序列（方案 C），推荐 B 起步、C 兜底。
- 歌词字体复用 littlefs 的 `lv_font_cjk_28.bin`（`ui_binfont`）。

---

## 1. 需求与范围

| 项 | 内容 |
|---|---|
| PC 端（开发者写） | 抓 Windows Media Session（SMTC）：播放器名/曲名/歌手/总时长/播放进度/专辑封面；本地或在线匹配 LRC；按本方案协议向设备推送 |
| 设备端 | 连同一 WiFi（STA 角色，复用 wifi_manager）；复用 link_manager 在线状态；收 NOW_* 推送 + 按需拉封面；新增 tab 实时显示封面（圆形遮罩 + 旋转）、歌名、歌手、进度、歌词 |
| 范围外 | 设备端出声（不接管音频）；设备端控制 PC 播放（通道已预留，见 §8） |

---

## 2. 可复用接口盘点（本次修订的核心）

| 现有资产 | 位置 | 能力 | 副屏怎么用 |
|---|---|---|---|
| **link_manager** | `main/middleware/bluetooth/link_manager.{c,h}` | UDP :50003 心跳；`recvfrom` 自动学习 PC IP；5s 超时判 OFFLINE；observer 事件（`LINK_EVT_ONLINE/OFFLINE` + peer_ip，4 槽位）；`link_manager_send_json()` 上行 | **直接复用**：tab 门禁卡显示 online/offline；封面/歌词拉取用 `link_manager_get_peer()` 拿 PC IP；上行命令零新增 |
| **ICD 端口规划与信令惯例** | `docs/项目规划/通信接口规范_ICD.md` V1.2 | 50000/50001 音频、50002 TCP 数据（长度前缀分帧）、50003 信令（JSON 单行 `{"type":...}`）；HEARTBEAT 200ms / 判离线 5s | 新增消息**按 ICD 惯例追加 V1.3**：NOW_META / NOW_PROG / NOW_LRC（50004）、HTTP 封面服务（50005） |
| **udp_voice** | `main/middleware/audio/udp_voice.c` | 12B 帧头（magic/version/type/seq/len）、select 超时收包、32 槽重排序、命令队列+事件回调骨架 | **编码模式参照**：pcnow_service 的收包任务/命令队列/事件上抛照这个骨架写 |
| **PSRAM 栈任务模式** | `udp_voice.c:70`、`link_manager.c:41` | `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)`（F35 教训落地） | pcnow_service 收包任务 8KB 栈 PSRAM 外置，照抄 |
| **record_service STREAM + 50002 分帧** | `record_service.{c,h}`、ICD §5 | 设备→PC TCP 长度前缀分帧上传 | 大块传输的备用先例（本期封面选 HTTP 拉取更省事；若未来要推大文件可回此路） |
| **session_manager** | `middleware/audio/session_manager.{c,h}` | 音频会话仲裁（SESSION_UDP_CALL 等） | **本期不用**（副屏无声）；将来"设备当 PC 音乐的音箱"时复用 |
| **JPEG 解码** | `overlay_wallpaper.c:102-139` | esp_new_jpeg → RGB888 → 字节交换 → lv_canvas | 封面解码整段照抄（RGB 序注释写清两端约定，防 §28.30 红蓝颠倒） |
| **cjson / esp_http_client / lwip** | `main/CMakeLists.txt:86-87` 已在 PRIV_REQUIRES | JSON 解析、HTTP 客户端、socket | **零新增依赖** |
| **tab_btsink 歌词三行窗** | `tab_btsink.c:65-117` | 裁剪窗+平移列+单 lv_anim 透明度插值，行高 `lv_font_get_line_height()` 现算 | tab_pcnow 歌词窗整段照搬 |
| **ui_binfont** | `ui/common/ui_binfont.{c,h}` | 全 CJK bin 字体（动态文本专用，禁套图标/写死英文） | 歌名/歌手/歌词；NULL 回退 `lv_font_cn_30` |

> 结论：**发现、在线状态、上行通道、端口规范、编码骨架全部现成**。设备端真正的新代码 = 1 个收包/解析任务 + 快照结构 + 1 个 UI 页。

---

## 3. 总体架构（V2）

```
┌─ PC (Windows, C# .NET 8 控制台) ──────────────────────────┐
│  SMTC 采集器 ─┬→ NOW_META   (曲名/歌手/专辑/时长/封面hash) │
│  (网易云/QQ/   ├→ NOW_PROG   (pos_ms, playing)     1Hz     │
│   foobar/浏览器)├→ NOW_LRC    (当前歌词行)                  │
│                └→ 封面(网易云API→SMTC Thumbnail 兜底)       │
│  心跳线程: HEARTBEAT → 设备:50003，每 200ms（ICD V1.2 不变）│
│  推送线程: NOW_* JSON → 设备:50004（新端口，UDP）           │
│  HttpListener :50005 /cover/<hash>.jpg /lyrics.json        │
│  上行接收: 50003 收 NOW_CMD（设备遥控播放，预留）           │
└──────────────────────────┬────────────────────────────────┘
                           │ WiFi 局域网（同一网段）
┌─ 设备 (ESP32-S31) ───────┴────────────────────────────────┐
│ link_manager(现有): 50003 心跳→在线状态/PC IP（不动一行）  │
│ pcnow_service(新): 绑定 :50004 收 NOW_* JSON               │
│   → cJSON 解析 → 快照 pc_now_t（临界区保护）               │
│   → 封面 hash 变化时 esp_http_client GET :50005            │
│   → JPEG 解码(esp_new_jpeg) → RGB888 交换 → 就绪标志       │
│ tab_pcnow(新, LVGL 线程 200/500ms lv_timer 轮询):          │
│   左: 圆形封面 176×176 (遮罩+旋转 §6.3)                     │
│   右: 歌名/歌手(bin字体) + 进度条 + 三行歌词窗              │
└───────────────────────────────────────────────────────────┘
```

---

## 4. 通信协议（ICD V1.3 增量）

### 4.1 不变的部分

- 心跳/在线状态机/能力协商：**ICD V1.2 §4 原样沿用**（PC 每 200ms `{"type":"HEARTBEAT",...}` → :50003；设备回 HEARTBEAT_ACK；5s 判离线）；
- 命名惯例：JSON 单行、`"type"` 字段路由。

### 4.2 新增端口

| 端口 | 协议 | 方向 | 用途 |
|---|---|---|---|
| **50004** | UDP | **PC → 设备** | NOW_* 元数据/进度/歌词推送（JSON 单行，≤1 datagram 一条） |
| **50005** | HTTP | 设备 → PC | 封面 JPEG / 全量歌词拉取（只读小服务） |

50000-50003 全部不动。

### 4.3 新增消息（UDP :50004）

| type | payload | 推送时机 |
|---|---|---|
| `NOW_META` | `{"type":"NOW_META","title":"..","artist":"..","album":"..","duration_ms":215000,"player":"Netease","cover":"a1b2c3d4"}` | 换曲时；**每 3s 周期重发**（UDP 容丢） |
| `NOW_PROG` | `{"type":"NOW_PROG","pos_ms":63200,"playing":true}` | 1Hz |
| `NOW_LRC` | `{"type":"NOW_LRC","text":"因为我不知道","index":12}` | 行变化时 + 每 3s 重发当前行 |

**UDP 可靠性论证**：局域网实测丢包≈0；即使丢一条，META/PROG/LRC 都有周期重发兜底（最多延迟 3s），且 progress 有设备端本地内插——**不需要 TCP 的可靠性开销**。这与 ICD 既有哲学一致（音频帧也走 UDP + 应用层补偿）。

### 4.4 HTTP 拉取（:50005）

- 封面：`GET /cover/<hash>.jpg` → 240×240 JPEG（PC 端缩好，≤64KB）。**只在 `cover` hash 变化时拉一次**；PC 本地按 hash 缓存；
- 全量歌词（**v1.1，已采纳**）：`GET /lyrics.json?k=<hash>` → `{"lines":[{"t":ms,"text":".."},...]}`（可 >MTU，HTTP 天然可靠）；**PC 下发前已归一化**（完成 LRC 解析/多时间标签展开/排序），设备只做二分查行——解析逻辑不过界，设备端实现收敛为 ~30 行查找；
- 设备端歌词时钟：NOW_PROG 打点 + esp_timer 外推（暂停冻结、越曲长封顶），进度条与歌词**同源**；
- hash = SHA1(title|artist) 前 8 hex（双端一致，ICD V1.3 §6.3）；
- 设备端拉取时机：META 里的 hash 与本地已持有不同才拉。

### 4.5 设备→PC 上行（预留，零新增通道）

`link_manager_send_json()` 直接发：`{"type":"NOW_CMD","cmd":"play_pause"}` / `next` / `prev` / `volume_up|down`。PC 端在 50003 收包分支加 NOW_CMD → SMTC `TryChangePlaybackStatusAsync` 等。

### 4.6 歌词推进 v1 vs v1.1 详析（决策依据，2026-09-23 讨论）

本质区别：**歌词时间轴放在谁手里**。v1 在 PC（设备是哑显示），v1.1 在设备（PC 只是文件源）。

| 维度 | v1（PC 推当前行） | v1.1（全量下发设备自算） |
|---|---|---|
| 设备端新增 | ≈0（seq 驱动三行窗照搬 tab_btsink） | LRC 解析 + 时间轴推进 150-250 行 + 容错用例 |
| PC 端 | 实时算当前行（反正在做 LRC 匹配） | 只管下载/下发，不再实时算行 |
| 拖进度条 | 等 PC 推新行（通常 <0.2s） | 立即正确（与进度条同源时钟） |
| 丢一条 UDP | 旧句滞留至 3s 重发 | 无感（不走网络） |
| 断线重连 | 等重发才恢复 | 本机就有，时钟恢复即恢复 |
| 进阶 UI 空间 | 只能单行 | 多行滚动 / 点行回跳（NOW_CMD seek）/ 逐字卡拉OK |
| 一致性 | 歌词行与进度条是两个真相来源 | 单一真相来源 = 设备内插时钟 |

**共同点（重要）**：三行动画层两种方案**完全复用**——把"当前行"抽象成一个
`{text, index}` 来源接口即可：v1 的来源是网络推送，v1.1 的来源是本地计算。
切歌瞬间的首句显示延迟两者相同（都要等 PC 检测换曲→拿到 LRC）。

**建议**：v1.1 值得多花约 1 天——副屏的核心体验就是"歌词和进度永远一致"，
且换曲时一次拉全量反而比逐行推送更干净（封面本来就要走 HTTP，lyrics.json
只是多一个路由）。v1 作为回退态保留：设备端"收到 NOW_LRC 就显示"的逻辑
可以一直留着，全量解析失败（畸形 LRC）时自动退化为 v1 行为，**灰度共存、
互为兜底**。最终定夺待用户拍板。

---

## 5. PC 端程序设计（C# .NET 8 控制台，单项目）

### 5.1 SMTC 采集（Windows 自带 API，无驱动依赖）

- `Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager`（WinRT，`Microsoft.Windows.SDK.NET.Ref` 或 CsWinRT）：
  - `SessionsChanged` + `GetCurrentSession()`：当前活跃播放器会话；
  - `MediaProperties`：Title / Artist / AlbumTitle / Thumbnail（任意播放器通用——网易云 UWP/Win32、QQ音乐、foobar（组件）、浏览器均注册 SMTC）；
  - `TimelineProperties`：Position；`PlaybackInfo.PlaybackStatus`：Playing/Paused。

### 5.2 LRC 匹配（按优先级）

1. 本地目录：`<音乐目录>/<歌手> - <曲名>.lrc`（可配多目录）；
2. 网易云 API：`api/search/get` 拿 id → `api/lyric?id=` 拿 LRC；QQ 音乐类推；
3. 失败 → 无歌词态（UI 占位），不阻塞其它字段。

### 5.3 封面

1. 网易云 `api/song/detail` 高清图 → 缩放到 240×240 JPEG（ImageSharp，质量 80，~15-30KB）；
2. 兜底：SMTC `Thumbnail`（RandomAccessStreamReference，通用）；
3. 曲名+歌手哈希缓存（`%TEMP%\pcnow_cover\<hash>.jpg`）。

### 5.4 线程与推送（V2 简化）

- **心跳线程**：200ms 一发 HEARTBEAT 到设备 :50003（从 HEARTBEAT_ACK 的 recvfrom 学设备 IP；5s 无 ACK 显示设备离线）——**与传声机 PC 端完全同一套代码**；
- **推送线程**：UDP :50004 发 NOW_*（META/PROG/LRC 按上表节奏）；
- **HttpListener :50005**：cover/<hash>.jpg + lyrics.json 两个只读路由；
- **50003 收包**：NOW_CMD 分支（预留）。

---

## 6. 设备端设计

### 6.1 `middleware/network/pcnow_service.{c,h}`（新）

```
生命周期: wifi_manager sys_cb 的 GOT_IP → 注册 link_manager observer（已有 4 槽位机制）
          LINK_EVT_ONLINE  → 启动收包任务（bind :50004）
          LINK_EVT_OFFLINE → 停任务、清快照
          WIFI_EVT_DISCONNECTED → 同上
```

- 收包任务（骨架照 `udp_voice.c`：命令队列 + `select` 超时收包 + PSRAM 外置栈）：
  - recvfrom :50004 → cJSON 解析 → 写快照（临界区）；
  - `cover` hash 变化 → `esp_http_client` GET `http://<peer_ip>:50005/cover/<hash>.jpg`（**同步拉取放在收包任务里**，UI 不等）→ JPEG 解码（照 `overlay_wallpaper.c:102-139`：esp_new_jpeg → RGB888 → **p[0]↔p[2] 交换**，注释写明"解码器 R,G,B 内存序 → LVGL RGB888 为 B,G,R"两端约定）→ 置 `cover_ready`；
- 快照接口（LVGL 线程调用，非阻塞，仿 `bt_sink_service_get_meta`）：

```c
typedef struct {
    char     title[128], artist[128], album[128], player[32];
    uint32_t duration_ms, pos_ms;        /* pos_ms 由本地 1Hz 内插 */
    bool     playing;
    char     lrc_text[128];
    uint32_t lrc_seq;                    /* 递增；UI 只在变化时播动画 */
    uint32_t cover_id;                   /* 递增；UI 只在变化时换图 */
    bool     cover_ready;
} pc_now_t;
bool pcnow_service_get_snapshot(pc_now_t *out);
pcnow_state_t pcnow_service_get_state(void);   /* OFFLINE/WAITING/LIVE */
```

- **线程纪律（铁律，同 bt_sink_service）**：服务线程绝不碰 LVGL，只更新快照/标志；UI 用 lv_timer 轮询。

### 6.2 `ui/tabs/tab_pcnow.c`（模仿 SINK 页）

```
┌──────────────────────────────────────────────┐
│ ● PC NOW   [EQ动画]              (顶栏 44px) │
│ ┌──────────┐  ♪ 歌名 (bin 28px)              │
│ │  圆形封面  │  ─ 歌手 (bin 28px)             │
│ │ 176×176   │  ── 进度条 + 03:12/04:14       │
│ │  (旋转)   │  上一句(暗)                     │
│ └──────────┘  当前行(亮)  ← 三行歌词窗        │
│               待入行                          │
└──────────────────────────────────────────────┘
```

- 门禁卡：`pcnow_service_get_state()` 非 LIVE → "等待 PC 推流…（检查 PC 端程序/防火墙）"，online 状态点颜色直接接 link_manager 事件；
- 封面：`lv_canvas`（RGB888 240×240 源）+ 外层容器 `radius = LV_RADIUS_CIRCLE` + `lv_obj_set_style_clip_corner(true)` 圆形遮罩 + 主题色描边环；
- 歌词窗：照搬 `tab_btsink.c` 三行窗（`lrc_seq` 驱动，与 bt_sink 的 `ly.seq` 同构）；
- 进度条：lv_bar，200ms lv_timer 本地内插刷新；暂停冻结；
- EQ/呼吸动画**按需起停**（LIVE 才跑）；
- 字体纪律：动态文本（歌名/歌手/歌词）用 `ui_binfont_get()`（NULL 回退 `lv_font_cn_30`）；写死英文/图标保持 UI_FONT + LV_SYMBOL。

### 6.3 封面旋转（本期技术难点）

**前提事实**：PPA SRM 硬件旋转只支持 90° 整数倍（esp_driver_ppa）；`CONFIG_LV_USE_PPA` 当前未开（`sdkconfig:5561`）。

| 方案 | 实现 | CPU | 内存 | 观感 | 结论 |
|---|---|---|---|---|---|
| A. PPA 90° 步进 | `ppa_do_rotate` 每 N 秒转 90° | ~0 | 0 | 生硬 | **不做** |
| **B. LVGL 软件旋转** | `lv_image_set_rotation(angle)`，timer 每帧 +2°；可选开 `CONFIG_LV_USE_PPA` 加速 blit/fill | 中（240×240 变换/帧，M2 实测） | 0 | 连续平滑 | **推荐起步**；仅本 tab 活跃时跑（切走即停，"动画按需起停"约定） |
| **C. 预渲染帧序列** | 收包任务离线预旋转 60 帧 RGB565（60×240×240×2 ≈ **6.6MB PSRAM**），UI 逐帧 blit | 极低 | 6.6MB PSRAM | 流畅 | B 不达标时兜底 |
| D. PPA 任意角 | — | — | — | — | **不存在**（硬件限制），写明防踩坑 |

---

## 7. 内存账

| 项 | 大小 | 说明 |
|---|---|---|
| pcnow 收包任务栈 | 8KB | **PSRAM 外置**（P2，照 udp_voice 模式） |
| JPEG 原始缓冲 | ≤64KB | 服务线程临时，解码完即 free |
| RGB888 解码输出 | 240×240×3 = 172.5KB | PSRAM 常驻（双缓冲 +172KB，M2 再定） |
| 方案 C 预渲染帧（可选） | ~6.6MB | 仅启用 C 时申请 |

内部 RAM 新增 ≈ 0（socket 缓冲 + cJSON 临时块）。link_manager/心跳是存量成本，不新增。

---

## 8. 后续扩展（本期不做，通道已预留）

- 设备→PC 遥控（暂停/上一首/下一首/音量）：`link_manager_send_json` NOW_CMD，设备侧零新增；
- v1.1 全量歌词（HTTP /lyrics.json + 设备自算高亮行）；
- 多设备：PC 端心跳/推送天然支持多订阅者。

---

## 9. 里程碑与验收

| 里程碑 | 内容 | 验收标准 |
|---|---|---|
| **M0 PC 端 demo** | C#：SMTC 采集 + 200ms HEARTBEAT（复用传声机 PC 端代码）+ :50004 推送 + :50005 HttpListener；先用 python 脚本当"假音箱"验证协议 | 假音箱收到正确 NOW_*；换歌/暂停/拖进度节奏正确 |
| **M1 设备收流+基础 UI** | pcnow_service（observer 挂接 + 50004 收包 + 解析 + 快照）+ tab_pcnow（无封面：歌名/歌手/进度/在线状态点） | 设备在线点变绿；换歌 <1s 上屏；PC 关程序 → 5s 内转 OFFLINE（link_manager 现成判离线）；重开自动恢复 |
| **M2 封面+歌词** | hash 变化触发 HTTP 拉取 + JPEG 解码 + 圆形遮罩；NOW_LRC 三行窗动画 | 封面正确显示（**红蓝序自查**）、圆形裁剪正确；歌词逐句动画；断线重连后 3s 内恢复显示；内存账入册 |
| **M3 旋转+打磨** | 方案 B 旋转 + CPU/帧率实测；不达标切方案 C | 封面持续旋转不掉帧；tab 切走动画停止；EQ 按需起停 |

> 对比 V1：M1 里"发现+TCP 重连状态机"（约 300 行 + 2~3 天调试）被 link_manager 复用替代，M1 预计减半。

> ## ✅ 状态（2026-09-24）：M0~M3 全部完成并**实机验收通过**
>
> | 里程碑 | 状态 | 关键落地 |
> |---|---|---|
> | **M0 PC 端 demo** | ✅ 完成，且**远超规划** | 不走 C# 路线：C# agent（`tools/pc_now_agent`）已融合进 `pc_host/pcnow/`（Python）；链路走 UDP 50003/50004 + HTTP 50005 |
> | **M1 设备收流 + 基础 UI** | ✅ 完成 + 实机验收 | `pcnow_service` + `tab_pcnow`（`link_manager` observer 判在线） |
> | **M2 封面 + 歌词** | ✅ 完成 + 实机验收 | 歌词最终采用 **v1.1 全量下发**（非 v1 单行推送）；封面 JPEG + 圆形遮罩；拉取拆到独立 `fetch_task`（F44） |
> | **M3 旋转 + 打磨** | ✅ 完成 + 实机验收 | 采用**方案 B**（`lv_image_set_rotation` 软件旋转，~30fps，仅本 tab 活跃时跑）；未触发切方案 C |
>
> **超出原规划的落地**：位置真值走 **CEF CDP 读进度 DOM**（`PC副屏歌词同步-位置真值方案…md` R3，
> 原为"待验证"的备选）；歌词对齐追加设备端 **1250ms 提前量**（F47）。
> 踩坑与改动流水详见 `11_遇到的问题.md` **F41~F47** 与 `改动记录.md` **§28.44~§28.56**。

---

## 10. 风险与待确认清单

| # | 风险/待确认 | 对策 |
|---|---|---|
| 1 | UDP NOW_* 丢包 | META/PROG/LRC 周期重发兜底（3s）；进度本地内插；LAN 实测丢包≈0 |
| 2 | 设备 :50004 与未来新服务的端口冲突 | ICD V1.3 统一登记，占 50004/50005，50006+ 留白 |
| 3 | SMTC 对个别播放器元数据不全 | Thumbnail 兜底；M0 产出已验证播放器清单 |
| 4 | 网易云 API 无官方保证 | 本地 .lrc 优先；失败静默降级无歌词态 |
| 5 | ~~LRC 时间轴偏差（网易云常见 +0.5~1s）~~ | ✅ **已解决**（F47/§28.56）：实测设备端比 PC 端**晚 1.25s**，落成设备端常量 `PCNOW_LYR_LEAD_MS`。⚠ 与本表原写的"PC 端全局偏移配置项"不同 —— 落在设备端是因为 `pusher._interpolated_pos()` 被 PC 自己的副屏预览与推送**共用**，在 PC 侧加提前量会把 PC 自己的显示也一起推早。换播放器后如需分播放器配置，再上移到 PC 侧按 player 下发 |
| 6 | AP 隔离/防火墙 | 传声机已有排查经验（ICD 附带）；设备侧保留手动填 PC IP 的 NVS 直连后门（v1.1） |
| 7 | 方案 B 旋转 CPU 超标 | 未发生（实机验收通过，未触发切方案 C） |
| 8 | ~~**歌词推进 v1（PC 推行 NOW_LRC）vs v1.1（全量自算）**~~ | ✅ **已采纳 v1.1**（2026-09-24）：全量 `lyrics.json` + 设备端二分查行。`NOW_LRC` 单行推送**保留为回退通道**（全量歌词缺失/解析失败时仍可显示），且 PC 在 `/lyrics.json` 200 命中后**自动停发**（F44） |
| 9 | ~~封面尺寸/旋转帧率期望（默认 240×240、~30fps）~~ | ✅ **已定并验收**：封面 240×240（PC 侧 `_shrink_cover` 归一化 ≤240px 且 ≤64KB）；旋转 ~30fps（每帧 +2°） |

---

## 11. 与小智方案的关系

- 无冲突：不动分区表、不动音频、不占内部 RAM 大头；
- 建议顺序：先做本方案（M0-M2 两天量级），小智方案 M0 侦察可并行。
