# 蓝牙本地推流 · 方案 B「延迟常驻」可执行方案

> 适用工程：`display_audio_photo/`（ESP32-S31-Korvo-1 V1.1，ESP-IDF v6.1.0，`IDF_TARGET=esp32s31`）
> 参考实现：官方例程 `managed_components/espressif__esp_bt_audio/examples/bt_audio/main/stream_proc.c`
> 上游仓库：`espressif/esp-gmf` @ `7d1dce0`，路径 `packages/esp_bt_audio/examples/bt_audio`
> 文档状态：**已落地（选型 B2）**，代码完成待实测
> 编写日期：2026-09-21 ｜ 更新：2026-09-21（更正 §3.3 元素链勘误，补 F22 体积坑）
>
> **决策记录**：B2（`early_init` 建 pool+pipeline+task，全程复用）；推流纳入 `SESSION_BT_PUSH` 互斥；
> UI 复用 `tab_btpush.c/h` 并挂为第 6 个主 tab；首次验证用 SD 卡 MP3。
> 实现与验证清单见 `todolist.md` 附录「蓝牙本地推流」；体积坑见 `docs/工程规范/11_遇到的问题.md` F22。

---

## 0. 一句话目标

把 A2DP Source（本机 SD 卡 MP3 → 蓝牙耳机/音箱）的推流链路接通，**且不再因为新增常驻全局对象/任务而破坏 bluedroid 的静态内存布局**。

---

## 1. 问题定性（为什么上一版失败）

### 1.1 失败现象（用户描述）

> BTPUSH 代码改了 binary 内存布局，踩到了 bluedroid 的静态队列。

### 1.2 根因分析

上一版 `bt_push_service.c` 采用「**首次 play 时懒加载**」建 pipeline：

```c
esp_err_t bt_push_service_play(const char *path)   /* 现 bt_push_service.c:124 */
{
    if (ensure_pipeline() != ESP_OK || !s_pipe) return ESP_FAIL;   /* ← 在这里建 */
    ...
}
```

三条独立成因叠加：

| # | 成因 | 说明 |
|---|---|---|
| 1 | **建仓时机太晚** | `ensure_pipeline()` 发生在「蓝牙已连接、用户点了歌」时。此刻 LCD 双 framebuffer + 摄像头 + WiFi + LVGL + bluedroid 已把内部 RAM 切得很碎，GMF 的 pool/元素实例/ringbuffer 申请不到连续块 |
| 2 | **在 BT 事件线程上下文重分配** | `STREAM_STATE_CHG` 回调由 bluedroid/组件线程派发，线程栈小。在该上下文里做毫秒级多块分配，容易触发 mempool 断言 |
| 3 | **静态池 vs 动态堆争抢** | bluedroid 的 `BTU_TASK`/`BTC_TASK` 队列、HCI mempool 都是**启动时一次性静态分配**的。任何在其后改变堆布局的动作（新增常驻对象、晚分配大块）都可能让原本可用的静态池申请失败 |

### 1.3 官方例程的做法（对照）

仔细读 `stream_proc.c` 后必须纠正一个常见误解：**官方并不是"用完即毁"，而是"一次性全建、永不复用"**：

```c
/* stream_proc.c:81-83 —— 全是文件作用域全局静态句柄 */
static esp_gmf_task_handle_t     local2bt_task   = NULL;
static esp_gmf_pipeline_handle_t local2bt_pipe   = NULL;
static esp_bt_audio_stream_handle_t local2bt_stream = NULL;

/* stream_proc.c:482 —— 复用而非重建：stop + reset 后重设 uri */
static void local2bt_play(const char *uri)
{
    esp_gmf_pipeline_stop(local2bt_pipe);
    esp_gmf_pipeline_reset(local2bt_pipe);
    esp_gmf_io_bt_set_stream(ESP_GMF_PIPELINE_GET_OUT_INSTANCE(local2bt_pipe), local2bt_stream);
    esp_gmf_pipeline_set_in_uri(local2bt_pipe, uri);
    esp_gmf_pipeline_loading_jobs(local2bt_pipe);
    esp_gmf_pipeline_run(local2bt_pipe);
}
```

