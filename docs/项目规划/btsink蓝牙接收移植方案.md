# btsink 蓝牙接收（A2DP Sink）移植可执行方案

> 状态：**待审阅**（本文只做方案，未改动任何代码）
> 参考仓库：https://github.com/han0519/esp32-xiaofeng
> 目标项目：`C:\Users\xiao1\Desktop\esp32\display_audio_photo`
> 撰写日期：2026-09-22

> ## ⚠ 2026-09-22 落地现状（读本文前请先看这段）
>
> 本文是**当初的设计稿**，其中**「专辑封面（JPEG）」这条需求已作废**。
> 实测与外部调研（见 `改动记录.md` §28.28）确认：安卓侧的 AVRCP Cover Art
> 默认关闭（默认 AVRCP 1.5 + OEM overlay 常为 false + App 未必交图），
> 对端从不推送 ⇒ 设备端收不到任何图。
>
> 因此随后在 `§28.29` 里：
> - **服务侧封面链路整条删除**（省 ≈337KB PSRAM + 一个解码任务 + 一个队列）；
> - **UI 侧改为**：曲名/歌手居中 + **三行歌词窗口（逐句上滑、渐入渐出）** + 圆形控制键 + 均衡器条；
> - 歌词来自对端把**歌词行覆盖写进 AVRCP TITLE(0x01)**，由
>   `bt_sink_service` 的 `burst_resolve()` 把「真曲名」和「歌词行」拆开
>   （新接口 `bt_sink_service_get_lyric()`）。
>
> 所以：**下面凡是提到"封面/COVER_ART/cover 解码任务"的段落都只作历史参考**，
> 不要再照它实现。当前真实状态以 `改动记录.md` §28.27 ~ §28.29 为准。

---

## 0. 一句话目标

让本机从「**A2DP Source**（推流到耳机）」扩展出「**A2DP Sink**（接收手机/电脑的音频）」，
新增一个 `btsink` 标签页复现参考仓库的音乐界面：
**接收音频流 + AVRCP 播放控制 + 歌曲元数据（标题/艺术家/专辑/流派）+ 专辑封面（JPEG）**，
并把参考仓库的**中文取模预渲染字体**改为**二进制字体文件放进本机 LittleFS**。

---

## 1. 调研结论

### 1.1 参考仓库画像（esp32-xiaofeng）

| 维度 | 事实 |
|---|---|
| 硬件 | **与本机完全同款**：ESP32-S31 Korvo1、800×480 RGB、GT1151 触摸 |
| 框架 | ESP-GMF + Espressif 官方 `esp_bt_audio` 组件 |
| 代码位置 | 主程序在 `components/esp_bt_audio/examples/bt_audio/`（官方 example 的二次开发）|
| 关键文件 | `main/main.c`(34 KB 事件调度) · `main/bt_ui/bt_ui.c`(50 KB UI) · `main/stream_proc.c`(29 KB 管线) · `main/bt_ui/fonts/lv_font_notosanssc_regular_28.c`(**13.3 MB 中文字体**) |
| 蓝牙角色 | `A2DP_SNK` + `AVRC_CT` + `AVRC_TG`（Sink 场景还默认开 `HFP_HF` 通话）|
| A2DP 角色选择 | **Kconfig `choice` 二选一**（`GMF_EXAMPLE_A2DP_SINK` / `_SOURCE`）→ 编译期定死 |
| 分区 | 无 LittleFS；`factory` 8 MB，字体**编译进固件** |
| 色深 | `LV_COLOR_DEPTH=16`（RGB565）|
| 封面解码 | `esp_jpeg` → RGB565 → `lv_image_dsc_t` |
| 字体 | NotoSansSC 28px，**bpp=1（单色）**，范围含 **全 CJK `0x4E00-0x9FFF`（20992 字）** |

### 1.2 本项目现状（display_audio_photo）

| 维度 | 事实 |
|---|---|
| 硬件/框架 | 同款硬件；ESP-IDF **v6.1.0**；LVGL **v9.6.0** |
| 蓝牙组件 | `managed_components/espressif__esp_bt_audio`（与参考**同源**）|
| 已注册角色 | `A2DP_SRC + AVRC_CT`（**Source 模式**）|
| Sink 骨架 | **已有**：`bt_role_t{BT_ROLE_SINK}`、事件 `BT_EVT_MEDIA_CMD` / `BT_EVT_VOL_ABSOLUTE` 已定义并转发 |
| Sink 缺口 | 角色未注册、无 `bt2codec` 管线、无元数据/封面处理、无 UI |
| 文件系统 | `esp_littlefs`，挂载点 **`/littlefs`**，分区 `storage` = **0x5E0000 ≈ 5.875 MB** |
| 镜像生成 | `littlefs_create_partition_image(storage littlefs_content)`（源目录 `littlefs_content/`）|
| JPEG | `esp_new_jpeg` 已在依赖；`fs_service.c` 已用 `esp_jpeg_dec.h`，并有全屏 PSRAM 缓冲 `file_buffer` |
| 色深 | **`LV_COLOR_DEPTH=24`（RGB888）** ← 与参考不同，封面解码要改 |
| 字体 | **子集**（`--symbols` 指定），121 KB / 225 KB / 462 KB，编译进固件 |
| sdkconfig | ✅ 已开 `BT_A2DP_ENABLE` · `BT_AVRCP_ENABLED` · **`BT_AVRCP_CT_COVER_ART_ENABLED`** · `BT_GOEPC_ENABLED`；❌ 未开 `BT_HFP_ENABLE` |
| LVGL FS 驱动 | **未注册**（`lv_fs_drv_register` 全项目 0 处）← 用文件字体必须补 |
| UI 框架 | `page_main.c` 的 `lv_tabview`，当前 **5 个 tab**（CALL/REC/AUDIO/VIDEO/SYS）|

