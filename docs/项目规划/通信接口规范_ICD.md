# 设备 ↔ PC 通信接口控制文档（ICD）V1.5

> 本文件是设备端（ESP32-S31，C）与 PC 端（Python）必须共同遵守的**唯一通信契约**。
> 双端常量、字节序、帧格式、状态语义必须严格一致；任何改动先改本文件再改代码。
> 适用模式：模式一 UDP 实时通话（当前）；模式二 TCP 录音复用同一帧头与信令（预留）。
>
> ⚠ **编制原则（V1.2 起）**：本文件描述**已实现的现状**，不是未落地的设计意图。
> 凡本文与代码不一致时，**以代码为准**并回填本文（代码是唯一事实来源）。
> 参数的实际值见 §1 与 §3.2、§4.3；三端（C / Python / 本文）漂移的历史见
> `docs/工程规范/11_遇到的问题.md` F26。
>
> 最后更新：2026-09-24（V1.3 副屏推流实机验收后回填）

---

## 1. 全局常量（双端一致）

| 参数 | 值 | 说明 |
|---|---|---|
| 采样率 SAMPLE_RATE | 16000 Hz | 双端统一，ASR 直接使用 |
| 位宽 BITS | 16 bit | signed PCM，小端 |
| 声道 | 1（mono） | |
| 字节序 | Little-Endian | 所有多字节整数 |
| 帧长 FRAME_MS | 20 ms | |
| 净荷 PCM_BYTES | 640 B | 16000×2×0.02 |
| 音频帧头 | 12 B | 见 §3.1 |
| 单包总长 | 652 B | 远小于 1500 MTU，**不分片** |

设备端以 C 宏定义、PC 端以 Python 常量定义，数值必须逐项相等。

**以下为控制类常量**（不属于音频采样契约，但双端必须一致，见 §3.2 / §4.3）：

| 参数 | 实际值 | 说明 |
|---|---|---|
| 重排窗口 REORDER_SLOTS | **32 帧** | 设备端 `udp_voice.c`；决定可容忍的乱序深度 |
| 出帧节拍 | 20 ms | 与帧长对齐 |
| 缺帧补齐阈值 | **60 ms** | 超过即补静音并推进时间轴 |
| seq 重同步阈值 | 0x4000 | 回退超过此值判对端重启，清槽 |
| PC 心跳周期 | **200 ms** | `net/link.py` |
| 离线判定阈值 | **5 s** | 设备端与 PC 端均为 5s |
| 播放水位上限 | 6 帧（3840B） | `audio_service` 侧，与本层重排解耦 |
| 信令包缓冲 | 384 B | `link_manager.c` `packet[384]`；JSON 信令须小于此值 |

> 上表的实际值由代码回填（V1.2）。修改任一值必须同步
> ① C 代码 ② Python 代码 ③ 本表 三处。

---

## 2. 端口与方向

| 端口 | 协议 | 方向 | 内容 |
|---|---|---|---|
| 50000 | UDP | 设备 → PC | 设备麦克风音频帧 |
| 50001 | UDP | PC → 设备 | PC 麦克风音频帧（PTT 回讲） |
| 50002 | TCP | 设备 → PC（预留） | 整段录音 PCM 流 |
| 50003 | UDP | 双向 | 信令/心跳（JSON 文本） |

设备不预配 PC IP：从 50003 收到的首个合法信令包 `recvfrom` 地址学习 PC IP，
音频上行目标 = 该 IP:50000。

---

## 3. 音频帧格式（50000 / 50001）

### 3.1 帧头布局（12 字节，小端）

| 偏移 | 长度 | 字段 | 类型 | 说明 |
|---|---|---|---|---|
| 0 | 2 | magic | u8[2] | 固定 `0xA5 0x5A`，用于丢包/错包快速丢弃 |
| 2 | 1 | ver | u8 | 帧头版本，当前 =1 |
| 3 | 1 | type | u8 | 0=PCM_S16LE，1=G.722（预留），2=控制内联（预留） |
| 4 | 2 | seq | u16 LE | 帧序号，从 0 递增，uint16 回绕 |
| 6 | 4 | ts_ms | u32 LE | 时间戳，发送方启动毫秒，用于抖动排序/延迟统计 |
| 10 | 2 | len | u16 LE | 其后净荷字节数，PCM 固定 640 |
| 12 | len | payload | bytes | PCM/编码数据 |

### 3.2 接收端处理语义

> ⚠ **双端实现不对称（重要事实）**：设备端是**定长 32 槽环形数组**，
> PC 端是**无界字典 `dict[int, bytes]`**。两者语义等价但资源行为不同，
> 见下方"双端差异"小节。本节先描述**公共语义**，再分别列出各自实现参数。