关键点：
- `local2bt_pipe` / `local2bt_task` 在 `stream_proc_init()` 里**建一次**，`RELEASED` 分支里**不销毁**；
- 唯一按需分配的是 `stream_user_data_t`，且 `heap_caps_calloc(..., MALLOC_CAP_SPIRAM)` —— **放 PSRAM**；
- 三个 GMF 任务的 `cfg.thread.stack_in_ext = true` —— **任务栈进 PSRAM**。

> 结论：官方能有内存，靠的不是"省"，而是"**在正确的时间一次要够**"。这是我们方案的核心借鉴点。

### 1.4 关键结论

**「建不建」不是重点，「什么时候建」和「谁来毁」才是。**

---

## 2. 方案 B 设计

### 2.1 设计原则（四条）

| 原则 | 内容 |
|---|---|
| **P1 时机前置** | 所有 GMF 重分配动作，提前到内部 RAM 最空的窗口（`controller_init()` 之后、`bsp_display_start()` 之前），与现有 controller 常驻策略同源 |
| **P2 显式外置** | GMF task 栈一律 `stack_in_ext = true`（进 PSRAM）；pool 元素 buffer 显式配置，不用默认值 |
| **P3 建一次不毁** | pipeline/task **首次 ALLOCATED 才建**、`STOPPED`/`RELEASED` **只停不毁**，避免碎片累积与"自删 task" |
| **P4 销毁异步** | 若确需销毁（蓝牙关闭 / 长时间空闲），**绝不**在事件回调里同步 destroy，必须投递到独立上下文执行 |

### 2.2 为什么不是「事件里临时建、用完即毁」

| 风险 | 说明 | 工程既有教训 |
|---|---|---|
| **自删 task** | `STOPPED`/`FINISHED` 事件多由 pipeline 自己的 task 派发；在回调里 `esp_gmf_pipeline_destroy()` 等于删掉当前执行栈 | 与「子页返回崩溃」同源：事件回调里同步 `lv_obj_del` 删掉含自身的事件源（见 `docs/工程规范/11_遇到的问题.md`） |
| **碎片累积** | 每次播放 alloc/free 一批中等块，几十次后 `largest_free_block` 掉到建不成 pipeline —— 表现为"第一次能播、后面越来越容易失败" | 见 §1.2 成因 1 |
| **回调上下文重分配** | 「临时建」把重分配放回 BT 事件线程，等于把 §1.2 成因 2 的坑原地保留 | — |

### 2.3 状态机（目标形态）

```
                        ┌──────────────────────────────────┐
   BT_EVT_ON  ──────►   │  host 已起，【不建任何 GMF 对象】 │
                        └──────────────┬───────────────────┘
                                       │ 用户点歌 → media_start()
                                       ▼
                        ┌──────────────────────────────────┐
   首次 ALLOCATED  ──►  │ ensure_pipeline() 建 pool/pipe   │  ← 只发生一次
                        │ 绑 io_bt + set_uri + loading_jobs│
                        └──────────────┬───────────────────┘
                                       ▼
                        ┌──────────────────────────────────┐
       STARTED     ──►  │ pipeline_run()                   │
                        └──────────────┬───────────────────┘
                                       ▼
                        ┌──────────────────────────────────┐
   STOPPED/RELEASED ─►  │ 只 stop() + reset()，【不 destroy】│
                        │ 解绑 stream、清 s_stream 指针     │
                        └──────────────┬───────────────────┘
                                       │ 蓝牙关闭 / 空闲超时（可选）
                                       ▼
                        ┌──────────────────────────────────┐
                        │ 异步销毁（独立上下文，见 §2.5）    │
                        └──────────────────────────────────┘
```

