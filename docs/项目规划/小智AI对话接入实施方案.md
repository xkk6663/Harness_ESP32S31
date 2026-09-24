# 小智 AI 对话接入实施方案

> 状态：**未开工**（方案已定，等排期；2026-09-24 复核 —— `main/` 内无任何 xiaozhi 相关代码）
> 日期：2026-09-23
> 参考仓库：https://github.com/han0519/esp32-s31-vocat（同芯片同 codec 活样例）、https://github.com/78/xiaozhi-esp32（官方完整应用，不直接搬）
> 关联文档：`docs/工程规范/13_内存管理规范.md`、`docs/工程规范/11_遇到的问题.md`（F35/F37）、`docs/项目规划/改动记录.md` §28.31

---

## 0. 结论摘要

- **可行**，走 `esp_xiaozhi` 组件路线（vocat 已在 S31 + IDF 6.x 上验证），不搬 `78/xiaozhi-esp32` 官方 app。
- **分区表必须重构**：当前 app 固件 0x4e09a0（≈4.88MB），ota_0 的 5MB 只剩 **2% 余量**；加 esp_xiaozhi + esp-sr + Opus 必然放不下。方案：放弃双 OTA，改单 app 7MB + 新增 srmodels 3MB。
- 分 5 个里程碑（M0 侦察 → M1 分区+编译 → M2 唤醒词 → M3 对话 MVP → M4 UI+点歌），每阶段有独立验收标准，可随时停在任一阶段不破坏现有功能。
- 交互：**mode 键 / 页内按钮 / 唤醒词「你好小智」**三入口进对话；三态模式仲裁（BT 音乐 / 本地在线音乐 / 小智对话）互斥。

---

## 1. 需求

| 项 | 内容 |
|---|---|
| 触发 | mode 键启动 / 唤醒词「你好小智」离线唤醒 |
| 对话 | xiaozhi.me 流式 ASR + LLM + TTS，Opus 16k |
| 唤醒词 | ESP-SR 离线 WakeNet，模型放 srmodels 分区 |
| 范围外 | 摄像头（不做）；WiFi 配网（复用现有 wifi_manager）；「你好喵伴」定制唤醒词（后议） |

---

## 2. 现状盘点（写方案前实测/实读，均给出处）

| 事实 | 出处 | 对本方案的影响 |
|---|---|---|
| app 固件 0x4e09a0 / ota_0 0x500000，剩 0x1f660（2%） | `build/log/idf_py_stdout_output_11636` | **分区表必须重构**（见 §4） |
| 16MB flash 恰好用满：nvs/phy/otadata + ota_0 5MB + ota_1 5MB + storage littlefs 0xA20000/0x5E0000 | `partitions.csv` | 无空闲分区，srmodels 只能挤占 OTA 或 storage |
| 麦克风 I2S RX 链路**已通**（record_service 支持 STREAM/LOCAL 录音） | `main/middleware/audio/record_service.{c,h}` | AFE 的 mic 输入有现成底层可参照，但**与录音互斥**，需进仲裁 |
| WiFi STA + 事件双槽位（sys_cb 进程级 / evt_cb 页面级），GOT_IP 后拉后台服务是既有模式 | `main/middleware/network/wifi_manager.h:84-98` | xiaozhi 连接服务挂在 sys_cb 的 GOT_IP 事件上 |
| 项目已有 GMF 音频管线（gmf_core/gmf_audio/gmf_io）与 GMF task 栈 PSRAM 外置经验（P1/P2/P3/P4 纪律） | `main/middleware/bluetooth/bt_sink_service.h:10-18` | esp_xiaozhi 依赖 `gmf_ai_audio ~0.8`，需核对与现有 gmf_* 版本兼容 |
| 内部 RAM 紧张：RGB bounce buffer 已降 HEIGHT=10；wifi 重试每次吃 10KB 不归还（F35） | `wifi_manager.h:32-42` | esp-sr AFE 常驻 ~50-80KB 内部 RAM 必须重新算账并实测 |
| taskLVGL 栈 16384（§28.31），水位 9128B 余量（收流场景） | `改动记录.md` §28.31 | AI 页整页重建峰值需重新测水位 |
| LV_COLOR_DEPTH=24、帧缓冲 RGB565、`LV_USE_FS_MEMFS=y`（bin 字体依赖） | `sdkconfig:5437/6111` | 字体链路不动 |
| UI 约定：模式切换只能页内按钮，tab 只做空态占位；回调不碰 LVGL 只置标志 | `tab_btsink.c:9-18`、`page_main.c:86-90` | tab_ai 照此纪律写 |

