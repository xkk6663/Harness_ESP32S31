# S4b FILES 页 MP3 本地播放 · 可执行方案 v1

> 2026-09-26 · 供审阅。目标：FILES tab 点 `.mp3` → 扬声器播放（复用 main 主线已验证的
> MP3 逻辑）。**照片/AVI 链路零改动**。

---

## 0. 调研结论（决定方案形态的三个事实）

| # | 事实 | 来源 |
|---|---|---|
| 1 | main 的 MP3 本地播放 = `esp_player + esp_audio_render + esp_gmf + esp_extractor/esp_audio_dec` 全家桶，输出走 `bsp_audio_codec_speaker_init()` **另开的 codec 句柄**（44100/2/16 固定） | `main:main/ui/tabs/tab_audio.c`（AUDIO HUB，LOCAL 模式） |
| 2 | 本分支 **没有任何 GMF 栈组件**（components/ 仅 cjson+esp_littlefs），但 main 在**同芯片同板**上已验证该组件树可用 ⇒ 走组件注册表拉取可行（`idf_component.yml` 加 `espressif/esp_player ^1.0.6`） | 两分支 idf_component.yml / managed_components 对比 |
| 3 | 本分支 `audio_service` 已持有 `spk_codec_dev`（ES8389），WAV 播放按文件头 `esp_codec_dev_open` + 逐块 write；**main 再 init 一个 speaker 句柄 = 同一 I2S 双句柄** —— 正是挂账的 P0（esp_codec_dev 多组件共用 i2s_data_if 资源竞争） | `audio_service.c:29/260-281` vs `tab_audio.c init_codec()` |

另：注册表**没有**独立的 esp_new_mp3 组件（已查，ComponentNotFoundError）；MP3 解码器
在组件树里由 `esp_audio_codec`（esp_audio_dec 的底层库）提供。

## 1. 两条路线

### 路线 A（推荐）：移植 main 的 esp_player 链路，输出统一进 audio_service

把 main `tab_audio.c` 的 LOCAL 链路原样搬进新服务任务，**唯一实质改动**：
`out_writer` 不再自建 codec 句柄，改为调 `audio_service` 新暴露的 raw-PCM 接口
（同一 `spk_codec_dev`，44100/2/16 open → write → close）⇒ **顺手把 P0 双句柄冲突
按设计消灭**，而不是留个雷。

- 白赚：AAC/FLAC/M4A 等解码器同栈可用（以后 FILES 里点别的音频也能放）。
- 代价：依赖树大（esp_player→render/extractor/audio_dec/audio_codec/media_lib/gmf），
  flash 预估 **+400~600 KB**（槽余 ~950 KB）；首次解依赖需联网拉 7~10 个组件
  （esp-gmf 曾 403 过一次，必要时离线 clone 进 components/ —— main 的 yml 注释里有先例）。

### 路线 B（备选）：audio_service 内嵌 MP3 软解（esp_audio_codec 单组件）

直接依赖 `espressif/esp_audio_codec`，在 audio_service 播放任务里加 `.mp3` 分支：
ID3 跳过 → 帧同步 → 逐帧解 PCM → 走现有 WAV 的 write 通路。
- 依赖最小（+150~250 KB flash），单句柄天然无 P0。
- 风险：esp_audio_codec 对 **esp32s31** 的预编译库支持未验证（esp_player 全家在 main
  已验证含 s31，单拆反而没验证过）；ID3/VBR/帧同步解析细节全要自己写。

**建议 A，并内置降级闸门**：M1 先只加依赖做"空集成 build"，量出真实 bin/ram 增量——
超预算或拉不到组件 ⇒ 自动降 B（决策点，见 §4）。

## 2. 路线 A 的模块设计

```
main/idf_component.yml                     + espressif/esp_player ^1.0.6
main/middleware/audio/media_player_service.{c,h}   ← main tab_audio LOCAL 链路搬移
main/middleware/audio/audio_service.{c,h}          ← +raw-PCM 对外接口（供 render 写）
main/middleware/media/file_mgr_service.c           ← PLAY 按 .avi/.mp3 分路由
main/ui/tab_files.c                                ← MP3 态 UI（无进度条，v1）
```

### 2.1 media_player_service（新，~200 行）

从 `main:main/ui/tabs/tab_audio.c` 搬移并裁剪：

- 保留：`register_media_defaults()`、`init_gmf_pool()`（ch/bit/rate_cvt + alc 四元素）、
  `esp_audio_render_create()`、`esp_player_init()`、播放器事件回调（只置标志）。