### 2.4 建仓时机前置（P1 的具体落点）

在 `main/app/main.c` 现有顺序上插入一行（**只加一处**）：

```c
void app_main(void)
{
    ESP_ERROR_CHECK(fs_service_mount());
    sdcard_service_mount();
    bsp_i2c_init();

    /* BT controller 早期常驻：此时 LCD/WiFi/摄像头未起，HCI 内存池一次拿成功 */
    bt_audio_service_controller_init();      /* 已存在，main.c:86 */

    /* ★ 新增：BT 推流 GMF pool 预建 —— 抢同一个空窗，之后只建 pipeline */
    bt_push_service_early_init();            /* PSRAM-only，可失败不 abort */

    bsp_display_start();
    ...
}
```

> **注意**：这里建的是 **pool（元素工厂）**，不是 pipeline。pool 是后续 `new_pipeline` 的前提，本身较轻；`pipeline + task` 仍留到首次 `ALLOCATED`（约定见 §2.3）。
> 这样做的收益：把"最重的那次分配"挪到空窗期，首次播放时只剩 pipeline 组装。

**选型说明**：也可以把 pool + pipeline 都在 `early_init` 建好（更接近官方）。**二选一，需你定**：
- **B1（推荐）**：`early_init` 只建 pool；首次 ALLOCATED 建 pipeline。首次播放有约几十 ms 的一次性延迟，但开机不占 pipeline 内存。
- **B2（最稳）**：`early_init` 建 pool + pipeline + task，全程复用不销毁，最贴近官方验证过的路径。

### 2.5 销毁异步化（P4 的具体机制）

沿用工程既有范式（`ui/ui_manager/page_manager.c` 的 `pending_destroy_cb` + 一次性定时器思路）：

```c
/* ── 在 bt_push_service.c 内 ── */
static volatile bool s_destroy_pending = false;
static TaskHandle_t  s_reaper = NULL;          /* 独立小任务，仅空闲期存在 */

/* 事件回调（BT/GMF 线程）——只置标志 + 唤醒，绝不 destroy */
void bt_push_service_request_destroy(void)
{
    s_destroy_pending = true;
    if (s_reaper) xTaskNotifyGive(s_reaper);
}

/* 独立上下文——出队后才真正销毁 */
static void reaper_task(void *arg)
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_destroy_pending) continue;
        s_destroy_pending = false;

        /* 此刻已脱离 pipeline 自身执行栈，destroy 安全 */
        if (s_task) { esp_gmf_task_deinit(s_task); s_task = NULL; }
        if (s_pipe) { esp_gmf_pipeline_destroy(s_pipe); s_pipe = NULL; }
        ESP_LOGI(TAG, "pipeline destroyed (async)");
    }
}
```

> 若按 **B2** 走（全程不销毁），本节可不实现，仅保留接口占位 —— 但**禁止**改成回调内同步销毁。

---

## 3. 与现有代码的接线清单

### 3.1 现状盘点（源码实测）

| 文件 | 行数 | 现状 | 需做什么 |
|---|---|---|---|
| `main/middleware/bt_audio_service.c/h` | 257 | ✅ 已注册 SRCS，已编译 | 改：`STREAM_STATE_CHG` 分支转发给 push_service；`play_source` 去掉 TODO |
| `main/ui/page_bluetooth.c` | 267 | ✅ 已注册，SYS 子页可达 | 无需改（可选：加推流状态） |
| `main/middleware/bt_push_service.c/h` | 213 | ❌ **未注册 SRCS** | 改：重写为方案 B 状态机；注册 SRCS |
| `main/ui/tab_btpush.c/h` | 181 | ❌ **未注册 + 未挂 tab** | 定：挂主页还是并入 Bluetooth 子页 |

### 3.2 必须修改的三个断点

**断点 1 —— `bt_push_service.c` 未注册编译**

`main/CMakeLists.txt` SRCS 当前无该文件，需补：

