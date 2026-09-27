# S5 音频链路结构 · MP3 + 小智落地 · 可执行方案 v1

> **执行状态（2026-09-26）**：M0 ✅ / M1a ✅（partitions.csv **v1.1**：布局与已烧录
> 版本一致，仅 srmodels→model 改名）/ M1b ✅（ext 协议）/ **M2 ✅ 代码落地**
>（media_player_service + FILES 路由 + MP3 态 UI）/ **M3 ✅ 代码落地**
>（vendor esp_xiaozhi + xiaozhi_service + yml/CMake）/ **M3.5 ✅ 依赖排雷 + 0.7.0 API 对齐**
>（media_lib_sal 1.0.0 + esp_hls_stream 1.0.4 预置并显式进 yml；VIDEO_PROCESSOR/HLS 关；
> xiaozhi_service.c 按 av_processor 0.7.0 重写：IO 回调制/句柄制/algo_mask/esp_event 两路事件；
> 静态校验 **2/2 OK 零警告**，详见 `改动记录.md` §36.7）。
>
> **★ 落地结果（2026-09-27 验收通过）**：M4/M5 ✅ 小智对话多轮闭环成立。
> 本文是设计方案（历史存档），**与最终实现的偏差清单见文末 §9**；
> 源码现状权威快照 = `改动记录.md` §51（最终交互模型/代码清单/日志契约/搁置项）。

> 2026-09-26 · 供审阅。回答三个问题：
> ① 本项目音频链路应该长成什么样（结构性讨论）；
> ② 本地 MP3 播放在这个结构里的位置（S4b 路线在小智背景下重议）；
> ③ 小智（esp_xiaozhi）怎么落进本分支（分区表按 §28 现状重算，不照搬桌面方案旧布局）。
>
> 结论先行：**MP3 与小智合并立项、共享一份 GMF 依赖树；audio_service 升级为全链路
> 唯一 codec 仲裁者（ext 协议两种模式）；唤醒词 v1 因内部 RAM 不足降级为按键触发。**

---

## 1. 现状盘点（音频资产与那个 P0）

| 组件 | 持有资源 | 行为 |
|---|---|---|
| `audio_service` | **`spk_codec_dev`（ES8389，同一 `i2s_data_if`）** | WAV 播放（按文件头 open + 逐块 write）；`audio_stream_start` 全双工 16k（`read_mic`/`write_spk`，udp_voice 通话用）；`SET_VOLUME` |
| `record_service` | 借 audio_service 的 mic 通路 | 录音到 WAV |
| `udp_voice` | ctl/tx/rx 三任务 | 局域网语音（rx 下行走 `audio_stream_write_spk`） |
| （main 主线）`tab_audio` | **自建 `bsp_audio_codec_speaker_init()` 第二句柄** | MP3 播放 44100/2/16 —— 这就是 P0：同一 I2S 双句柄无仲裁 |

**P0 的本质不是"句柄重复"而是"没有所有者"**：谁想出声谁自己 init codec，
互相不知道对方存在。所以正解不是运行时规避，而是**收权**——
全项目只允许 audio_service 碰 codec，其它一切音频客户端走它暴露的协议。

## 2. 目标音频链路结构

### 2.1 结构图

```
                     ┌────────────────────────────────────────────┐
   下行(spk 出声)     │            audio_service（唯一所有者）        │
                     │   spk_codec_dev + vol + 全双工 stream        │
                     └──────┬───────────┬───────────┬──────────────┘
                            │           │           │
        WAV 提示音(内部) ────┘           │           └──── udp_voice rx(通话下行)
                                        │
              ┌─────────────────────────┴──────────────────────────┐
              │                ext 协议（两种模式）                   │
              │  ① 流式:  ext_claim(fmt)→ext_write()→ext_release()  │
              │     客户端: MP3/AAC(esp_player out_writer)、         │
              │             未来 TTS-PCM 流                          │
              │  ② 独占租借: ext_claim_exclusive()→自建 codec_dev→   │
              │     ext_release_exclusive()                         │
              │     客户端: 小智(AFE 全双工管线自己掌握 mic+spk 时序)  │
              └────────────────────────────────────────────────────┘

   上行(mic 收音)    record_service / udp_voice tx / 小智 AFE
                     —— 三者互斥，audio_service stream 通道统一闸门
```