### 1.3 差异对照（移植映射表）

| 参考仓库 | 本项目 | 处置 |
|---|---|---|
| Kconfig choice 编译期选角色 | 需**运行时切 tab** | ⚠ **必须重新初始化 host**（见 §3.1）|
| `LV_COLOR_DEPTH=16` | `24` | 封面解码输出改 **RGB888** |
| 字体 13.3 MB C 数组进固件 | 要求进 LittleFS | 改 **bin 字体 + `lv_binfont_create`**（见 §5）|
| `bt_ui.c` 全套自建 UI | 已有 `ui_common`/`ui_theme` 设计语言 | 复用本项目主题，仅**取参考的封面+元数据布局** |
| `esp_board_manager` 板级管理 | 本项目用 `bsp/esp-bsp.h` | 保持本项目 BSP 调用方式 |
| `static bt_ui_media_refs_t` 单实例 | 需挂进 tab（可多次创建） | 改为 `lv_obj_set_user_data` 动态分配 |
| HFP 通话/拨号器/PBAP | 用户未要求 | **本期不做**（见 §3.4）|
| 无 LittleFS | 已有 `/littlefs` | 作为字体与封面缓存落点 |

---

## 2. 总体架构

### 2.1 数据流

```
                    ┌──────────────── 手机 / 电脑 (A2DP Source + AVRCP CT) ────────────────┐
                    │                                                                       │
        A2DP 音频流 │                                                     AVRCP 控制/元数据 │
                    ▼                                                                       ▼
   ┌────────────────────────────────┐                        ┌──────────────────────────────┐
   │  esp_bt_audio (A2DP_SNK)       │                        │  esp_bt_audio (AVRC_CT/TG)   │
   │  io_bt  →  aud_dec(SBC)  →     │                        │  MEDIA_CTRL_CMD              │
   │  aud_asrc → io_i2s             │                        │  PLAYBACK_STATUS_CHG         │
   └────────────┬───────────────────┘                        │  PLAYBACK_METADATA(type/value)│
                │ PCM                                        └───────────────┬──────────────┘
                ▼                                                            │
      ┌──────────────────┐                          ┌──────────────────────┴────────────┐
      │ ES8389 codec     │                          │  标题/艺术家/专辑/流派  →  直接刷新 │
      │ (板载扬声器)      │                          │  COVER_ART(JPEG) → 队列 → 解码任务 │
      └──────────────────┘                          └──────────────────────┬────────────┘
                                                                           ▼
                                                            ┌──────────────────────────┐
                                                            │ btsink tab (LVGL)        │
                                                            │ 封面 + 曲目信息 + 控制键  │
                                                            └──────────────────────────┘
```

### 2.2 模块划分（新增/改动）

| 层 | 文件 | 动作 |
|---|---|---|
| middleware/bluetooth | `bt_sink_service.c/.h` | **新增**：Sink 管线（bt2codec）+ 元数据/封面事件分发 |
| middleware/bluetooth | `bt_audio_service.c/.h` | **改**：角色随模式切换（重 init host）、新增元数据/封面事件透传 |
| ui/tabs | `tab_btsink.c/.h` | **新增**：复现参考 UI 的接收页 |
| ui/common | `ui_binfont.c/.h` | **新增**：LittleFS 字体加载（`lv_binfont_create`）+ `lv_fs` 驱动注册 |
| ui/pages | `page_main.c` | **改**：5 tab → 6 tab（加 `btsink`）|
| assets | `littlefs_content/fonts/*.bin` | **新增**：二进制字体（替代 C 数组）|
| 构建 | `main/CMakeLists.txt` | **改**：登记新 `.c`（本项目硬性约定）|
| 配置 | `sdkconfig` / `sdkconfig.defaults` | **改**：LVGL FS / binfont / 分区相关开关 |

---

## 3. 关键决策（**需你拍板**）

### 3.1 ⚠ 决策一：Source / Sink 如何共存

#### 硬约束（已逐行核对 IDF 源码，非推测）

**① sdkconfig 层面：只有一个总开关，"两个都开"这个前提不成立**

`bt_target.h:88-95`（只要 `CONFIG_BT_A2DP_ENABLE=y` 就成立）：
```c
#define BTA_AV_SINK_INCLUDED   TRUE
#define BTC_AV_SINK_INCLUDED   TRUE
#define BTC_AV_SRC_INCLUDED    TRUE
```
→ **Source 与 Sink 的代码本来就都编译进固件**，既不需要、也不存在两个开关。
（`CONFIG_BT_A2DP_ENABLE` 是唯一的 A2DP 总开关，本项目已开。）

**② 运行时层面：A2DP 是全局单例，Source / Sink 只能激活一个**

`esp_a2dp_api.c` —— 两者**共用同一个** `g_a2dp_on_init`：
```c
/* L49  */ esp_err_t esp_a2d_sink_init(void) {
              if (g_a2dp_on_init || g_a2dp_sink_ongoing_deinit) return ESP_ERR_INVALID_STATE;
/* L332 */ esp_err_t esp_a2d_source_init(void) {
              if (g_a2dp_on_init || g_a2dp_source_ongoing_deinit) return ESP_ERR_INVALID_STATE;
```

`btc_av.c:1255-1290` —— 只维护**一个** AV 状态机：
```c
if (service_id == BTA_A2DP_SOURCE_SERVICE_ID) {
    stat = btc_a2dp_source_startup();
} else if (service_id == BTA_A2DP_SINK_SERVICE_ID) {   /* ← if / else if，二选一 */
    stat = btc_a2dp_sink_startup();
}
...
g_a2dp_on_init = true;
```
→ **结构上就是二选一**，不是"可能冲突"。本机 `bt_audio_service.c:117` 那条注释是正确的。