---

## 3. 技术路线选型

### 3.1 为什么不搬 `78/xiaozhi-esp32`

- 它是**完整应用**（自带 boards/ 板级抽象、自有 UI/配网/OTA），搬进来等于重写本项目；
- 板级驱动与本项目 BSP（`espressif__esp32_s31_korvo_1`）冲突；
- 它的协议层已被抽成官方组件 `esp_xiaozhi`，直接用组件即可。

### 3.2 采用：esp_xiaozhi in-tree 0.1.1（vendor）

vocat 的 `main/idf_component.yml` 原文（关键依据）：

```yaml
dependencies:
  jason-mao/av_processor: ^0.5.5
  espressif/gmf_ai_audio: ~0.8
  78/xiaozhi-fonts: ~1.3.2        # 本项目有自己的 bin 字体，可不引入
  espressif2022/esp_emote_expression: ^1.0.2   # 表情引擎，本项目不需要
  espressif/esp_mmap_assets: ^1.3.2            # 同上，不需要
  # Registry 0.1.0 only supports IDF <6; use in-tree 0.1.1 for IDF 6.x until published.
  espressif/esp_xiaozhi:
    path: ../components/esp_xiaozhi
  idf:
    version: '>=5.5.0'
```

**本项目的依赖决策**：

| 组件 | 引入方式 | 说明 |
|---|---|---|
| `esp_xiaozhi` | **vendor**：从 vocat 仓库拷 `components/esp_xiaozhi/` 到本项目 `components/` | registry 版不支持 IDF6，in-tree 0.1.1 是唯一已验证 IDF6 路径 |
| `esp-sr` | 随 esp_xiaozhi/gmf_ai_audio 依赖拉入（版本 M0 确认） | WakeNet/AFE；需确认对 IDF v6.1 的支持 |
| `gmf_ai_audio ~0.8` | registry 拉取 | 含 AI 音频管线（Opus/AEC 编排）；**与现有 gmf_* 版本冲突待 M0 核对** |
| `jason-mao/av_processor ^0.5.5` | registry 拉取 | vocat 音频处理中间层 |
| `78/xiaozhi-fonts`、`esp_emote_expression`、`esp_mmap_assets` | **不引入** | 本项目有自己的 CJK bin 字体与 LVGL UI；表情引擎不需要 |

---

## 4. 分区表重构（M1 核心，破坏性变更）

### 4.1 新旧对照

```csv
# ---- 现状（16MB 用满，app 只剩 2%）----
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
otadata,  data, ota,     0x11000,  0x2000,
ota_0,    app,  ota_0,   0x20000,  0x500000,
ota_1,    app,  ota_1,   0x520000, 0x500000,
storage,  data, littlefs, 0xA20000, 0x5E0000,

# ---- 方案：单 app 7MB + srmodels 3MB + storage 6MB ----
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
otadata,  data, ota,     0x11000,  0x2000,
app,      app,  factory, 0x20000,  0x6E0000,
srmodels, data, spiffs,  0x700000, 0x300000,
storage,  data, littlefs,0xA00000, 0x600000,
```

- 放弃 OTA A/B：本项目为 USB 烧录开发机，`otadata` 保留无害（无 ota_0/1 时 bootloader 走 factory）。**代价声明：失去 A/B 回滚能力**。
- srmodels 预留 3MB：官方 srmodels.bin 全量 ~4MB，但可按需裁剪（只留 wn9_xiaozhi 等所选模型后 ~1.5-2MB）；M0 实测 vocat 的 srmodels.bin 大小后如需调整在此窗口内改。
- **storage 首地址 0xA20000 → 0xA00000**：littlefs 内容（字库 lv_font_cjk_28.bin、壁纸）必须**重烧**，且 `esptool` 地址参数跟着变（烧录命令见 4.3）。