### 2.2 为什么小智要"独占租借"而不是逐块 ext_write

小智的 AFE（esp-sr）管线是**全双工自驱动**的：它自己决定何时读 mic、何时写 spk，
且 AEC 需要回采参考与扬声器输出严格同步。把它的 PCM 拽出来走 `ext_write`
等于把它的时间轴打散。正确姿势：**对话态把整个 codec（spk+mic）租给它**——
audio_service 保持 IDLE、dev close，小智的管线在**同一个 `i2s_data_if` 上**
自建句柄 open；`ext_release_exclusive()` 归还。I2S 外设同一时刻只有一个
open 状态的句柄 ⇒ P0 从设计上消灭，而不是运行时赌运气。

MP3 则相反：esp_player 的 out_writer 本来就是"解码一帧喂一帧"的流式模型，
逐块 `ext_write` 天然契合，还能让 `SET_VOLUME` 继续生效（同句柄）。

### 2.3 ext 协议 API（S4b §2.2 的升级版）

```c
/* audio_service.h 新增 */
typedef struct { int sample_rate; int channels; int bits; } audio_ext_fmt_t;

/* 模式① 流式：拿到后以 fmt open dev，期间 WAV/录音/通话/ext 全拒（BUSY） */
esp_err_t audio_service_ext_claim(const audio_ext_fmt_t *fmt);
esp_err_t audio_service_ext_write(const void *pcm, size_t bytes); /* 16bit 对齐 */
void      audio_service_ext_release(void);   /* close dev + 归还 */

/* 模式② 独占租借：本服务 dev close，调用方在共享 i2s_data_if 上自建句柄 */
esp_err_t audio_service_ext_claim_exclusive(void);
void      audio_service_ext_release_exclusive(void);

/* mic 闸门：录音/udp_voice tx 在任何 ext 占用期间被拒 */
bool      audio_service_mic_available(void);
```

实现要点：一个 `s_ext_mode`（NONE/STREAM/EXCLUSIVE）标志 + 现有 cmd 处理入口
统一判它。**WAV / 录音 / udp_voice 现有路径行为零变化**（M1 回归验证）。

### 2.4 抢占策略

v1 **不做运行时抢占**，只做"忙则拒 + UI 提示"。唯一例外：MP3 播放中用户按键
进入对话 → UI 先发 STOP（file_mgr 现有通道），等 `PLAY_DONE`/`ext_release`
落地后再 `claim_exclusive`，链路串行不并发，避免竞态。

## 3. MP3 重新定位：与小智共享 GMF 依赖树

S4b 搁置的理由是"太占内存"（esp_player 全家 flash +400~600 KB）。
小智入场改变这笔账：**esp_xiaozhi 本来就依赖 `gmf_ai_audio ~0.8`（esp-gmf 家族）**
—— 单独做 MP3 要扛整棵树，合并做只多付 esp_player/esp_extractor 这一小截
（估计 +100~200 KB 边际成本）。**S4b 路线 A 在小智背景下从"贵"变"顺手"。**

| 项 | 单做 MP3（S4b-A） | 合并（本方案） |
|---|---|---|
| GMF 依赖树 | 全额自担 | 与小智共担 |
| flash 边际 | +400~600 KB | +100~200 KB |
| 风险 | gmf 栈单独验证 | esp_player main 已验证；esp_xiaozhi vocat 同芯片已验证 |
| 若小智黄了 | — | MP3 独立成立（顺序上先交付） |

实施 = S4b §2.1/2.3 原样收编：`media_player_service`（main `tab_audio.c`
LOCAL 链路搬移，裁 BT PUSH/曲目列表/自建 codec）+ FILES 按扩展名路由
+ MP3 态 UI（无进度条 v1）。

## 4. 小智落地（按桌面方案，三处按本分支现状修正）

### 4.1 分区表（★ v2，2026-09-26 照抄 vocat 实证布局，取代 v1）