**③ 因此切换角色只能"整个 host 重建"**

`esp_bt_audio` 的角色位是在 `esp_bt_audio_init(&cfg)` 里**一次性传入**的（`cfg.classic.roles`），
**init 之后改不了**。所以不能"单独 deinit source、再 init sink"，只能是：

```
set_power(false)        → esp_bt_audio_deinit()   （销毁 host：bluedroid + 全部 profile）
s_role = BT_ROLE_SINK   （改角色位）
set_power(true)         → esp_bt_audio_init()     （用新角色位重建 host）
```

> **"运行时切换"的准确含义 = 蓝牙 host 推倒重建**，而不是给已有 host 打补丁。

**④ 什么共通、什么不共通**（回答"协议栈是否共通"）

| 层 | 是否共通 | 说明 |
|---|---|---|
| **Controller**（硬件 + HCI 内存池） | ✅ **常驻共通** | `bt_audio_service_controller_init()` 开机早期建立，`set_power` 从不碰它；Source/Sink 共用同一 controller |
| **Host**（bluedroid） | ❌ **不共通** | 每次 `set_power(true)` 都是**真正的重新 init** |
| **A2DP profile** | ❌ **互斥** | `g_a2dp_on_init` 全局单例，同一时刻只能 SRC 或 SNK |
| **AVRCP profile** | ⭕ **不互斥** | `bt_audio_classic.c:37-42` 里 CT / TG 是**独立 if** → 可同时注册 |

→ 合法：`A2DP_SNK | AVRC_CT | AVRC_TG`（参考仓库的配置）
→ 非法：`A2DP_SRC | A2DP_SNK`

**⑤ 代价（必须接受）**

- **按"角色切换"按钮**会断开蓝牙连接：这是 host 重建的必然代价（A2DP 单例，见 ②）。
  Source 模式推流中按按钮切到 Sink → 耳机掉线；切回需重连。
  **但切 tab 本身不断连** —— tab 只是 UI，不碰蓝牙栈（见下 ⑦）。
- 重建有数百 ms 延迟 + 内存峰值；本项目 `set_power(true)` 有内部 RAM 体检闸门，不足会返回 `ESP_ERR_NO_MEM`

**⑥ 附带发现（后续实现需注意的坑）**

`esp_bt_audio_media.h:25/37` 与 `bt_audio_ops.c:467/476` 显示
**`esp_bt_audio_media_start/stop()` 目前只接受 `A2DP_SRC` 角色**。
对 Sink 无影响（流由手机发起、本机被动接收，不需要调它），
但若将来想让 Sink 侧主动起流，会撞到这个 API 限制。

**⑦ 切换的触发点：页内按钮，不是 tab（★ 用户定调，2026-09-22）**

角色切换是一个**明确、主动**的动作，必须由用户在 **Bluetooth 子页面**（`page_bluetooth.c`）
按按钮触发 —— 该按钮**已存在**：`s_role_btn`（"Role: Source/Sink (tap)"）+ `role_btn_cb`。
**不要新造 tab 触发机制。**

- **切 tab ≠ 切角色**。`tab_changed_event` **不得**调用任何角色切换，否则用户每划一次 tab
  就莫名掉线。
- **btsink tab 只做 UI 渲染**：进入（`tab_btsink_build`）时读 `bt_audio_service_get_role()`：
  - `== BT_ROLE_SINK` → 建/显示媒体界面（封面 + 元数据 + 控制键）；
  - `!= BT_ROLE_SINK` → 只显示**空态占位**（"Sink 未启用 / 去 SYS > Bluetooth 切换"），
    不建管线、不连设备。
- 按按钮导致角色变化时：若此刻正停在 btsink tab，由 500 ms 定时器发现 role 变化 → 切到空态；
  若不在 btsink tab，下次进入时按新角色渲染即可。

> **现状说明（Phase 1 要接通的地方）**：`bt_audio_service_set_role()` **目前只写 `s_role` 变量**
> （`bt_audio_service.c:348-352`），且 `classic_roles()` **硬编码只返回 SRC|CT、根本没读 `s_role`**
> （`:118-122`）。所以现有按钮只是个"UI 存根"，按了不改实际角色。Phase 1 要把它接通：
> `classic_roles()` 按 `s_role` 返回 + `role_btn_cb` 改走 `switch_role()`（off → 改 → on）。

#### 方案对比

| 方案 | 做法 | 优点 | 代价 |

|---|---|---|---|
| **A. 运行时切换（推荐）** | 切到 btsink tab → `set_power(false)` → 改角色 → `set_power(true)` | 一个固件两种角色，符合"新建 tab"的诉求 | 每次切换重新 init host（~数百 ms）；本项目 `set_power(true)` 已有**内部 RAM 体检闸门**，切换前必须先关掉占用大的页面 |
| B. 编译期二选一 | 学参考仓库，Kconfig 定死 | 最稳、最省内存 | 想要推流就得重编固件，与"tab"诉求冲突 |

> **建议 A**，触发点定在 Bluetooth 页的按钮。切换逻辑要保守：按按钮时先停播、释放 AUDIO 页资源，再 init host。
> 具体见 §4.2。

### 3.2 ⚠ 决策二：中文字体字符范围（直接影响体积）

参考仓库是**全 CJK**（20992 字），C 源 13.3 MB；实测其**位图数据 1.64 MB**，转二进制约 **1.9 MB**。

| 选项 | 字符范围 | bpp | 预估 bin 体积 | 说明 |
|---|---|---|---|---|
| **A. 复现参考** | `0x20-0x7E,0x3000-0x303F,0x4E00-0x9FFF,0xFF00-0xFFEF` | 1 | **≈1.9 MB** | 完全克隆；占 littlefs 32% |
| **B. 常用字优化（推荐）** | GB2312 全集（6763 字）+ ASCII + 标点 | 1 | **≈0.6 MB** | 覆盖 99.9% 日常中文，占 10% |
| C. 观感优先 | 同 B 范围 | **4** | ≈2.4 MB | bpp4 抗锯齿，深色底上更细腻 |