### 4.2 烧录影响清单（一次性）

1. `idf.py erase-flash`（分区表大改，建议全擦）→ `idf.py -p COM9 flash`；
2. **重烧 storage**：`esptool --chip esp32s31 -p COM9 write-flash 0xA00000 build/storage.bin`（★ 地址在前文件在后；新地址，别再用 0xA20000）；
3. 重烧 srmodels（esp-sr 构建产物，见 M2）。

### 4.3 验收

- 烧录后：字库正常显示中文（bin 字体从新偏移加载成功）、壁纸正常、WiFi/蓝牙/SINK 全部回归通过；
- `idf.py partition-table` 显示新表；app 7MB 余量 >40%。

---

## 5. 软件模块设计

### 5.1 新增文件

```
main/middleware/ai/xiaozhi_service.h   服务接口（仿 bt_sink_service 风格）
main/middleware/ai/xiaozhi_service.c   生命周期/状态机/事件上抛
main/ui/tabs/tab_ai.{c,h}              AI 对话页（页内按钮 + 空态占位）
components/esp_xiaozhi/                vendor 自 vocat（in-tree 0.1.1）
```

**★ 全部 .c 必须登记 `main/CMakeLists.txt` SRCS**（规范 03/10；CMake 需同步 `PRIV_REQUIRES` 增加 esp_xiaozhi / esp-sr / gmf_ai_audio 等）。

### 5.2 xiaozhi_service 状态机

```
IDLE --(mode键/页内按钮/唤醒词)--> PREPARING（暂停 BT/录音，起 AFE+连接）
PREPARING --> CHATTING（xiaozhi.me 会话中）
CHATTING --(退出/超时)--> RELEASING（释放 AFE/连接，恢复原模式）
任何态异常 --> RELEASING --> IDLE
```

- 线程纪律（沿用 P4 铁律）：服务内部回调（AFE/WakeNet/协议）**绝不碰 LVGL**，只更新 volatile 快照 + 投递队列；UI 用 lv_timer 轮询——与 `bt_sink_service` 完全同构。
- 唤醒词检测**常驻**（AFE 常开，mic 通路独占）；与 `record_service` 录音互斥（同一 I2S RX），仲裁表见 §5.3。

### 5.3 三态模式仲裁

| 活跃模式 | A2DP Sink | 本地/在线播放 | 录音 record | AFE 唤醒词 | esp_xiaozhi |
|---|---|---|---|---|---|
| BT 音乐 | ✔ | ✘ | ✘ | ✔（AEC 回采参考接 I2S 输出） | 待命 |
| 本地/在线音乐 | 挂起 | ✔ | ✘ | ✔（同上） | 待命 |
| 小智对话 | **挂起**（释放栈/池压力） | ✘ | ✘（互斥） | ✔ | ✔ |

- 仲裁入口：**页内按钮 + mode 键**（`button_service` 已有），唤醒词是"从任意态进对话"的全局旁路；
- 挂起 A2DP 用 `bt_sink_service_request_stop()`（建一次不毁，恢复时重新 STARTED）；
- 唤醒词常驻的开销进 M2 内存实测账；若内部 RAM 撑不住，降级方案为"仅对话态开 AFE、平时 mode 键进入"（体验降级但保命）。

### 5.4 UI：tab_ai

- 空态：门禁卡片（未连 xiaozhi.me / 未登录激活 → 提示 + 页内按钮"进入对话"），照 `tab_btsink` 的 gate 卡模式；
- 对话态：状态点 + 波形/呼吸动画（**按需起停**，沿用 EQ 动画的 eq_set_running 模式）、当前对话字幕（bin 字体，三行窗口可复用 `tab_btsink.c` 的歌词窗实现）；
- 主题令牌只用 `ui_theme.h`，禁止硬编码颜色。

### 5.5 语音点歌（M4）

- xiaozhi 协议支持 **MCP 工具**：设备端注册 `search_and_play(query)`；
- 回调里 HTTPS 请求公共音乐搜索 API（vocat `page_music.c` 的 `k_music_apis` 数组：uomg / vvhan / codelife / Meting 多端点兜底，可整段参考）→ cJSON 解析出音频流 URL → 切回本地/在线音乐模式播放；
- 播放器复用现有 GMF 管线或 `esp_audio_simple_player`（M4 定）。