```cmake
                        "middleware/bt_audio_service.c"
                        "middleware/bt_push_service.c"        # ★ 新增
```

**断点 2 —— 事件未转发（致命）**

现 `bt_audio_service.c:94-99` 只把 stream 事件压成一个 bool 回调给 UI：

```c
case ESP_BT_AUDIO_EVENT_STREAM_STATE_CHG: {
    esp_bt_audio_event_stream_st_t *st = (esp_bt_audio_event_stream_st_t *)event_data;
    ESP_LOGI(TAG, "stream state=%d", st->state);              /* ← 只打日志 */
    if (s_evt_cb) s_evt_cb(st->state ? BT_EVT_STREAM_ON : BT_EVT_STREAM_OFF, NULL, s_evt_ctx);
    break;
}
```

问题：**丢失了 `stream_handle` 和真实的四态语义**（ALLOCATED/STARTED/STOPPED/RELEASED 被压成 bool）。必须改为：

```c
case ESP_BT_AUDIO_EVENT_STREAM_STATE_CHG: {
    esp_bt_audio_event_stream_st_t *st = (esp_bt_audio_event_stream_st_t *)event_data;
    ESP_LOGI(TAG, "stream %p state=%d", st->stream_handle, st->state);
    /* 先转发给 push 服务（带 handle + 完整 state） */
    bt_push_service_on_stream_state(st->stream_handle, st->state);
    /* UI 仍按原语义收 BOOL（保持 page_bluetooth 不变） */
    if (s_evt_cb) {
        bool on = (st->state == ESP_BT_AUDIO_STREAM_STATE_STARTED);
        s_evt_cb(on ? BT_EVT_STREAM_ON : BT_EVT_STREAM_OFF, NULL, s_evt_ctx);
    }
    break;
}
```

> ⚠ **兼容性**：改成 `STARTED` 才置 ON，与原先「任意非零即 ON」不同。需确认 `page_bluetooth.c:76` 的 "Streaming" 文案行为符合预期（只在真正推流时显示）。

**断点 3 —— `play_source` 仍是 TODO**

现 `bt_audio_service.c:240-245`：

```c
esp_err_t bt_audio_service_play_source(const char *path)
{
    if (!s_on) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "play source: %s (TODO: local2bt wiring)", path ? path : "(null)");
    return esp_bt_audio_media_start(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, NULL);
}
```

改为**先登记 uri，再启流**（uri 在 ALLOCATED 里才真正下发，因为那时才知道 stream）：

```c
esp_err_t bt_audio_service_play_source(const char *path)
{
    if (!s_on) return ESP_ERR_INVALID_STATE;
    if (!path) return ESP_ERR_INVALID_ARG;

    /* 1) 把 uri 交给 push 服务暂存（等 ALLOCATED 事件） */
    esp_err_t err = bt_push_service_set_uri(path);
    if (err != ESP_OK) return err;

    /* 2) 触发 A2DP 流；后续由 STREAM_STATE_CHG 驱动 pipeline */
    return esp_bt_audio_media_start(ESP_BT_AUDIO_CLASSIC_ROLE_A2DP_SRC, NULL);
}
```

### 3.3 pipeline 元素链（已核实，与官方一致）

> ⚠ **勘误（2026-09-21 实测更正）**：本方案初稿曾误称"官方用 `aud_rate_cvt → aud_ch_cvt`、
> 与本工程 `aud_asrc` 不一致"。经逐行核对本地
> `managed_components/espressif__esp_bt_audio/examples/bt_audio/main/stream_proc.c:792-807`
> 的 `setup_pipeline_local2bt()`，**官方用的就是 `{"aud_dec","aud_asrc","aud_enc"}`，与本工程完全相同**。
> 下面是实测结论，**无需改元素链**。

官方 `stream_proc.c:794`：
```c
const char *name[] = {"aud_dec", "aud_asrc", "aud_enc"};   /* io_file -> ... -> io_bt */
```