> **建议 B**（bpp=1 与参考一致）。若你觉得 bpp=1 在深色背景上边缘太硬，可换 C。
> 注意：本项目色深 24 位，bpp=1 字体渲染时会被 LVGL 做 alpha 混合，观感优于 16 位下的表现。

### 3.3 ⚠ 决策三：字体加载方式

| 方式 | API | 说明 |
|---|---|---|
| **A. 文件路径（推荐）** | `lv_binfont_create("/littlefs/font_cn_28.bin")` | 需先注册 `lv_fs` 驱动；**LAZY 按需读字形**，RAM 占用小 |
| B. 整体入内存 | `lv_binfont_create_from_buffer(buf, size)` | 从 LittleFS 读 1.9 MB 进 PSRAM；实现简单但吃内存 |

> **建议 A**：`lv_binfont_create_ex` 支持 `dynamic_glyph_load`（按需加载 glyph），
> 对 1.9 MB 字体最省 RAM。代价是要写一个 ~60 行的 `lv_fs` 驱动把 LittleFS 的 POSIX 接口桥过去。

### 3.4 决策四：是否包含通话（HFP）

参考仓库含 HFP 通话 + 拨号器 + PBAP 通讯录。**用户需求未提及**。

> **建议本期不做**。理由：本项目 `CONFIG_BT_HFP_ENABLE` 当前**未开**，开启会显著增加
> 蓝牙内存占用与复杂度；且需求清单只列了音频接收 + 控制 + 元数据 + 封面。

### 3.5 决策五：`btsink` tab 命名与位置

当前 5 个 tab 已占满 800 px 宽（每个 ~160 px）。再加第 6 个会挤。

| 选项 | 做法 |
|---|---|
| **A. 加为第 6 个 tab（推荐）** | 名称用短词，如 `SINK` 或 `BT-IN`；图标 `LV_SYMBOL_BLUETOOTH` |
| B. 合并进现有 | 把 Sink 做成 AUDIO 页的第三种"出口模式"（LOCAL / BT PUSH / BT IN）|

> 你明确说"新建一个 tab 叫做 btsink"，故按 **A** 实施，但**显示名**建议缩短（`btsink` 六个字符在 133 px 里会截断）。
> 若坚持显示 `btsink`，可考虑把 tab bar 字体调小或允许横向滚动。

---

## 4. 分阶段实施

### Phase 0 — 依赖与配置（不改逻辑，先让编译环境就绪）

1. **sdkconfig / sdkconfig.defaults** 补开关：
   ```
   CONFIG_LV_USE_FS_POSIX=y            # 或 LV_USE_FS_STDIO，取决于桥接方式
   CONFIG_LV_FS_POSIX_LETTER=65        # 'A' → 路径 "A:/littlefs/xxx.bin"
   CONFIG_LV_BIN_DECODER_... (若有)    # 让 binfont_loader 参与编译
   ```
   > ⚠ 这两个开关**必须双写** `sdkconfig` 与 `sdkconfig.defaults`（本项目既有踩坑：只改一个会被覆盖）。
2. **CMakeLists.txt**：确认 `esp_new_jpeg` 已依赖（已满足）；新增文件按约定登记。
3. **分区**：无需扩大（见 §5.1 测算）。

**产物**：可编译的空壳，无行为变化。

---

### Phase 1 — 蓝牙层：注册 Sink 角色

**文件**：`middleware/bluetooth/bt_audio_service.c`

1. 把 `classic_roles()` 从"写死 Source"改为**按当前模式返回**：
   ```c
   static uint32_t classic_roles(void) {
       return (s_role == BT_ROLE_SINK)
              ? (ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SNK | ESP_BT_AUDIO_CLASSIC_ROLE_AVRC_CT)
              : (ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC | ESP_BT_AUDIO_CLASSIC_ROLE_AVRC_CT);
   }
   ```
   > `AVRC_CT` 两种模式共用（本机作为 CT 去控制对端播放器）；
   > 如需同时当 TG 接收对端命令，可再加 `AVRC_TG`（参考仓库两个都开）。
2. 新增 **`bt_audio_service_switch_role(bt_role_t)`**：
   ```c
   esp_err_t bt_audio_service_switch_role(bt_role_t role) {
       if (s_role == role && s_on) return ESP_OK;
       bool was_on = s_on;
       if (was_on) bt_audio_service_set_power(false);   /* deinit host */
       s_role = role;
       if (was_on) return bt_audio_service_set_power(true);  /* re-init with new role */
       return ESP_OK;
   }
   ```
   > ⚠ **调用者只有一处**：`page_bluetooth.c` 的 `role_btn_cb`（角色按钮）。
   > **不要**在 `tab_changed_event` 里调用（用户定调：tab 切换不得碰蓝牙栈，见 §3.1 ⑦）。
   > ⚠ **关键**：`set_power(true)` 里有内部 RAM 体检闸门（`BT_HOST_MIN_FREE_INTERNAL`）。
   > 切换前必须确保 AUDIO 页/视频页已释放，否则会返回 `ESP_ERR_NO_MEM`。
   > UI 侧要把 `ESP_ERR_NO_MEM` 提示给用户，而不是当成功处理（既有约定）。
3. Sink 模式下开**可被发现**：`esp_bt_audio_classic_set_scan_mode(true, true)`（参考仓库做法）。
4. **新增事件透传**（当前 `on_bt_audio_event` 缺这两支）：
   - `ESP_BT_AUDIO_EVENT_PLAYBACK_STATUS_CHG` → `BT_EVT_PLAYBACK_STATUS`
   - `ESP_BT_AUDIO_EVENT_PLAYBACK_METADATA` → `BT_EVT_METADATA`（含 `type/length/value`）
   现有 `BT_EVT_MEDIA_CMD` / `BT_EVT_VOL_ABSOLUTE` 已就绪，直接复用。