- **裁掉**：BT PUSH 模式、曲目列表（列表 UI 归 FILES）、`bsp_audio_codec_speaker_init()`。
- **改**：`audio_out_write(ctx)` → 调 `audio_service_ext_write()`；生命周期挂到
  audio_service 的"外部占用"状态上（claim/release，见下）。
- 服务任务 + cmd 队列 + 事件回调（同 file_mgr/page_wav_player 范式）：
  PLAY(path) / STOP(/PAUSE)，evt：PLAY_STARTED/PLAY_DONE/Error。

### 2.2 audio_service 扩展（~40 行）：外部占用协议

```c
/* MP3/GMF 播放期间独占 spk：返回 ESP_OK = 拿到（本服务保持 IDLE 且不开 dev）；
 * 期间调用方以 44100/2/16 open→write→close，SET_VOLUME 照常生效（同句柄）。
 * 任何 WAV 播放/录音请求在外部占用期间被拒（事件回 BUSY）。 */
esp_err_t audio_service_ext_claim(void);
void      audio_service_ext_release(void);
esp_err_t audio_service_ext_write(const void *pcm, size_t bytes);  /* 内部 open/close */
```

实现要点：`s_ext_busy` 标志 + 现有 cmd 处理入口统一判它；mic 录音同拒（I2S 全双工共享）。

### 2.3 FILES 路由与 UI（~60 行改动）

- `file_mgr_service::do_play()`：按扩展名分流——`.avi` 走现有视频链；`.mp3/.m4a/.flac`
  → `media_player_service_post(PLAY)`（不再走 viewfinder/守卫那套视觉管线，
  但共享"一次只播一个"状态机：MP3 与 AVI 互斥由 file_mgr 的 `s_run` 保证）。
- 守卫扩展：MP3 播放要求 audio_service 空闲（ext_claim 成功）；**录音中拒绝**；
  不要求相机/取景器（音频与视觉管线无共享资源）⇒ 听歌时可以同时看照片预览。
- UI：MP3 态隐藏进度条（esp_player 的位置查询 v1 不接），状态行 `Playing MP3 (no audio bar)`；
  tap 画布退出 = STOP（与 AVI 一致）；文件列表图标 `LV_SYMBOL_AUDIO`。

### 2.4 互斥矩阵（增量）

| 场景 | 规则 |
|---|---|
| MP3 ↔ WAV 播放/录音 | `audio_service` ext_claim 互斥（同一 codec 句柄） |
| MP3 ↔ AVI 视频 | file_mgr `s_run` 单状态机互斥（同一次 PLAY 只走一条链） |
| MP3 ↔ 照片预览 | **允许并行**（无共享资源；解码 CPU 充裕：MP3 软解 ~20 MHz 级） |
| MP3 ↔ 蓝牙/小智 | 本分支无 bt；将来合并 main 时在 audio_service 层仲裁（P0 的正解） |

## 3. 里程碑（每步可验收）

| 里程碑 | 内容 | 验收 |
|---|---|---|
| **M1 依赖试探（决策点）** | yml + esp_player 空集成（init 但不接 UI），全量 build | 记录 bin/ram 增量；bin 涨后仍 ≤ 6 MB 槽 −200 KB 余量 ⇒ 继续 A；否则降 B |
| **M2 服务搬移** | media_player_service + audio_service ext 协议 | 用一条测试命令播 SD 上的 MP3（日志验证 PCM 在写、无双句柄告警） |
| **M3 FILES 接线** | 路由 + UI + 互斥扩展 | FILES 点 MP3 出声；STOP/退出即停；WAV 播放页不受影响；录音在 MP3 期间被拒且提示 |
| **M4 收尾** | 文档 §34 + 记忆 + 验收清单 | — |

## 4. 待拍板（默认值即推荐项）

1. **路线 A（esp_player 全家 + 输出统一，M1 闸门可降 B）** —— 还是直接选 B？
2. MP3 播放 v1 **不做进度条/暂停**（只有 出声/停止）—— 接受？
3. 支持格式：v1 只对 `.mp3` 分路由，`.m4a/.flac` 图标显示但不播 —— 还是顺手全开（A 路线零成本）？

## 5. 风险与对策

| 风险 | 对策 |
|---|---|
| 组件树 flash 增量超预算 | M1 闸门：空集成 build 量准了再动 M2 |
| registry 拉取 403（esp-gmf 前科） | 离线 clone 进 `components/`（main 的 yml 注释有先例与做法） |
| 双句柄 I2S 冲突（P0） | 本方案用"输出统一到 audio_service"从设计上消灭，而不是运行时规避 |
| GMF 栈与 esp-dl/LVGL 的 RAM 峰值叠加 | main 同板已验证共存；M1 顺带看 `[mem]` 各 stage 水位 |