本工程 `bt_push_service.c` 采用**同样的元素链**，元素名与顺序均一致：

```c
const char *el_names[] = {"aud_dec", "aud_asrc", "aud_enc"};
esp_gmf_pool_new_pipeline(s_pool, "io_file", el_names, 3, "io_bt", &s_pipe);
```

关键点在于 **`aud_asrc` 需要显式配置目标采样率/声道**（官方在 `ALLOCATED` 事件里做）：

```c
/* 官方 stream_proc.c:170-189 stream_proc_set_asrc_dest() 的核心三步 */
esp_gmf_asrc_set_dest_rate(asrc, sample_rate);   /* 目标采样率 = 耳机协商值 */
esp_gmf_asrc_set_dest_ch(asrc, dest_ch);         /* 目标声道数 */
esp_asrc_cfg_t *cfg = (esp_asrc_cfg_t *)OBJ_GET_CFG(asrc);
cfg->weight = weight;                            /* 声道混合矩阵 */
cfg->weight_len = src_ch * dest_ch;
```

MP3 44.1k/立体声 → SBC 48k/立体声，**重采样是刚需**（S31 有硬件 ASRC 加速），
asrc 元素的 dest 参数必须按耳机协商结果设置，否则输出采样率不对、耳机不出声。

> 注：本工程 `set_asrc_dest()` 与官方 `stream_proc_set_asrc_dest()` 同构；
> asrc 元素通过 `esp_gmf_pipeline_iterate_element()` + tag 比对 `"aud_asrc"` 定位。

---

## 4. 风险与验证点

| # | 风险 | 判据 / 缓解 |
|---|---|---|
| R1 | `early_init` 抢不到内部 RAM | 打印 `heap_caps_get_free_size/largest_free_block`，失败只打日志不 abort（与 `controller_init` 一致的降级策略） |
| R2 | 首次 ALLOCATED 建 pipeline 失败 | 日志 `ensure_pipeline failed`；此时应回 `media_stop` 并给 UI 报错，不留半初始化状态 |
| R3 | SD 卡读与 Flash 读时序互扰 | 若失败时正在读 SD，需单独排查 SDMMC 4-bit + FAT 与 littlefs 并发。建议首次验证用 **littlefs 内的小 MP3**，排除 SD 变量 |
| R4 | 推流与通话/录音争 codec | **待定**：是否加 `SESSION_BT_PUSH` 到 `session_manager`（见 §5 待定项） |
| R5 | `esp_bt_audio_media_start` 返回成功但无 ALLOCATED | 需在 `play_source` 后加超时兜底（建议 3 s），超时提示"耳机未协商成功" |

### 闭环验证步骤（照 `skills/esp-idf-closed-loop.md`）

```powershell
# 1. 构建
tools\build.ps1

# 2. 烧录（只烧 app，921600）
idf.py -p COM9 -b 921600 app-flash

# 3. 抓日志（12s）
& "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe" docs\_serial_capture.py --port COM9 --secs 20
```

**期望日志序列**（新增标记，便于过滤）：

```
BT_SVC : early controller init: free internal=... largest=... free PSRAM=...
BT_PUSH: early pool ready                      ← 新增
BT_SVC : controller ready (host off)
...
BT_SVC : ON, device name=Korvo_XXXX, role=SOURCE
BT_SVC : connect role=SRC addr=..
BT_SVC : connection CONNECTED addr=..
BT_PUSH: play requested: file:///sdcard/music/xxx.mp3
BT_SVC : stream 0x... state=0                  ← ALLOCATED
BT_PUSH: pipeline ready (lazy)                 ← 首次建仓
BT_PUSH: codec: type=2 sr=48000 ch=3           ← SBC / 48k / 立体声
BT_SVC : stream 0x... state=1                  ← STARTED
BT_PUSH: pipeline state=... (RUNNING)
```

**失败判据**：出现 `panic` / `Guru` / `assert` / `Cache error` / `mem pool` 字样即失败。