用户的观察促成了 v2：vocat 的分区表里没有叫 "srmodels" 的分区，但有**叫 `model` 的
模型分区（912KB）**，且把 **nvs 扩到 64KB**。三个实证全部采纳：

```csv
nvs,      data, nvs,     0x9000,   0x10000,    ← 64K：vocat 实证 16K 会被高频写爆
                                                （esp_wifi_set_config 报 NO_FREE_PAGES 崩机）；
                                                小智 keystore/激活凭证同样写 NVS。
                                                起始不变只扩大 ⇒ WiFi 配置保留
otadata,  data, ota,     0x19000,  0x2000,
phy_init, data, phy,     0x1b000,  0x1000,
model,    data, spiffs,  0x1c000,  0xE4000,    ← 912KB，vocat 实证装下"你好小智"唤醒词模型；
                                                名字对齐 esp-sr 生态默认（esp_srmodel_init("model")）
factory,  app,  factory, 0x100000, 0x700000,   ← 单 app 7MB，放弃 OTA（vocat 同款）
storage,  data, littlefs, 0x800000, 0x800000, ← 8MB 整！
```

校验：0x1c000+0xE4000=0x100000（恰 64K 对齐）→ 0x100000+0x700000=0x800000 →
0x800000+0x800000=**0x1000000** ✓。

- 对比 v1（srmodels 3MB + storage 5.875MB）：v2 的 model 省 2.1MB、nvs 加固、
  **storage 增至 8MB**。v1 的"为什么独立分区"论证不变（§4.1 原四条）。
- model 分区 v1 不烧录（无唤醒词，`ai_mode_wakeup=false`）；唤醒词回归时
  `esptool write_flash 0x1c000 srmodels.bin`，无需改表。
- 烧录流程：建议 `idf.py erase-flash`（分区大改）→ `idf.py flash` →
  `idf.py storage-flash`；erase-flash 后 WiFi 需重新配网（nvs 起始未变，
  不 erase 则配置保留）。

**★ 为什么 srmodels 不能塞进 littlefs 而要独立分区（2026-09-26 讨论）**

1. esp-sr 读模型不走文件系统：`esp_srmodel_init(分区名)` 内部 `esp_partition_mmap`
   把**整块分区**映射进地址空间，推理时零拷贝直读 flash（3MB 模型不进 RAM）。
   前提 = 一段物理连续的裸 flash。
2. littlefs 是日志式文件系统，文件数据与元数据交错、磨损均衡会挪位置，
   **文件在物理上不连续 ⇒ 无地址可 mmap**，结构性不可行。
3. `esp_partition` API 没有子分区概念；要实现只能魔改 esp-sr loader
   （mmap → littlefs 读 + 3MB 拷进 PSRAM），改厂商库 + 失去 zero-copy，收益为负。
4. 工程附带：srmodels.bin 是生成镜像按偏移烧录；独立分区让模型与用户数据
   （照片/录音/素材）彻底隔离，互不牵连格式化与升级。

**★ v1 机会点（已评估，维持预留）**：v1 无唤醒词 ⇒ wakenet/multinet 模型
用不上（AFE 的 AEC/NS/VAD 是纯算法不吃模型），srmodels 分区 v1 纯闲置。
曾评估砍掉换 storage 8.875MB，代价 = M0 增加"esp_xiaozhi 去 wakenet 化"
改造 + 将来唤醒词回归时全量重烧；**拍板维持预留 3MB**（storage 5.875MB
已够用，不值得为 3MB 在 vendor 管线上动刀）。

### 4.2 内部 RAM 与唤醒词（★ 第二处修正：降级路径是默认路径）

桌面方案预算 AFE 常驻 50~80 KB 内部 RAM。本分支现实：上电余量 ~4 KB +
§35 栈收缩刚省 ~32 KB ≈ **36 KB，且 WiFi 51.2 KB 内部 DMA 挪不走**。
**常驻 AFE 放不下，唤醒词「你好小智」v1 做不了**，这不是悲观是算术。