**产物**：`btsink` 模式下手机能搜到并连上本机。

---

### Phase 2 — 音频层：bt2codec 接收管线

**文件**：`middleware/bluetooth/bt_sink_service.c/.h`（**新增**）

仿照现有 `bt_push_service.c` 的「方案 B 延迟常驻」设计（pool 早建、栈外置 PSRAM、
建一次不毁、异步销毁），方向反过来：

```
Sink 管线元素链：io_bt(stream) → aud_dec(SBC) → aud_asrc → io_i2s(codec)
```

关键点（参考 `stream_proc.c:528-543`）：
1. `STREAM_STATE_CHG` 的 **ALLOCATED** 时建/取管线，用
   `esp_gmf_audio_dec_reconfig(pipe->head_el, &simple_dec_cfg)` 按对端 codec 配置解码器
   （`dec_type = ESP_AUDIO_TYPE_SBC`）。
2. 用 `stream_proc_set_asrc_dest()` 的等价逻辑，把 ASRC 目标设为 `CODEC_DAC_SAMPLE_RATE`。
3. **Sink 特有：时钟同步**（参考用了 `clk_sync` + `playback_sync`）。
   手机时钟与板载 I2S 时钟存在漂移，不做同步会周期性 under/over-run。
   → 本阶段**先做最小可用**（不对齐时钟），跑通后再视卡顿情况补（见 §7 风险）。
4. `STREAM_STATE_CHG` 的 **STOPPED** 时不销毁，只 `reset`（与推流同策略）。

**产物**：手机播放音乐，板载扬声器出声。

---

### Phase 3 — 元数据与封面

**文件**：`middleware/bluetooth/bt_sink_service.c`（事件处理）

#### 3.1 元数据（标题/艺术家/专辑/流派）

参考 `main.c:444-481`：

```
PLAYBACK_STATUS_CHG(TRACK_CHANGE)
        ↓ 主动请求
esp_bt_audio_playback_request_metadata(
     TITLE | ARTIST | ALBUM | GENRE | COVER_ART)
        ↓ 对端回
ESP_BT_AUDIO_EVENT_PLAYBACK_METADATA  (type / length / value)
        ├── 非封面：value 是 UTF-8 字符串 → 缓存到 static 缓冲 → 通知 UI
        └── 封面  ：value 是 esp_bt_audio_playback_cover_art_t*
```

> ⚠ **线程纪律**（本项目铁律）：事件回调跑在 BT 线程，**绝不直接碰 LVGL**。
> 只写 volatile 标志 / 投队列，由 LVGL 定时器或独立任务消费（同 `tab_call.c` 既有做法）。

#### 3.2 封面（JPEG）

参考 `bt_ui.c:695-822` + `bt_ui_cover_task`：

```
POST_COVER(data,size)
   → 拷一份进 FreeRTOS 队列（队列容量 2，丢弃旧的）
   → 独立任务 "ui_cover"(栈 4096, prio 5)
        ├── lvgl_port_lock()
        ├── esp_jpeg_get_image_info()  取宽高，按目标尺寸选 scale(1/1,1/2,1/4,1/8)
        ├── malloc(outbuf)  →  esp_jpeg_decode()
        ├── 填 lv_image_dsc_t → lv_image_set_src()
        └── lvgl_port_unlock()
```

**本项目必须改的两处**：
1. **色深**：`out_format` 从参考的 `JPEG_IMAGE_FORMAT_RGB565` 改为
   **`JPEG_IMAGE_FORMAT_RGB888`**，`cf` 用 `LV_COLOR_FORMAT_RGB888`（本项目 24 位）。
2. **目标尺寸**：参考封面区 360×340，本项目 tab 内可用空间更小（见 §4 Phase 4），
   按实际布局定 `COVER_IMG_SIZE`。

> `esp_jpeg` 的 `swap_color_bytes` 在 24 位下不需要；RGB888 时注意是否需 BGR 交换，实测微调。

**产物**：手机切歌时，tab 上显示曲名/艺术家与专辑封面。

---

### Phase 4 — UI：`btsink` tab

**文件**：`ui/tabs/tab_btsink.c/.h`（**新增**）

**设计原则**：**布局参考参考仓库，视觉沿用本项目主题**（`ui_common` + `ui_theme`），
不把参考那套深蓝 `0x102840` 硬编码搬进来（本项目已统一 AI 深蓝令牌）。

布局（横向，塞进 tabview 内容区）：