---

## 5. 待你确认的决策点

> 这三项定了才能编码，请逐条给结论。

1. **B1 还是 B2？**（§2.4）
   - B1：`early_init` 只建 pool，首次 ALLOCATED 建 pipeline —— 开机省内存，首次播放有一次性延迟
   - B2：`early_init` 建 pool + pipeline + task，全程复用 —— 最贴近官方，最稳

2. **推流是否纳入 `session_manager` 互斥？**
   现枚举只有 `SESSION_UDP_CALL` / `SESSION_TCP_RECORD`。是否新增 `SESSION_BT_PUSH`，禁止与通话/录音并存？

3. **`tab_btpush` 的入口怎么处理？**
   - 方案甲：在 `page_main.c` 加第 6 个 tab（当前 5 个：CALL/REC/MUSIC/VIDEO/SYS）
   - 方案乙：删掉 `tab_btpush.c/h`，播放控制并入 `page_bluetooth.c` 子页（设备连上后显示曲目列表）
   - 方案丙：暂不接 UI，先用串口/日志手势验证推流链路通，UI 后置

4. **播放源路径**：`tab_btpush.c:51` 现扫 `/sdcard/music/*.mp3`。是否也支持 `/littlefs` 下的 MP3？（影响 R3 的排查顺序）

---

## 6. 改动文件清单（预估）

| 文件 | 动作 | 规模 |
|---|---|---|
| `main/middleware/bt_push_service.c` | **重写**为方案 B 状态机 | ~250 行 |
| `main/middleware/bt_push_service.h` | 接口调整（加 `early_init` / `set_uri` / `request_destroy`） | ~50 行 |
| `main/middleware/bt_audio_service.c` | 事件转发修正 + `play_source` 接线 | ~15 行改动 |
| `main/middleware/bt_audio_service.h` | 无需改（或加 push 状态查询） | — |
| `main/app/main.c` | 加 `bt_push_service_early_init()` 一行 | 1 行 |
| `main/CMakeLists.txt` | SRCS 注册 `bt_push_service.c` | 1 行 |
| `docs/项目规划/改动记录.md` | 追加章节 | — |
| `docs/项目规划/todolist.md` | 追加蓝牙推流条目 | — |

---

## 附：关键 API 速查（本工程 `managed_components` 实测）

```c
/* 事件数据结构 —— esp_bt_audio_event.h:82-85 */
typedef struct {
    esp_bt_audio_stream_handle_t  stream_handle;
    esp_bt_audio_stream_state_t   state;   /* 0=ALLOCATED 1=STARTED 2=STOPPED 3=RELEASED */
} esp_bt_audio_event_stream_st_t;

/* 流状态枚举 —— esp_bt_audio_stream.h:63-68 */
ESP_BT_AUDIO_STREAM_STATE_ALLOCATED
ESP_BT_AUDIO_STREAM_STATE_STARTED
ESP_BT_AUDIO_STREAM_STATE_STOPPED
ESP_BT_AUDIO_STREAM_STATE_RELEASED

/* 方向 —— esp_bt_audio_stream.h:35-39 */
ESP_BT_AUDIO_STREAM_DIR_SOURCE

/* 上下文 —— esp_bt_audio_stream.h:44-48 */
ESP_BT_AUDIO_STREAM_CONTEXT_MEDIA      /* ← local2bt 走这个 */

/* 编解码信息 —— esp_bt_audio_stream.h:119-142 */
esp_bt_audio_stream_get_codec_info(handle, &codec_info);
/* codec_info.codec_type / sample_rate / channels / codec_cfg / cfg_size */

/* 绑定 —— 组件 README:410 */
esp_gmf_io_bt_set_stream(ESP_GMF_PIPELINE_GET_OUT_INSTANCE(pipe), stream);
```

---

*本方案完全基于工程现有源码与本地 `managed_components` 实测接口编写，未引入任何未经核实的外部假设。*