降级路径（推荐，即默认）：**AFE 按会话创建/销毁**——按键进入对话时
create AFE + pipeline（1~2 秒准备），退出即 destroy 归还。代价：
① 无免唤醒体验；② 会话建立有可感知延迟。收益：内部 RAM 峰值可控，
对话态与 MP3/通话天然互斥。

唤醒词列为 M6+ 远期目标（等任务栈静态化 + 内部 RAM 手术后重估）。

### 4.3 三态仲裁表（本分支无 BT，比桌面方案简化）

| 模式 | audio_service | MP3 | 录音/udp_voice | AFE/esp-sr | mic | spk |
|---|---|---|---|---|---|---|
| IDLE | IDLE | ✗ | ✗ | ✗ | 闲 | 闲 |
| WAV 提示音 | 自持播放 | 拒 | 拒 | ✗ | 拒 | 16k/44.1k 按文件 |
| 本地音乐 | ext 流式占用 | ✓ | 拒 | ✗ | 拒 | ext fmt（44100/2/16） |
| 小智对话 | ext 独占租借 | 拒 | 拒 | ✓（会话级） | AFE 全双工 16k | 小智管线自持 |
| 录音/通话 | stream 模式（现状） | 拒 | ✓ | ✗ | record/udp_voice | write_spk（现状） |

### 4.4 集成方式

- **vendor 路线**：从 vocat 仓库拷 `components/esp_xiaozhi/`（0.1.1 in-tree，
  registry 0.1.0 不支持 IDF6）；依赖 `gmf_ai_audio ~0.8` +
  `jason-mao/av_processor ^0.5.5` + esp-sr。
- **音频绑定改造（核心工作量）**：M0 侦察确认 esp_xiaozhi 拿 mic / 写 spk 的
  绑定点（board 层），把它改成调 `ext_claim_exclusive()` 后自建 codec_dev
  （同一 i2s_data_if），release 时归还。vocat 同芯片已验证管线本身，我们只动
  "谁给它 codec"这一层。
- AEC 回采参考：按 vocat 现成接法（M3 验证出声与回声抑制）。

## 4.5 M0 核对清单（2026-09-26，clone vocat 实读；仓库在 `Desktop/esp32/_extern/esp32-s31-vocat`）

| # | 核对项 | 结论 | 出处 |
|---|---|---|---|
| 1 | esp_xiaozhi 0.1.1 是什么 | **纯协议层**（websocket/mqtt/mcp-c-sdk/cjson），**零音频依赖**——AFE/Opus 都不在组件里。vendor 拷入即可，轻 | `components/esp_xiaozhi/idf_component.yml`（依赖仅 ws/mqtt/cjson/mcp） |
| 2 | 音频管线在哪 | **av_processor**（jason-mao）：`audio_manager`（管 play/rec dev）+ `audio_recorder`（AFE+Opus 上行）+ `audio_playback/feeder`（Opus 下行）；AFE 经 `esp_gmf_afe`（gmf_ai_audio 提供） | `main/esp_xiaozhi_chat_app.c:1270-1430` |
| 3 | **音频绑定点** | **两个 `esp_codec_dev` 句柄**：vocat 由 `esp_board_manager_adapter` 产出 `bsp_info.play_dev/rec_dev` → 喂 `audio_manager_config`。移植时不引入 esp_board_manager（省一棵树），**直接喂本分支 audio_service 的 spk/mic_codec_dev** —— ext_claim_exclusive 的天然落点 | `esp_xiaozhi_chat_app.c:1278-1281` |
| 4 | **唤醒开关现成** | `afe_config.ai_mode_wakeup`（vocat=true）。v1 置 **false** ⇒ AFE 不做唤醒 ⇒ **srmodels v1 可不烧**（分区已预留无害），不用在 vendor 管线上动刀 | `:1270-1271` |
| 5 | AEC 回采 | ES8389 以 no_dac_ref=false 打开 → "MR" 布局（ch0=mic、ch1=DAC 内部回采）= AEC reference。**本分支 Korvo-1 BSP 的 I2S 通道布局是否同样出 MR 待 M3 上机确认**（影响 AEC 效果，不阻塞编译） | `:1287-1297` 注释 |
| 6 | esp_board_manager_adapter | 可不引入；待 M1 验证 av_processor 的 audio_manager 不强依赖它 | M1 拉包看 resolution |
| 7 | gmf 版本对齐 | gmf_ai_audio ~0.8 由 av_processor 拉入；与 esp_player 的 esp-gmf 版本冲突与否 = M1 拉包后看 `dependencies.lock` | 桌面方案风险#2 |
| 8 | PA 控制 | vocat 板 NS4150B + GPIO2 active-HIGH 是**它家板级**——本项目 Korvo-1 PA 由 BSP 管，**别抄这段** | `:1332-1340` |
| 9 | 增益时序 | ES8389 in_gain 必须在 open 之后设（is_open 检查）——本分支 audio_service 已符合 | `:1397` |