```
┌──────────────────────────────────────────── tabview 内容区 ────────────────────────────────────────────┐
│ ┌──────────────────┐   ┌──────────────────────────────────────────────┐                                │
│ │                  │   │  标题 (lv_font_cn_28, COL_TEXT, 自动换行)      │                                │
│ │   专辑封面         │   │  艺术家 (COL_SUBTEXT)                          │                                │
│ │   (方形/圆角)      │   │  专辑 / 流派 (COL_SUBTEXT)                     │                                │
│ │   无封面时显示      │   │  ────────────────────────                     │                                │
│ │   占位图标         │   │  [◀◀]   [▶/⏸]   [▶▶]   ← AVRCP 控制          │                                │
│ └──────────────────┘   └──────────────────────────────────────────────┘                                │
│  状态行：BT 连接/播放状态 · 音量(远端绝对音量) · "-- no track --"                                        │
└─────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

关键实现点：
1. **不要用参考的 `static` refs**（它只能单实例）。本项目 tab 会 destroy/rebuild，
   用 `lv_obj_set_user_data(root, heap_caps_malloc(...))` + 在 `tab_btsink_destroy()` 释放。
2. **控制键回调** → 直接调 `esp_bt_audio_playback_play/pause/next/prev()`（CT 方向）。
3. **状态刷新**：500 ms `lv_timer`（与 `tab_audio.c` 一致），从 volatile 变量取最新状态。
4. **封面对象**：`lv_image_create` + `lv_obj_set_style_radius` 做圆角；
   本项目 24 位下 `lv_image_dsc_t.header.cf = LV_COLOR_FORMAT_RGB888`。
5. 中文字体：`lv_obj_set_style_text_font(label, s_font_cn, 0)`（来自 Phase 5 加载的字体句柄）。

**改 `page_main.c`**：
- `lv_tabview_add_tab(s_tabview, LV_SYMBOL_BLUETOOTH"  SINK")` → `tab_btsink`
- `main_create` 里 `tab_btsink_build(tab_sink, NULL)`
- `main_destroy` 里 `tab_btsink_destroy()`
- **切 tab 时触发角色切换**：`tab_changed_event` 里判断目标 tab 是否为 btsink，
  调 `bt_audio_service_switch_role()`（配合 §3.1 方案 A）。

**产物**：能看到并操作 btsink 页。

---

### Phase 5 — 中文取模预渲染 → LittleFS

这是本轮**唯一与你原需求有偏离的环节**（参考仓库是编译期 C 数组），方案如下。

#### 5.1 目标

把中文字体从「C 数组编译进固件」改为「**二进制文件放 LittleFS，运行时加载**」。

#### 5.2 生成 bin 字体

用 LVGL 官方 `lv_font_conv`（Node 工具）：

```bash
# ★ 2026-09-21 订正：本节的旧命令是错的/过时的，已按实际产出字库反推订正。
#   旧版漏了 0xFF00-0xFFEF，更要命的是还漏了日文假名 0x3040-0x30FF，
#   导致日文标题（如 夜の不幸 / パミレド）整段显示为乱码。详见 改动记录.md §28.27。
node <repo>/.tmp-fontgen/node_modules/lv_font_conv/lv_font_conv.js \
  --size 28 --bpp 1 --no-compress --no-kerning --format bin \
  --font <LVGL>/scripts/generators/built_in_font/SourceHanSansSC-Normal.otf \
  -r 0x20-0x7E,0x3000-0x30FF,0x4E00-0x9FFF,0xFF00-0xFFEF \
  -o littlefs_content/fonts/lv_font_cjk_28.bin
```

> ⚠ `--format bin` 才会输出二进制；`--format lvgl` 输出的是 C 数组（参考仓库用的就是它）。
> ⚠ `--no-kerning` **不要省**：若不关 kerning，只要所选码位里存在字距对，
> 就会多出 `kern` 表并把 `advance_width_format` 由 0 变 1、
> `advance_width_bits` 由 6 变 10，走上一条本工程未验证的加载分支。
> ⚠ 源字体用**含假名**的思源黑体 / Noto Sans SC；`NotoSansSC-Regular` 亦可，
> 三者的 `typo_asc/typo_desc/underline` 一致，可互换。
> 若要 GB2312 子集，把 `-r 0x4E00-0x9FFF` 换成 `--symbols <GB2312 字符文件>`，
> 但**务必保留假名段**否则日文曲目会乱码。

**体积实测依据**（来自对参考仓库字体的逐字节统计）：

| 项 | 字节 |
|---|---|
| 参考 C 源文件 | 13,239,109 |
| 其中 `glyph_bitmap[]` 真实位图 | **1,724,833 ≈ 1.64 MB** |
| glyph 个数 | 21,360 |
| → 推断 bin 字体（含 dsc + cmap） | **≈ 1.9 MB** |

#### 5.3 放进 LittleFS

1. 把 `lv_font_cjk_28.bin` 放进 `littlefs_content/fonts/`。
2. 重新生成镜像并**单独烧录**（注意：本项目 `littlefs_create_partition_image` **不带** `FLASH_IN_PROJECT`）：
   ```bash
   idf.py build                                            # 生成 build/storage.bin
   esptool --chip esp32s31 -p COM9 write-flash 0xA20000 build/storage.bin
   ```
   > ⚠⚠ **参数顺序：地址在前、文件在后**。esptool 的用法是
   > `write-flash [OPTIONS] <address> <filename>...`。写成
   > `write-flash build/storage.bin 0xA20000` 会报
   > `Address "build/storage.bin" must be a number.`（2026-09-22 实际踩到）
   >
   > ⚠ **重要更正**：`CMakeLists.txt` 旧注释里的烧录偏移 `0x620000` **已过时**。
   > 当前 `partitions.csv` 里 `storage` 的实际偏移是 **`0xA20000`**（`ota_1` 结束处），
   > 大小 `0x5E0000` = 6,160,384 B。该注释与本节均已于 2026-09-22 订正。
   > 烧前务必核对 `partitions.csv`；烧错地址会覆盖 `ota_0`。
3. **⚠ 风险**：`fs_service_mount()` 设了 `format_if_mount_failed = true`。
   一旦挂载失败会**格式化整个 littlefs**，字体（和其他素材）全部丢失。
   字体上线后建议改为 `false`，或至少把字体纳入"可重新烧录"的认知。

#### 5.4 运行时加载

**新增 `ui/common/ui_binfont.c/.h`**：

```c
/* ① 注册 LVGL 文件系统驱动（把 LittleFS 的 POSIX 接口桥过去），全项目一次 */
void ui_binfont_fs_init(void);

/* ② 从 LittleFS 加载 bin 字体（LAZY 按需读字形） */
lv_font_t *ui_binfont_load(const char *path);   /* 如 "/littlefs/fonts/font_cn_28.bin" */

