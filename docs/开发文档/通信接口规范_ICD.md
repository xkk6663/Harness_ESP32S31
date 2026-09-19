# 设备 ↔ PC 通信接口控制文档（ICD）V1.0

> 本文件是设备端（ESP32-S31，C）与 PC 端（Python）必须共同遵守的**唯一通信契约**。
> 双端常量、字节序、帧格式、状态语义必须严格一致；任何改动先改本文件再改代码。
> 适用模式：模式一 UDP 实时通话（当前）；模式二 TCP 录音复用同一帧头与信令（预留）。
> 最后更新：2026-09-14

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
| 音频帧头 | 12 B | 见 §2 |
| 单包总长 | 652 B | 远小于 1500 MTU，**不分片** |

设备端以 C 宏定义、PC 端以 Python 常量定义，数值必须逐项相等。

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

### 3.2 接收端处理语义（双端一致）
- magic/ver 不符 → 直接丢弃并计 `bad_pkt`；
- 按 seq 排序进 16 帧重排窗口；乱序帧按 ts 归位；
- 期望帧超过 **40ms** 未到 → 该帧补**静音（640B 全 0）**，计 `lost`，绝不阻塞等待；
- seq 回绕按 uint16 模比较；重复 seq 丢弃；
- 播放侧水位：低于 2 帧补静音，高于 6 帧丢最旧帧防延迟累积。

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
| HEARTBEAT | PC→设备 | sw_ver, listen_ports | PC 每 1s 发；设备据此学习 PC IP、刷新在线 |
| HEARTBEAT_ACK | 设备→PC | fw_ver, proto_ver, sample_rate, ip | 设备回复能力信息，PC 校验兼容 |
| SESSION_START | 双向 | mode("udp"/"tcp") | 请求开始会话 |
| SESSION_STOP | 双向 | reason | 结束会话 |
| PTT_DOWN | PC→设备 | — | PC 按住对讲，下行音频开始 |
| PTT_UP | PC→设备 | — | PC 松开，下行音频停止 |
| MODE_SWITCH | 双向 | mode | 预留：UDP/TCP 模式切换 |

### 4.3 心跳与在线状态机
- PC 周期 1s 发 HEARTBEAT；设备每 500ms 巡检，**2s** 未收到 → OFFLINE；
- 设备状态：OFFLINE(红) ↔ ONLINE(绿)；通话中掉线自动 SESSION_STOP；
- PC 侧 2s 未收到 HEARTBEAT_ACK → 显示设备离线。

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