**M0 结论**：绑定点即"两个 codec 句柄"，比桌面方案预估的侵入度小得多；
无唤醒词 v1 连 srmodels 都不用烧；M1 拉包是唯一的版本冲突裁决点。

## 4.6 M3 代码级落地方案（2026-09-26，基于 vocat 实读 + esp_xiaozhi API 全文）

### 4.6.1 文件清单

```
components/esp_xiaozhi/                  ← vendor 整拷 vocat/components/esp_xiaozhi/（0.1.1）
main/middleware/ai/xiaozhi_service.h/.c  ← 服务壳：状态机 + 仲裁 + av_processor 编排
main/middleware/ai/                       ← 新目录（登记 main/CMakeLists.txt）
main/idf_component.yml                   ← + jason-mao/av_processor ^0.5.5
                                           + espressif/gmf_ai_audio ~0.8（esp-sr 随之拉入）
main/CMakeLists.txt                      ← PRIV_REQUIRES += esp_xiaozhi av_processor
                                           gmf_ai_audio esp-sr（实际名以拉取为准）
main/ui/tab_ai.h/.c                      ← M5
```

**明确不引入**（与桌面方案 §3.2 一致）：esp_board_manager / esp_board_manager_adapter
（绑定点直接喂句柄，省一棵树）、78/xiaozhi-fonts、esp_emote_expression、esp_mmap_assets。

### 4.6.2 xiaozhi_service 接口（仿 record_service/file_mgr 范式）

```c
typedef enum {
    XZ_STATE_IDLE = 0, XZ_STATE_PREPARING, XZ_STATE_CHATTING, XZ_STATE_RELEASING,
} xiaozhi_state_t;
typedef enum {
    XZ_EVT_STATE = 0,      /* 状态机变化（UI 读 volatile 快照） */
    XZ_EVT_TEXT,           /* 对话文本 user/assistant（const char *，回调期有效） */
    XZ_EVT_TTS_STATE,      /* start/stop/sentence_start */
    XZ_EVT_ERROR,          /* esp_err_t + source */
} xiaozhi_evt_id_t;

void      xiaozhi_service_init(void);        /* 建任务/队列，不占音频 */
esp_err_t xiaozhi_service_start(void);       /* 按键进入对话（异步，PREPARING 后事件通知） */
esp_err_t xiaozhi_service_stop(void);        /* 退出对话 */
xiaozhi_state_t xiaozhi_service_get_state(void);
void      xiaozhi_service_set_evt_cb(xiaozhi_evt_cb_t cb);
```

常驻任务 + cmd 队列（START/STOP），事件回调在服务任务上下文（只投数据不碰 LVGL，
P4 纪律）；UI 侧 volatile 快照 + lv_timer 轮询（tab_ai，M5）。

### 4.6.3 状态机与音频接线（核心，全按 vocat 实证路径）