#### 3.2.1 公共语义（双端一致的部分）

1. **校验**：magic / ver / len 不符 → 丢弃并计 `bad`，不做重排尝试（快速失败）；
2. **seq 回绕**按 uint16 模比较；
3. **对端重启检测**：seq **大幅回退** → 判定新会话，`expected` 重置 + **清空重排缓冲**；
4. **重排**：合法帧按 seq 存入缓冲，不按到达顺序直接播放；
5. **出帧**：按 20ms 节拍推进 `expected`，命中则送播放，未命中则计数（丢帧）；
6. **时间轴单调推进**：同一份 `deadline` 基准按 `+= .02` 累加，
   **不做"等够 N 毫秒"的阻塞等待**（避免抖动累积）。

#### 3.2.2 双端实现参数对照（V1.2 按代码回填）

| 项 | 设备端（C） | PC 端（Python） |
|---|---|---|
| 实现文件 | `middleware/audio/udp_voice.c` | `pc_host/net/voice_udp.py` |
| 重排缓冲 | **定长 32 槽**（`REORDER_SLOTS`，`slots[32]`） | **无界字典**（`slots: dict[int, bytes]`） |
| 槽位寻址 | `seq % 32`（**同槽新 seq 会覆盖旧帧**，计 `reordered`） | `slots[seq]`（**不会碰撞覆盖**） |
| 出帧节拍 | 20ms（`now - last_play_us >= 20000`） | 20ms（`deadline += .02`） |
| 缺帧补齐 | **60ms** 未到 → 补 640B 静音，计 `lost` | **无补齐**，直接跳过（仅计 `miss`） |
| seq 重同步阈值 | 回退 **> 0x4000** 判重启 | 回退 **> 0x7FFF** 判重启（`diff > 0x7FFF`） |
| 包长校验 | `n != 652` → 计 `bad` | 由 `unpack_voice_frame` 抛 `ValueError` |
| 诊断周期 | 2s | 2s |

**已知的两处不对称（当前无害，但需知情）**：

1. **重排深度不一致**：设备 32 槽 vs PC 无界。
   设备端槽位复用（`seq % 32`）意味着**乱序深度超过 32 帧时会覆盖尚未播放的帧**；
   PC 端不会覆盖，但**长会话下字典可能持续增长**（内存无上限）。
   当前场景（PC 作为下行发送方、设备作为接收方）**不受影响**，
   因为重排逻辑只在**接收侧**使用，两端不会同时是接收方。
   若未来双向对讲改为双端都做重排，需重新评估；
2. **静音补齐策略不一致**：设备补静音（保时间轴），PC 直接跳过（丢帧即丢时长）。
   故 **PC 侧回放时长会短于实际录制时长**，设备侧不会。
   若需长期录音对齐，应统一为设备侧策略。

> ⚠ **已知边界**：`spk_rx_rb_pool` 为 3840B，而"保留最新 6 帧" = 6×640 = 3840B，
> **两者恰好相等**，稳态实际只能保住 5 帧多。详见
> `docs/工程规范/12_音频链路架构.md` §4.1。

#### 3.2.3 播放侧水位（`audio_service`，设备端专有）

低于 2 帧补静音，高于 **6 帧**丢最旧帧防延迟累积。实现见 `audio_stream_write_spk()`：

```c
while (ringbuffer_data_len(&spk_rx_rb) + len > 6 * AUDIO_FRAME_BYTES) {
    /* 丢弃最旧帧 */
}
```

> 注意：这是**播放缓冲策略**，与 §3.2.1 的**接收重排策略**是两个独立的层，
> 都按 20ms 帧计量但触发条件不同。

### 3.3 时钟与延迟语义（重要）
- 双端各自本地时钟、**不做时钟同步**；`ts_ms` 只用于**单端**抖动排序与到达间隔统计；
- **禁止**用"设备 ts_ms 与 PC 本地 now 相减"算端到端延迟（基准不同，结果错误）；
- 端到端延迟/RTT 由信令握手（HEARTBEAT 往返）估算，抖动用相邻 seq 到达间隔估算。

---

## 4. 信令协议（UDP 50003，JSON / UTF-8，单行一个对象）

### 4.1 通用信封
```json
{ "type": "<MSG_TYPE>", "ts": 12345, "ver": 1 }
```
- 每条信令一个 JSON 对象，`\n` 分隔（可选）；未知 type 忽略但不报错（前向兼容）。

### 4.2 消息类型