/* ③ 释放 */
void ui_binfont_free(lv_font_t *font);
```

实现要点：
- 用 `lv_binfont_create_ex(&dsc)` 并置 `dynamic_glyph_load = true`（省 RAM）。
- ⚠⚠ **2026-09-21 重要更正**：**不要**用 `lv_binfont_create_from_buffer()`。
  它只是 `lv_binfont_create_ex()` 的语法糖，但会把 `dsc` **零初始化**，
  于是 `dynamic_glyph_load` 落到默认的 `false`，`load_glyph()` 会
  `lv_malloc()` 出一整块 **glyf 展开尺寸**（本字体 ≈1.71MB）的位图缓存。
  必须走 `lv_binfont_create_ex{ .buffer, .buffer_size, .dynamic_glyph_load = true }`。
  > "文件副本是否常驻"（A 轴）与"字形位图是否全展开"（B 轴）是**两件独立的事**，
  > 旧文把它们混为一谈了。
- **失败兜底**：加载失败时回退到现有的 `lv_font_cn_20/30/45`（C 数组，不删），
  保证 UI 不缺字、不崩。
- ⚠ 开 `dynamic_glyph_load = true` 后，LVGL 会**长期持有**那个 memfs 文件句柄，
  每次渲染按 `loca` 偏移现读字形 ⇒ **源缓冲必须常驻到字体销毁为止**，
  不能 `create` 完就 free（悬空指针 → HardFault）。

---

## 5. 分区与资源测算

### 5.1 LittleFS 容量是否够

| 项 | 数值 |
|---|---|
| `storage` 分区 | 0x5E0000 = **6,160,384 B ≈ 5.875 MB** |
| 现有内容 | `boot_tone.wav` + `xwkkk.jpg`（体积待核，量级很小）|
| 新增字体（方案 B） | ≈ 0.6 MB |
| 新增字体（方案 A，复现参考） | ≈ 1.9 MB |
| **余量** | 方案 B 余 ~5.2 MB；方案 A 余 ~3.9 MB |

✅ **两种方案都放得下**。LittleFS 本身有元数据开销（约 3%），远低于余量。

### 5.2 固件体积变化

- **减少**：中文字体不再编译进 app（若把现有 C 数组字体也迁到 LittleFS，可省 0.8 MB flash）
- **增加**：Sink 管线 + UI 代码（代码量小，忽略）
- app 分区 5 MB，余量充足。

### 5.3 内存（PSRAM / 内部 RAM）

| 项 | 影响 |
|---|---|
| 封面解码缓冲 | 按实际封面对角线 × 3 B；360×360×3 ≈ 380 KB → **必须 PSRAM** |
| bin 字体 LAZY 加载 | 只缓存当前用到的字形，占用小 |
| bt2codec 管线 | 参照 `bt_push_service` 的"pool 早建 + 显式外置 PSRAM"策略 |
| ⚠ 内部 RAM | Sink 模式同样受 `BT_HOST_MIN_FREE_INTERNAL`(48 KB) 约束；**切角色前先释放其他重页** |

---

## 6. 验证清单

按阶段验收，每步都要能独立跑通：

**Phase 1**
- [ ] `btsink` tab 下手机能搜到本机并成功配对连接
- [ ] 日志出现 `classic_connect` / `CONNECTION_STATE_CHG: CONNECTED`
- [ ] 切回 AUDIO tab 后推流功能仍正常（角色切回 Source）
- [ ] 内存不足时 `set_power` 返回 `ESP_ERR_NO_MEM` 且 UI 有提示、不崩

**Phase 2**
- [ ] 手机播放音乐 → 板载扬声器出声，无爆音
- [ ] 播放 30 min 无周期性卡顿/断流（时钟同步是否达标）
- [ ] 暂停/恢复/停止 → 日志状态机正确，无 pipeline 泄漏

**Phase 3**
- [ ] 切歌时 UI 显示正确的标题/艺术家/专辑
- [ ] 封面正确显示（JPEG 解码成功，无花屏/偏色）
- [ ] 快速连续切歌不崩（队列容量 2，旧封面被丢弃）

**Phase 4**
- [ ] tab bar 6 个 tab 显示正常、不截断
- [ ] 控制键（⏮ ▶/⏸ ⏭）能控制手机播放
- [ ] 反复进出 btsink tab 不泄漏（对象树正确 destroy）

**Phase 5**
- [ ] `font_cn_28.bin` 烧录成功，`ls` 可见
- [ ] 运行时加载成功，中文正常显示（不是方块）
- [ ] 故意重命名字体文件 → UI 回退到内置字体而非崩溃

**全局**
- [ ] 推流（AUDIO/bt push）与接收（SINK）两条链路互不干扰
- [ ] `idf.py build` 无新警告

---

## 7. 风险与回退

| 风险 | 可能性 | 影响 | 缓解 |
|---|---|---|---|
| **A2DP SRC/SNK 切换时 host 重初始化失败** | 中 | 蓝牙整体不可用 | 切角色前强制释放重页 + 内存体检；失败回退到 Source 并提示 |
| **Sink 时钟漂移导致周期性卡顿** | 中 | 听感差 | Phase 2 先最小实现，卡顿再补参考的 `clk_sync`/`playback_sync` |
| **封面解码内存不足** | 中 | 封面不显示 | 强制 PSRAM + 按尺寸选 `esp_jpeg` scale（1/2、1/4、1/8）|
| **`format_if_mount_failed=true` 抹掉字体** | 低但**致命** | 素材全丢 | 改为 `false`；字体可重新烧录 |
| **烧录偏移写错（0x620000 vs 0xA20000）** | 中 | 烧到 app 区，砖 | 订正注释；烧前核对 `partitions.csv` |
| **bpp=1 中文观感偏硬** | 低 | 审美 | 换 bpp=2/4（体积上升，见 §3.2-C）|
| **6 个 tab 太挤** | 中 | 显示截断 | 缩短 tab 名；或改 tab bar 字号 |
| **`lv_binfont_create` 在 v9.6 行为差异** | 低 | 加载失败 | 已确认 API 存在于本仓库版本；失败回退内置字体 |
| **AVRCP 元数据对端不支持** | 中 | 元数据/封面为空 | 参考仓库同条件下可用；UI 需能优雅显示"无元数据" |

### 回退策略

整个方案**分层可回退**：
1. Phase 1/2 失败 → 移除 `btsink` tab，其余功能零影响（新代码都在独立文件里）。
2. Phase 3 失败 → Sink 仍可听歌，只是无元数据/封面。
3. Phase 5 失败 → 回退用现有 C 数组字体（不删源码）。

> **重要**：所有新增都在**新文件**里，对现有 `tab_audio` / `bt_push_service` 只做**最小侵入**改动，
> 保证任何阶段都可以单独停下来。

---

## 8. 附录

### 8.1 关键 API 速查（本项目组件内已存在）

| API | 头文件 | 用途 |
|---|---|---|
| `esp_bt_audio_init(&cfg)` | `esp_bt_audio.h` | 初始化（`cfg.classic.roles` 决定角色）|
| `esp_bt_audio_playback_play/pause/stop/next/prev()` | `esp_bt_audio_playback.h` | AVRCP 控制（CT 方向）|
| `esp_bt_audio_playback_request_metadata(mask)` | 同上 | 主动拉取元数据 |
| `esp_bt_audio_playback_reg_notifications(mask)` | 同上 | 注册远端通知（播放状态/曲目变化/进度）|
| `esp_bt_audio_classic_set_scan_mode(conn, disc)` | `esp_bt_audio_classic.h` | Sink 需 `(true,true)` 可被发现 |
| `esp_bt_audio_media_start/stop(role, ...)` | `esp_bt_audio_media.h` | 起停媒体流 |
| `esp_jpeg_get_image_info()` / `esp_jpeg_decode()` | `esp_jpeg_dec.h` | 封面解码 |
| `lv_binfont_create()` / `lv_binfont_create_ex()` | `lv_binfont_loader` | 文件加载字体 |

### 8.2 元数据掩码（`esp_bt_audio_playback_metadata_mask_t`）

| 位 | 宏 | 类型 |
|---|---|---|
| 0x01 | `..._TITLE` | UTF-8 字符串 |
| 0x02 | `..._ARTIST` | UTF-8 字符串 |
| 0x04 | `..._ALBUM` | UTF-8 字符串 |
| 0x08 | `..._TRACK_NUM` | 数值 |
| 0x10 | `..._NUM_TRACKS` | 数值 |
| 0x20 | `..._GENRE` | UTF-8 字符串 |
| 0x40 | `..._PLAYING_TIME` | 毫秒 |
| 0x80 | `..._COVER_ART` | `esp_bt_audio_playback_cover_art_t*`（含 `format_fourcc/size/data`）|

### 8.3 涉及文件清单

**新增（5）**
- `main/middleware/bluetooth/bt_sink_service.c` / `.h`
- `main/ui/tabs/tab_btsink.c` / `.h`
- `main/ui/common/ui_binfont.c` / `.h`
- `littlefs_content/fonts/font_cn_28.bin`

**改动（5）**
- `main/middleware/bluetooth/bt_audio_service.c` / `.h`（角色切换 + 事件透传）
- `main/ui/pages/page_main.c`（第 6 个 tab + 切换钩子）
- `main/CMakeLists.txt`（登记新 `.c` + 订正烧录偏移注释）
- `sdkconfig` + `sdkconfig.defaults`（LVGL FS / binfont 开关，**双写**）
- `main/middleware/system/fs_service.c`（`format_if_mount_failed` 改 `false`）

### 8.4 参考仓库对应位置（便于对照抄思路）

| 主题 | 参考文件:行 |
|---|---|
| 事件调度中心 | `main/main.c:483-680` |
| 元数据处理 | `main/main.c:444-481` |
| 元数据触发 | `main/main.c:427-434`（TRACK_CHANGE → request_metadata）|
| 封面解码 | `main/bt_ui/bt_ui.c:695-822` |
| 封面任务 | `main/bt_ui/bt_ui.c:237-253` |
| 播放页布局 | `main/bt_ui/bt_ui.c:824-1017` |
| 音量（远端绝对） | `main/main.c:175-232` |
| Sink 管线 | `main/stream_proc.c:528-543` |
| 字体生成参数 | `main/bt_ui/fonts/lv_font_notosanssc_regular_28.c:1-3`（Opts 注释）|
| 角色选择 | `main/Kconfig.projbuild:33-79` |
| Sink 必需 sdkconfig | `sdkconfig.defaults.esp32s31.classic` |

---

## 9. 需要你先确认的 5 件事

1. **§3.1 角色共存**：接受「运行时切换（**在 Bluetooth 页按按钮**重 init host；**切 tab 完全不碰蓝牙**）」？还是宁愿「编译期二选一」？
2. **§3.2 字体范围**：复现参考的全 CJK（1.9 MB）？还是 GB2312 常用字（0.6 MB，覆盖 99.9%）？
3. **§3.3 字体加载**：走「`lv_fs` 驱动 + `lv_binfont_create`（省 RAM、需写 ~60 行桥接）」还是「整体读进 PSRAM」？
4. **§3.4 通话**：确认**本期不做** HFP/拨号器？
5. **§3.5 tab 名**：屏幕宽度有限，显示名用 `SINK` 还是坚持 `btsink`（可能截断）？

确认后我按 Phase 0 → 5 逐步实施，每个 Phase 结束给你一个可编译、可烧录、可验收的中间态。