```
IDLE --start()--> PREPARING:
  1. audio_service_ext_claim_exclusive(&spk, &mic)     ← ext 独占租借（§2.3 模式②）
  2. audio_manager_init: play_dev=spk, rec_dev=mic,
     mic_layout="MR"|"MM"（M3.2 裁决）、board 16k/16bit/(MR?2ch:1ch)
  3. esp_codec_dev_open(spk,16k) + open(mic,16k)；set_in_gain 在 open 之后（ES8389 语义）
  4. audio_playback_open（playback DSP 链不移植——vocat 的 DRC/EQ/HOWL 是它家调音）
  5. audio_recorder_open: opus 16k 编码 + afe_config（ai_mode_wakeup=false）
     + recorder_event_cb → esp_xiaozhi_chat_send_audio_data   （上行：:1080 模式）
  6. audio_feeder_open: opus 16k 解码；esp_xiaozhi 的 audio_callback
     → audio_feeder_feed_data(data,len)                        （下行：:803/:941 模式）
  7. esp_xiaozhi_chat_init(ESP_XIAOZHI_CHAT_DEFAULT_CONFIG + 两个回调) → start
     → open_audio_channel（hello: opus/16000/1ch/60ms 默认）
PREPARING --> CHATTING（AUDIO_CHANNEL_OPENED 事件）
CHATTING --stop()--> RELEASING:
  chat_stop → recorder/feeder/playback close → audio_manager_deinit
  → esp_codec_dev_close(spk,mic) → audio_service_ext_release_exclusive() → IDLE
任何态异常 --> RELEASING --> IDLE（ext_release 幂等，audio_service 侧安全）
```

**MP3 播放中按键进入对话**：xiaozhi_service.start 先发 file_mgr STOP，等
PLAY_DONE（ext_release 落地）后再走 PREPARING——链路串行不并发（§2.4）。

### 4.6.4 AEC/麦克风布局：两个预案（M3.2 上机裁决）

Korvo-1 BSP 的 I2S RX 布局决定 AFE 配置（vocat 板 = "MR"：ch0=mic、ch1=DAC 内部
回采，AEC 参考来自 codec 内部 loopback）：

| 预案 | 判据 | AFE 配置 | 代价 |
|---|---|---|---|
| A. MR（照 vocat） | mic dev 能 2ch open 且 ch1 有回采信号 | mic_layout="MR"，AEC 开 | 无（最优） |
| B. MM（降级） | RX 仅 1ch / ch1 无回采 | mic_layout="MM"，AFE 关 AEC | TTS 播放中上行会带设备自身声音 ⇒ 服务端 ASR 自触发；v1 简化对策 = **feeder 活跃期暂停 recorder 上行**（无 barge-in，说完再说）；后续可抄 vocat 的 900ms 静音窗 |

试探方法（M3.2）：mic 2ch open → 录 2s → 播放正弦同时录 → 看 ch1 能量。

### 4.6.5 激活与服务器

esp_xiaozhi 组件内置 xiaozhi.me 协议与激活流程（NVS keystore 存凭证）；transport
MQTT/WebSocket 自动选择（vocat Kconfig 仅此一项，默认 AUTO）。首次激活的验证码：
v1 走串口日志（CHAT_TEXT/activation 事件打印），用户在 xiaozhi.me 输入；配网复用
现有 page_wifi（复用 wifi_manager，不复用 esp_xiaozhi 的 provisioning UI）。

### 4.6.6 内存预算（会话级，[mem] 三元组实测两点：PREPARING 后 / CHATTING 中）

| 项 | 预估 | 说明 |
|---|---|---|
| AFE（MM 单麦无 AEC） | ~30-50KB 内部 RAM | MR+beamformer 更高；audio_recorder_open/close 即 AFE 生命周期 |
| Opus enc+dec | ~30-50KB | 随 av_processor |
| websocket/mqtt+TLS | 已有组件 | udp_voice 验证过栈量级 |
| xiaozhi 协议任务栈 | M3.1 读源码定 | 新任务显式 PSRAM 外置（P2 纪律），先测量再动刀 |

### 4.6.7 M3 拆步（每步可验收）

| 步 | 内容 | 验收 | 决策点 |
|---|---|---|---|
| M3.0 拉包裁决 | yml + reconfigure → dependencies.lock | esp-gmf 版本与 esp_player 对齐；av_processor 未拖入 esp_board_manager | 冲突 ⇒ 调 esp_player 或 vendor 隔离 |
| M3.1 vendor+空编译 | esp_xiaozhi 拷入 + xiaozhi_service 空壳（init/任务/队列，无音频） | build 过；bin/ram 增量记录 | bin 超 7MB−200KB ⇒ 停下重议 |
| M3.2 绑定点试探 | ext_claim_exclusive → 2ch RX / 回采探测 | MR/MM 裁决（4.6.4 方法） | MM ⇒ 预案 B |
| M3.3 对话 MVP | 全管线 + xiaozhi.me | 真机完整对话一轮；对话期 MP3/WAV/录音全拒且退出恢复 | — |
| M3.4 激活+UI | 激活流程 + tab_ai（页内按钮范式） | 激活成功；UI 无阻塞无栈告警 | — |