| type | 方向 | 关键字段 | 语义 |
|---|---|---|---|
| HEARTBEAT | PC→设备 | sw_ver, listen_ports | PC 每 **200ms** 发；设备据此学习 PC IP、刷新在线 |
| HEARTBEAT_ACK | 设备→PC | fw_ver, proto_ver, sample_rate, ip | 设备回复能力信息，PC 校验兼容 |
| SESSION_START | 双向 | mode("udp"/"tcp") | 请求开始会话 |
| SESSION_STOP | 双向 | reason | 结束会话 |
| PTT_DOWN | PC→设备 | — | PC 按住对讲，下行音频开始 |
| PTT_UP | PC→设备 | — | PC 松开，下行音频停止 |
| MODE_SWITCH | 双向 | mode | 预留：UDP/TCP 模式切换 |

### 4.3 心跳与在线状态机

以下为**已实现的实际参数**（V1.2 按代码回填，原"2s 超时"为失实描述）：

| 项 | 实际值 | 设备端 | PC 端 |
|---|---|---|---|
| PC 发送 HEARTBEAT 周期 | **200ms**（`now - last_send >= 0.2`） | — | `net/link.py` |
| 设备巡检周期 | **500ms**（`select` 超时 `500000` us） | `link_manager.c` | — |
| 设备判 OFFLINE 阈值 | **5s** 未收到（`> 5000000` us） | `link_manager.c` | — |
| PC 判离线阈值 | **5s** 未收到 ACK（`now - last_ack > 5`） | — | `net/link.py` |

> ⚠ **V1.0 曾写"PC 每 1s 发 / 2s 判离线"，与实际不符**：
> 实际是 **PC 每 200ms 发**、**双端均 5s 判离线**。
> 5s 阈值给了较宽的容错窗口（相当于容忍连续丢 25 个心跳包），
> 代价是掉线检测延迟最长 5s。

设备端状态机：

```
OFFLINE(红) ──收到 HEARTBEAT──▶ ONLINE(绿)
     ▲                              │
     └──── 5s 未收到 HEARTBEAT ──────┘
```

- 设备收到 HEARTBEAT 后：学习/刷新 peer IP → 回 `HEARTBEAT_ACK` → 刷新 `s_last_heartbeat_us`；
- **仅在 OFFLINE → ONLINE 的跃变时**才通知观察者（`LINK_EVT_ONLINE`），
  后续心跳只刷新时间戳不重复通知；
- 转为 OFFLINE 时通知 `LINK_EVT_OFFLINE`，通话中掉线由 UI/CALL tab 主动下发
  `VOICE_CMD_TX_OFF` / `VOICE_CMD_RX_OFF`；
- PC 侧 5s 未收到 HEARTBEAT_ACK → 显示设备离线。

### 4.4 能力协商
HEARTBEAT_ACK 的 `proto_ver` 与 PC 期望不一致时，PC 弹兼容提示；
`sample_rate` 不一致拒绝建立会话，避免错采样导致变速/噪声。

---

## 5. 模式二：TCP 录音与文件同步（端口 50002）

### 5.1 总体
- TCP 可靠字节流，**接收方必须按应用层分帧切包**（不能依赖 recv 分包边界）；
- 控制消息走 50003 JSON（REC_*），文件数据走 50002；
- 一次上传 = 三段：`REC_BEGIN → 数据块序列 → REC_END`。

### 5.2 REC_BEGIN（上传起始，经 50003 信令或 50002 首条 JSON 行）
```json
{ "type":"REC_BEGIN", "session_id":"<uuid>", "name":"rec_20260915_101500.wav",
  "sample_rate":16000, "channels":1, "bits":16, "total_bytes":480044, "ts":0, "ver":1 }
```
PC 回复 `REC_BEGIN_ACK {session_id, accepted:true}`；不兼容（采样率不符）回 accepted:false。

### 5.3 数据块（50002，长度前缀分帧）
每块：`chunk_len(4,LE) | offset(4,LE) | crc16(2,LE) | payload(chunk_len-6)`
- chunk_len 建议 4096B（含 6B 头），payload ≤4090；
- offset 为该块在文件 PCM 区的字节偏移，**支持断点续传/乱序重发**；
- crc16/CCITT 校验 payload，错误则 PC 发 `REC_RESEND {offset}`，设备重发该块；
- PC 按 offset 写入正确位置，重复块幂等忽略。

### 5.4 REC_END（结束）
```json
{ "type":"REC_END", "session_id":"...", "total_bytes":480044, "crc_file":"..." }
```
PC 校验总长度/CRC，回填 44B WAV 头，入库并回 `REC_END_ACK {saved:true, id:...}`。