---

## 6. 内存预算与测量计划（沿用 13 号文档方法论）

| 项 | 预估 | 来源/措施 |
|---|---|---|
| esp-sr AFE（AEC+NS+VAD+WakeNet，双麦） | 50~80KB 内部 RAM | M2 实测 `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` 前后差 |
| Opus 编解码缓冲 | ~30-50KB | 随 gmf_ai_audio，M3 实测 |
| WiFi+mbedTLS 已有账（F35：单次连接尝试 ~10KB） | 已在预算内 | 保持 WIFI_MAX_RETRY=3 不动 |
| esp_xiaozhi 协议任务栈 | M0 读源码定 | 栈显式 PSRAM 外置（P2 纪律） |
| 所有新建任务 | — | **先测量再动刀**：每个新任务建 HWM 水位日志，7 天内不移除 |

---

## 7. 里程碑与验收

| 里程碑 | 内容 | 验收标准 |
|---|---|---|
| **M0 侦察**（纯调研，不改代码） | clone vocat 到本地；摸清：esp_xiaozhi 0.1.1 API、esp-sr 版本与 IDF6.1 兼容性、srmodels.bin 实际大小与内容、mic 初始化路径、gmf_ai_audio 与现有 gmf_* 版本关系 | 产出《M0 核对清单》，§8 待确认项全部有答案 |
| **M1 分区+编译** | partitions.csv 重构、vendor 组件、idf_component.yml、xiaozhi_service 空壳登记 | 全擦全烧后现有功能回归通过（字库/壁纸/SINK/配网）；build 过 |
| **M2 唤醒词** | esp-sr AFE 常驻 + srmodels 烧录 + mic 独占通路；唤醒触发 LED/日志 | 说「你好小智」触发唤醒日志；内部 RAM 前后差入账；BT 音乐同时播放不崩 |
| **M3 对话 MVP** | esp_xiaozhi 接 xiaozhi.me（激活流程）、TTS 出声、mode 键进入退出、BT 挂起恢复 | 真机完成一轮语音对话；对话中 SINK 已挂起；退出后 BT 恢复 |
| **M4 UI+点歌** | tab_ai 完整 UI、三态仲裁收口、MCP search_and_play + 在线播放 | 语音说「播放 XX」能搜到并播放；UI 无阻塞、无栈告警 |

---

## 8. 风险与待确认清单

| # | 风险/待确认 | 影响 | 对策 |
|---|---|---|---|
| 1 | esp-sr 对 IDF v6.1.0 的兼容版本 | M1 可能编译不过 | M0 从 vocat 拿到确切版本号；必要时 esp-sr 也 vendor |
| 2 | gmf_ai_audio ~0.8 与现有 gmf_core/gmf_audio 版本冲突 | 依赖树打架 | M0 对照两边 idf_component.yml；必要时锁旧版或 vendor 隔离 |
| 3 | Korvo-1 麦克风数量（单/双麦）与 AFE 配置 | AFE 内存与算法效果 | M0 读 BSP 板级定义确认；单麦则关 AEC 阵列相关开关 |
| 4 | vocat srmodels.bin 的模型许可（含"你好喵伴"定制模型） | 只取"你好小智"官方模型可规避 | M2 只烧"你好小智"；喵伴后议 |
| 5 | 内部 RAM 放不下常驻 AFE | 唤醒词体验降级 | 降级为"仅对话态开 AFE"；bounce buffer HEIGHT 10→20 换回 31.25KB 的后手 |
| 6 | 放弃双 OTA | 无法 A/B 回滚 | 开发机可接受；发布前再评估 |
| 7 | storage 搬址后遗漏重烧 | 字体/壁纸乱码 | 4.2 烧录清单写死流程；烧录脚本化 |

---

## 9. 明确不做

- 摄像头 / 表情引擎 / vocat 圆屏相关；
- 「你好喵伴」唤醒词（第一版）；
- 小智官方 app 的配网/激活 UI 全套（复用本项目 page_wifi，激活走日志二维码或串口）。