## 5. 互斥矩阵（全家族）

| ↓申请 \ 已占用→ | WAV 播放 | MP3 | AVI 播放 | 录音/通话 | 小智对话 | 取景器/预览 |
|---|---|---|---|---|---|---|
| WAV 播放 | 拒 | 拒 | ✓（无共享） | 拒 | 拒 | ✓ |
| MP3 | 拒 | 拒 | ✓ | 拒 | 拒 | ✓ |
| AVI 播放 | ✓ | ✓ | 拒（file_mgr s_run） | ✓ | ✓ | 拒（staging 借用） |
| 录音/通话 | 拒 | 拒 | ✓ | 拒 | 拒 | ✓ |
| 小智对话 | 拒 | 拒 | ✓ | 拒 | 拒 | ✓ |
| 取景器 | ✓ | ✓ | 拒 | ✓ | ✓ | 拒 |

原则：**音频域一个闸门（audio_service ext/stream 状态）**；视觉域维持现有
守卫（viewfinder staging 借用 / file_mgr 单状态机）。听歌可以看照片、
放视频可以录音——只有 codec 和 staging 两类资源真的互斥。

## 6. 里程碑（每步可验收，MP3 先行）

| 里程碑 | 内容 | 验收 | 决策点 |
|---|---|---|---|
| **M0 侦察** | vendor esp_xiaozhi 拷入 + 依赖树 dry-run（reconfigure 拉包不 build）；读 vocat 音频绑定点；**esp-sr on IDF6.1+s31 编译试探** | 绑定点笔记 + 组件版本矩阵（esp_player 的 esp-gmf 与 gmf_ai_audio 的版本必须对齐） | esp-sr 编译不过 ⇒ 小智降"自写 WS 流式 ASR/TTS"另议；MP3 不受影响 |
| **M1 分区+ext 协议** | 新 CSV + ext 两种模式实现 + 全量 build + 烧录流程走通 | [mem] 各 stage 水位记录；WAV/录音/udp_voice 回归不变；bin 涨幅记录 | — |
| **M2 MP3**（S4b 收编） | media_player_service + FILES 路由 + UI | FILES 点 mp3 出声；互斥矩阵生效；录音中拒 MP3 且有提示 | bin 超预算 ⇒ 降 S4b-B 或再议 |
| **M3 小智编译** | esp_xiaozhi 链接 + srmodels 烧录 + 绑定点接 ext 独占租借 | 空对话 pipeline 编译链接过；bin ≤ 7MB − 200KB | — |
| **M4 对话 MVP** | 按键触发会话（无唤醒词）→ AFE → 流式 ASR/LLM/TTS → 出声 | 与小智服务器完整对话一轮；对话期 MP3/WAV/录音全拒且退出后恢复 | — |
| **M5 UI+收尾** | tab_ai 页（页内按钮范式、非 tab 切换触发）+ 文档 + 记忆 | — | — |

## 7. 风险与对策

| 风险 | 对策 |
|---|---|
| ~~esp-sr 不支持 IDF 6.1 / s31~~ **已降级**：vocat（同芯片 IDF6.x）REQUIRES espressif/esp-sr 并实机运行 ⇒ 兼容性有活样例 | M3.0 拉包后编译验证即可 |
| esp_player 与 gmf_ai_audio 的 esp-gmf 版本冲突 | M0 产出版本矩阵，统一 pin；冲突时以 gmf_ai_audio 为准调 esp_player 版本 |
| registry 拉包 403（esp-gmf 前科） | 离线 clone 进 components/（main yml 注释有先例） |
| 内部 RAM：AFE 会话期 50~80 KB 叠加 WiFi 51.2KB DMA | 会话级 create/destroy；对话期与通话互斥本来就是设计；M1/M4 两次 [mem] 实测 |
| 放弃双 OTA | 流程已写明（USB 全量）；用户已知情 |
| ext 协议触碰现有路径引发回归 | M1 回归清单（WAV/录音/udp_voice/音量）+ 行为零变化原则 |
| srmodels 许可 | 个人开发可用、不可二次分发（桌面方案 §8） |