### 5.5 控制信令（50003，模式二新增）
| type | 方向 | 语义 |
|---|---|---|
| REC_START / REC_STOP | PC↔设备 | 开始/中止一次录音 |
| REC_BEGIN / REC_BEGIN_ACK / REC_END / REC_END_ACK | 设备→PC / PC→设备 | 文件上传握手与收尾 |
| REC_RESEND | PC→设备 | 请求重发指定 offset 块 |
| REC_DELETE | PC 内部 | 删除 PC 本地录音（不下发设备） |
| REC_LIST_REQ / REC_LIST_RSP | 双向 | 列举设备本地待传文件 / PC 库列表 |

### 5.6 边录边传形态（预留）
- 复用 §3 的 12B 音频帧在 50002 上连续发送，PC 按帧头 len 粘包切帧、边收边落盘；
- SESSION_START(mode=tcp) 建立，SESSION_STOP 收尾回填 WAV，语义与先录后传一致。

### 5.7 STREAM / LOCAL 双模与断点拼接（V1.1）
- REC_START 由设备按 PC 在线情况自选 STREAM（边录边传）或 LOCAL（离线缓存，≤300s PCM）；
- STREAM 中断网：设备转 LOCAL 从断点写本地，已发部分 PC 侧保留；恢复后补传，
  补传 REC_BEGIN 带 `base_offset`（已发字节数），PC 从该偏移续写，最终拼成一段完整 WAV；
- LOCAL 录完 STOP 后入待传队列，PC 上线后正常走 REC_BEGIN→分块→REC_END；
- PC 不区分来源，统一按 session_id + offset 落盘入库。

---

## 6. 变更记录

- V1.0（2026-09-14）：初版，定义采样契约、12B 音频帧、50003 JSON 信令、心跳状态机、TCP 预留。
- V1.1（2026-09-15）：补 TCP 文件三段式协议（REC_BEGIN/分块 CRC/REC_END）、STREAM/LOCAL 双模与 base_offset 断点拼接、时钟语义。
- **V1.2（2026-09-21）：按代码回填，修正三处失实描述**。
  起因：梳理音频链路时把"文档表格"与"两端代码"逐项对表，发现已漂移
  （详见 `docs/工程规范/11_遇到的问题.md` F26）。

  | 项 | V1.0/V1.1 原描述 | V1.2 实际（按代码） |
  |---|---|---|
  | 重排窗口 | "16 帧" | **32 槽**（设备端）／无界字典（PC 端） |
  | 缺帧补齐 | "超过 40ms" | **60ms** |
  | 心跳周期 | "PC 每 1s 发" | **每 200ms** |
  | 离线判定 | "2s 未收到" | **双端均 5s** |

  同时补充：
  - §1 增加"控制类常量"表（重排窗口 / 补齐阈值 / 重同步阈值 / 心跳 / 水位 / 信令包缓冲）；
  - §3.2 改写为「公共语义 + 双端实现参数对照 + 已知不对称」三段结构，
    明确记录设备端 32 槽 vs PC 端无界字典、设备补静音 vs PC 直接跳过 两处不对称；
  - §4.3 补设备端状态机与跃变通知语义。

  **编制原则确立**：本文描述**已实现的现状**，代码是唯一事实来源；
  文档与代码不一致时以代码为准并回填本文。

- **V1.3（2026-09-24）：PC 音乐副屏推流（NOW_*）落地并实机验收通过**。
  增量内容全部在 §7，本节只记验收事实与三处**易被误读**的点：

  1. §7.3 `/lyrics.json` 的 **`t` 单位恒为毫秒整数**（F45）——四端已对齐
     （`netease.parse_lrc` 产出口 / `http_server` 序列化 / `pusher` 比较 / C# `Program.cs`），
     设备端另有**单位哨兵**兜底并告警。
  2. §7.5 `line` / `sync_src` 已落地，**但设备端不再是"有 line 就采信"**——
     见 §7.5 内**修订后**的取行规则（取"本机带提前量二分"与"PC line"两者**更靠后者**）。
  3. 设备端 `pcnow_service.c::PCNOW_LYR_LEAD_MS = 1250` 是**设备本地实现细节，不是 ICD 字段**：
     双端契约里没有 lead 字段，PC 端不需要也不应知道这个值（详见 §7.5 末注）。

  验收判据（2026-09-24 实机）：`init done (udp/50004, http pull :50005, lyric lead 1250 ms)`；
  换行时刻比补偿前早约 1.25s；seek 后仍对齐；封面 240×240 就绪、无 `cover decode failed`；
  换曲后三行歌词随进度滚动、`/lyrics.json` 的 `t` 为 5~6 位数。

## 7. V1.3 增量：PC 音乐副屏推流（NOW_*，2026-09-23 起）

> ⚠ **编号修正（V1.3）**：本节原误编号为"## 6."，与"变更记录"重号；现改为 7。

> 服务背景见 `PC音乐副屏局域网推流实施方案.md` V2。设备端实现：
> `main/middleware/network/pcnow_service.c`；PC 端实现（**两套等价，二选一运行**，
> 同时跑会 50004/50005 绑定冲突）：`tools/pc_now_agent/`（C# 控制台）或
> `pc_host/pcnow/`（统一上位机 v2 的音乐副屏页）。50000-50003 全部不变，
> 本节只加端口与消息。

### 7.1 新增端口

| 端口 | 协议 | 方向 | 用途 |
|---|---|---|---|
| 50004 | UDP | **PC → 设备** | NOW_META / NOW_PROG / NOW_LRC（JSON 单行，≤1 datagram/条） |
| 50005 | HTTP | 设备 → PC | 只读拉取：`/cover/<hash>.jpg`、`/lyrics.json?k=<hash>` |

### 7.2 NOW_* 消息（UDP 50004）

| type | payload | 推送时机 |
|---|---|---|
| NOW_META | title/artist/album/player/duration_ms/lrc/cover/hash/playing/**sync_src** | 换曲立即 + **3s 周期重发** + 曲长/同步源变化立即补发 |
| NOW_PROG | pos_ms / playing / hash / **line** | **1Hz** |
| NOW_LRC | text / index | 行变化（v1 回退通道，与全量歌词并存） |

- `lrc` / `cover` 字段为 **hash 字符串**（SHA1(title|artist) 前 8 hex）；
  空串 = 该资源不存在。设备端以 hash 判重，避免重复下载。
- `line` / `sync_src` 为 **V1.3 新增**，详见 7.5。**老固件忽略这两个字段即退回
  V1.2 行为**（设备按本机内插 `pos_ms` 二分查行），因此是向后兼容的纯增量。

### 7.5 V1.3 增量：行号通道 `line` 与同步置信度 `sync_src`（2026-09-24）

**动机**：网易云 Win32 等 CEF 播放器的 SMTC **没有时间轴**（实测 `position=end_time=
timedelta(0)`、`last_updated=1601`），进度只能靠 PC 本地内插，且从"PC 开始计时的时刻"
起算 ⇒ 上位机中途启动时会整体偏移，歌词跟着错。
完整调研见 `PC副屏歌词同步-位置真值方案讨论与执行计划.md`。

**`NOW_PROG.line`（PC → 设备，整数）**

| 值 | 含义 |
|---|---|
| `>= 0` | 当前歌词行号（0 起），由 PC 用 `bisect(lrc_t, pos)` 算出，**并做过程序化迟滞** |
| `-1` | 无歌词 / 未知 → 设备退回按 `pos_ms` 本机二分 |

PC 端的**行号迟滞规则**（`pcnow/pusher.py::_stable_line`）：

- 允许「保持」或「前进」（含人工校准后的大跳）；
- **回退必须连续 3 拍（3s）确认**才生效。

理由：人耳对"歌词来回跳"的容忍度远低于对"整体偏移"的容忍度。位置抖动 1~2 s 时，
二分结果会在相邻两行反复横跳，在 PC 端一次性抑制掉，设备端只收到单调干净的行号。

设备端取行规则（`pcnow_service.c::pcnow_service_get_info`，**F47 修订，2026-09-24**）：

| 量 | 定义 |
|---|---|
| `idx_pc` | `s_prog_line ∈ [0, s_lrc_count)` 时取它，否则 **-1**（越界/无解一律弃用） |
| `idx_local` | 本机二分 `lrc_find_locked(pos_now + PCNOW_LYR_LEAD_MS)`（**带提前量**） |
| **最终行号** | `idx = max(idx_pc, idx_local)` —— 取两者中**更靠后者** |

- **为什么不再是"有 line 就采信"**（修订前的写法）：设备端要对歌词做时间提前量补偿，
  而 `line` 是**行号**——行号/索引/枚举**无法被时间偏移修正**。若"line 优先"，补偿会在
  line 有效时**静默失效**（现象：改了提前量宏毫无变化，或时灵时不灵，见 F47）。
- **为什么取 `max`**：正常播放时本机值恒 ≥ PC 值（本机连续内插 + 提前量，而 line 只有 1Hz
  刷新且**未补偿**）⇒ 生效的是本机值，**补偿真正落地**；播放器位置抖动时本机值可能突然
  回退，此时取 PC 那个**做过迟滞**的 line ⇒ 仍不会来回跳，PC 侧 `_stable_line` 的价值保住。
- **越界**（-1 / 越界 / 收到 `NOW_CMD seek` 后本地拖动期间）一律把 `idx_pc` 置 -1：
  说明两端歌词不同源（换曲竞态），此时本机二分至少不会错得更离谱。
- 诊断：`lrc_line_from_pc = (idx_pc > idx_local)` 记录本拍谁赢。

> ★ **`PCNOW_LYR_LEAD_MS` 不属于本 ICD**：它是设备端**实测标定**的内部常量，用于补掉链路上
> 累积的**固定偏置**（PC 的 SMTC/内插位置本就滞后于听感，且 NOW_PROG 只有 1Hz、两拍之间不刷新）。
> 双端契约里**没有** lead 字段，**PC 端不需要也不应知道这个值**。
> ⚠ 该值绑的是"当前这套 PC + 播放器 + 输出链路"，换播放器后若偏移不同，**别再加常量**，
> 正确做法是把 lead **上移到 PC 侧**（`pusher` 按 `player` 决定 `lead_ms` 随 `NOW_META` 下发）——
> 那时才需要在 §7.2 的 NOW_META 字段表里新增该字段并升版本号。

**`NOW_META.sync_src`（PC → 设备，字符串）**

| 值 | 含义 | 设备 UI 显示 |
|---|---|---|
| `timeline` | 播放器上报了真时间轴（B 站/UWP 等） | 同步准确 |
| `dom` | 从浏览器/CEF 的 CDP 读了 DOM | 同步准确 |
| `a11y` | 从界面无障碍树读了位置 | 同步准确 |
| `manual` | **用户拖滑条做过人工校准**（锚点已被重设） | 已校准 |
| `event` | 仅靠换曲/暂停事件锚定（可能整体偏移） | 本地内插 |
| 空串/缺省 | 老版 PC | 按 `event` 处理 |

**人工校准用的是 `NOW_CMD seek`（复用既有消息，不新增类型）**：

- 设备拖滑条 → `pcnow_service_send_seek(pos_ms)` → PC `main.py::on_link_json`；
- PC 端 `pusher.anchor(target, source="manual")` 把内插时钟重锚到目标，并打印
  `校准(锚点): 内插 x.x s → 实际 y.y s（偏差 ±z.z s）`；
- **若该播放器无时间轴，PC 端跳过 `try_change_playback_position_async`**（网易云曲内
  跳转本就无解），仅重锚内插时钟 —— 语义从"seek"变成"校准"；
- 有真时间轴的播放器仍照旧发 SMTC seek，下一拍 `on_position` 会用真实位置再校准。

### 7.3 HTTP 拉取（50005，PC 端为 TcpListener 手写 HTTP/1.1，免 URLACL）

- `GET /cover/<hash>.jpg` → 240×240 JPEG（≤64KB），PC 端已缩放；
- `GET /lyrics.json?k=<hash>` → `{"lines":[{"t":<ms>,"text":"<...>"}...]}`，
  **已归一化**（PC 完成 LRC 解析/多时间标签展开/排序），设备只做二分查行；
  - ★★ **`t` 的单位是毫秒（整数），这是硬契约**（F45）。曾因 PC(Python) 侧按**秒**
    下发、而设备端 `(uint32_t)valuedouble` 与 C# agent 按**毫秒**用，导致设备端
    `s_lrc_t[]` 存进个位数、二分**恒返回最后一行** ⇒ 歌词定在最后一句不动。
    定义方/实现方共 4 处，**改单位必须同时改这 4 处**（ID 仅在实现里）：
    `pc_host/pcnow/netease.py::parse_lrc`（★ 产出口）、`pc_host/pcnow/http_server.py`
    （原样序列化）、`pc_host/pcnow/pusher.py`（`NOW_LRC.t` 与 `_line_for` 比较单位）、
    `tools/pc_now_agent/Program.cs`（C# 版，本来就是 ms）。
    设备端已加**单位哨兵**：末行 `t<=3600` 且曲长 `>=60s` 时打 `ESP_LOGW` 并 ×1000。
- 设备端拉取时机：META 里的 hash 与本地已持有 hash 不同才拉。

### 7.4 歌词 v1.1 语义（时间轴在设备）

- 设备端保存全量行数组，用自己的**内插时钟**（NOW_PROG 打点 + esp_timer 外推，
  暂停冻结、越曲长封顶）二分得出当前行；
- ★ **歌词与进度条同源但不同偏移**（F47）：进度条/时间用**未补偿的真值** `pos_ms`
  （否则进度条会虚报提前量），歌词行另加设备内部提前量 `PCNOW_LYR_LEAD_MS`
  （见 §7.5 末注）。两者由同一内插时钟派生，故暂停/seek/换曲时仍然一起动；
- NOW_LRC 单行推送保留为回退：全量歌词缺失/解析失败时仍可显示；
- NOW_CMD（设备 → PC，经 50003）：`{"type":"NOW_CMD","cmd":"play_pause|next|prev|volume_up|volume_down"}`，
  本期为预留，PC 端仅打印。

---

## 8. V1.4 增量：WiFi 局域网 OTA（OTA_*，2026-09-29）

> 方案出处：`docs/项目规划/WiFi局域网OTA升级实施方案.md` §2。
> 设计要点：**控制面复用 UDP 50003 既有 JSON 通道（零新增端口）**，
> **数据面由设备主动 HTTP 拉取 PC 的 :50005**（与封面/歌词拉取同构：
> 设备是 HTTP 客户端、PC 是服务端）。

### 8.1 消息（UDP 50003，`type` 字段区分，cJSON）

| type | 方向 | 字段 | 说明 |
|---|---|---|---|
| `OTA_OFFER` | PC→设备 | `ver`(string) `size`(number) `sha256`(hex64 string) `url`(string) | 升级提议；`url` 是设备可直接 GET 的完整地址 |
| `OTA_ACCEPT` | 设备→PC | `size`(number) | 校验通过，即将开始拉取 |
| `OTA_REJECT` | 设备→PC | `reason`(string) | 拒绝原因，见下表 |
| `OTA_PROGRESS` | 设备→PC | `percent`(0-100) `speed`(KB/s) | 每 2% 一条 |
| `OTA_RESULT` | 设备→PC | `ok`(0/1) `detail`(string) | ok=1 后设备 1s 内重启 |
| `OTA_ABORT` | PC→设备 | `reason`(string, 可选) | 中止；**M2 起真正生效**：设备下载循环每 4KB 块边界检查标志，立即停手（不写完当前块） |

**`OTA_REJECT.reason` 取值语义**：

| reason | 含义 |
|---|---|
| `busy` | 录音中 / 通话或推流中 / 已在升级（三选一） |
| `pending` | 运行槽处于 PENDING_VERIFY（自证窗口内，IDF 规定不能再写另一槽） |
| `space` | 备用槽装不下该 size |
| `network` | link_manager 未在线 |
| `badargs` / `badjson` | OFFER 字段缺失 / JSON 解析失败 |

### 8.2 HTTP 路由（PC :50005 新增两条）

| 路由 | 响应 |
|---|---|
| `GET /firmware.json` | 200 `{"ver":"...","size":N,"sha256":"hex64","full_size":M,"limit_percent":P}`（设备二次核对，防 UDP 丢包拿到错信息） |
| `GET /firmware.bin` | **M2 支持 Range**：无 `Range` 头 → `200` + `Content-Length: N`；带 `Range: bytes=N-` → **`206 Partial Content`** + `Content-Range: bytes N-(M-1)/M` + `Accept-Ranges: bytes`，body 从偏移 N 起 |

> 两条路由**仅在 `--ota` 会话期间存在**：固件解析成功才注册（"就绪才宣告"），
> 解析失败则整个进程直接退出，绝不产出半成品路由。

### 8.2.1 M2 断点续传契约（★ 跨层语义，改前必读）

**分段哈希校验**：固件按 **256KB** 切"锚点段"（`OTA_SEG_SIZE`），设备端每段**独立**
算 SHA256 并连同 `next_offset` 落 nvs（`ota` namespace / key `resume`）。

| 项 | 契约 |
|---|---|
| 段大小 | 256 KB（`OTA_SEG_MAX=24` ⇒ 支持到 6 MB） |
| 续传起点 | **回退到最近段边界**（`seg_done * 256KB`），最多重算 256KB 尾巴 |
| 生效条件 | `total_size` 与 `sha256` 都与本次 OFFER 一致，且 `seg_done ≤ next_offset/256KB` |
| Range 请求 | 设备发 `Range: bytes=<start>-`（start = 段边界） |
| 服务端降级 | PC 回 **200**（不支持 Range）⇒ 设备**自动改整包重下**（避免槽内容错位） |
| 完整性 | ① `esp_ota_end`（镜像头 + 段 CRC）；② 段摘要链；③ **段数一致性硬校验** `seg_done == ceil(size/256KB)` |
| 断点保留 | 失败（非 ABORT）保留 nvs 断点供下次续传；成功切槽 → 清除 |

> ★ **SHA-256 是流式哈希，跨重启无法从中途累积** —— 所以断点续传**不**做"整包 SHA 从偏移续算"，
> 而是"段摘要链 + 镜像校验"双兜底。整包 SHA256 仅用于 **PC 侧自校验 / 从头下载的交叉核对**。

### 8.3 端侧状态机

```
IDLE → (OTA_OFFER 校验通过) OFFERED → DOWNLOADING → VERIFY
     → REBOOT_PENDING → [esp_restart] → PENDING_VERIFY
     → (30s 自证: WiFi 已连) → VALID      （失败则 bootloader 自动回滚）
```

### 8.4 心跳字段联动（V1.3 已定，此处强调）

`HEARTBEAT_ACK.fw_ver` 已由硬编码 `"phase2"` 改为 `esp_app_get_description()->version`
（真实编译版本）。**版本三方一致是 OTA 判定"升级成功 / 已回滚"的唯一依据**：
`bin 内 esp_app_desc_t.version` == `HEARTBEAT_ACK.fw_ver` == PC 侧解析值。

### 8.5 PC 端版本策略与破坏性测试开关（M2/M3 新增）

| 开关 | 语义 | 预期设备端反应 |
|---|---|---|
| `--ota-limit N` | 只发前 N%（1..99），`/firmware.json` 与 `OTA_OFFER.size` 均报**截断长度** | 设备端 `size mismatch` → 保留断点 → 续传可补齐 |
| `--ota-corrupt` | 流中点翻 1 bit | `esp_ota_end` 失败（镜像段 CRC）/ 段摘要不符 |
| `--force` | 忽略 SemVer 比对 | 同版本/降级也照发 |
| （默认） | 设备 `fw_ver >=` 固件版本 ⇒ **拒发**（退出码 8） | 设备端不会收到 OFFER |

**SemVer 粗比规则**：按 `.` 分 3 段数值比较；容忍 `v`/`V` 前缀；`-dirty`/`+build` 后缀不参与；
缺位补 0。★ 仅用于"要不要升"的粗判，**不参与安全/完整性判定**（那是 SHA256 的事）。

**在线判定与主动中止**：会话期（已 ACCEPT、未收到 RESULT）设备心跳丢失 > **8 s** ⇒
PC 主动发 `OTA_ABORT{"reason":"peer_lost"}`。★ 这依赖 `DeviceOfflineMs=5000`（心跳超时）先置离线。

---

## 9. V1.5 增量：PC 壁纸上传（WP_*，2026-10-07）

> 方案出处：`docs/项目规划/SD卡壁纸与PC壁纸管理实施方案.md` V2.1（§76）。
> 与 §8 同构：**控制面复用 UDP 50003 JSON 通道，数据面复用 TCP 50005**，零新增端口。
> PC 端实现：pc_host v0.4（`wallpaper.py` + 系统页"壁纸管理"卡片 + httpd `/wallpaper/<name>` 路由）。

### 9.1 信令（UDP 50003，JSON / UTF-8）

| 方向 | 报文 | 字段 | 说明 |
|---|---|---|---|
| PC → 设备 | `WP_OFFER` | `name`（池内纯文件名）、`size`（字节）、`sha256`（**保留字段，设备端忽略**）、`url`（`http://<pc>:50005/wallpaper/<name>`） | PC 本地已预校验：≤2MB + JPEG SOF = 800x480 |
| 设备 → PC | `WP_RESULT` | `ok`（0/1）、`name`、`err`（失败原因短语，成功为空串） | 回包固定发 PC:50003（端口覆写，见 §8.1 同一依据） |

### 9.2 设备端行为（wallpaper_service §76）

收到 WP_OFFER → `wp_fetch` 任务（**内部 RAM 栈**，F35：下载要写 SD 卡）依次执行：

1. 前闸：SD 已挂载、轮换服务非降级态，否则 `WP_RESULT ok:0`；
2. HTTP GET 流式落 SD（先写 `/sdcard/wallpaper/.wp_tmp` 临时名），8KB 块 + **2MB 硬闸**；
3. 回读对账（stat 与已写字节数 + PC 报的 size 三方一致）；
4. `wallpaper_service_validate_file()`：JPEG 头部 parse，必须 800x480；
5. `rename` 落池（FATFS 不许覆盖，先 unlink 旧同名）→ `request_set()`；
6. `WP_RESULT` 回包。**任何一步失败：删临时文件、当前壁纸不动。**

成功后 UI 经 §75 代际号轮询自动 crossfade，NVS `wp_last` write-through。
`name` 校验双闸：设备端拒路径分隔符/引号 + 后缀必须 .jpg/.jpeg（服务端池扫描再验一次，防路径注入）。

### 9.3 PC 端数据面（httpd :50005）

`GET /wallpaper/<name>` —— **就绪才宣告**：仅上传会话活跃时存在（`WallpaperSession.start()`
attach、收尾 detach），名字与当前会话不符即 404；`sha256` 忽略的依据：TCP 保证传输完整性，
落池后 validate_file 为最终闸。

### 9.4 设备侧 grep 关键字

- `grep "\[WP\]"` → 壁纸上传全链路（offer rejected / fetched / fetch failed / fetch ready）