## 8. 拍板记录（2026-09-26 已拍板）

1. ✅ **MP3 与小智合并立项**（共享 GMF 树，边际 +100~200KB）
2. ✅ **唤醒词 v1 放弃，改按键触发对话**（AFE 会话级 create/destroy）
3. ✅ **分区表单 app 7MB 布局 + 放弃双 OTA**（4.1 的 CSV；srmodels 3MB 维持预留，
   详见 4.1"为什么不能塞 littlefs"与"v1 机会点"）
4. ✅ **顺序：M2 先 MP3、后 M3+ 小智**
5. ✅ **MP3 v1 只开 `.mp3`**，`.m4a/.flac` 显示图标不播

## 9. 落地结果：与本文方案的偏差清单（2026-09-27 验收通过）

本文写于 2026-09-26（设计稿）。最终实现整体沿本文架构（ext 独占租借、会话级
AFE、单 app 7MB 分区、MP3 先行），以下为**实际落地与本文的偏差**，以
`改动记录.md` §42→§51 为准：

| # | 本文方案 | 实际落地 | 原因/出处 |
|---|---|---|---|
| 1 | §4.6.3 会话序列止于 `open_audio_channel`（hello） | 建链后须 **`send_wake_word("你好小智")`** 作对话回合扳机；listen 开票/收票由 **PTT 按钮显式控制（MANUAL）** | 服务器 v1 协议：无 wake detect 不开 dialog；realtime 自动回合对端不可控 → §46→§49 三轮演进定案 |
| 2 | listen 模式未定（§46 试 AUTO、§47 试 REALTIME） | **`LISTENING_MODE_MANUAL`**，按住说话/松开收票 | re-listen 自动重开票只有紧跟 tts stop 一轮可靠（§48 实测废弃） |
| 3 | §4.6.4 预案 A(MR+AEC)/B(MM+关AEC) 待裁决 | **预案 B 定案（§53）**：MM+1ch、AFE 关 AEC——MR 实测否决（参考通道错配把人声当回声消掉 = 每轮只转写前 ~5 字）+ TTS 闸门兜自声；打断走 abort_speaking（§52）+ 8s 停摆自愈 |
| 4 | 未涉及对话中文显示 | 对话记录用 **ui_binfont 全 CJK bin 字体**（littlefs→PSRAM，两段式加载），按钮/状态行英文 | 开放文本必缺字，cn_20 死字表不够（§50） |
| 5 | §1 架构表含 record_service / udp_voice | **§37 已阉割**：本分支这两者及其 UI 已删，ext 客户端只剩 media_player（流式）与小智（独占租借） | 固件体积让位核心功能 |
| 6 | §4.6.5 transport 自动选择 | 实走 **WebSocket 通道**（OTA 响应无 mqtt 节 → `use_mqtt=false`） | v1 账号无 MQTT 配置；MQTT+UDP 差异见 §46/会话总结 |
| 7 | §4.1 v2 分区表 srmodels 3MB 预留 | 实际定稿 **v1.1**（同 v1，仅 srmodels→model 改名 912KB，nvs 64KB） | M0 后重算，见执行状态头 |
| 8 | —（新增） | esp_xiaozhi **MCP 引擎必填**（`chat_start` 拒启坑）+ **WS start 异步竞态补丁**（§44/§45） | vendor 组件两处本地补丁，vendored 副本允许 |

**验收判据（已全部达成）**：多轮 PTT 对话（`ptt START/STOP: ESP_OK` +
`uplink sent` 增长 + `chat text role=0`）；对话中文全显示；对话期 MP3/录音
互斥且退出恢复；良性告警清单见 §51.3。
