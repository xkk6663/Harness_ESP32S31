# 摄像头人脸模型加载（Flag 状态机）可执行方案

> 基于 ESP32-S31-Korvo-1 / ESP-IDF v6.1 / esp-dl HumanFaceDetect
> 参考原 CAM Flag 方案，改造为嵌入式 C 实现，适配当前 face_service 现状

> ★ **状态（2026-09-25 复核）：本文的设计已被落地，并**被后续方案扩展** —— 阅读前先看下面这条：**
> - 本文的「单模型 + 4 状态 Flag（未加载/加载中/已加载/失败）+ 异步不阻塞 LVGL + 不新增常驻任务」
>   **已实现**（状态机现值仍是这 4 个，`git log`: `feat(camera): face model load moved to cam_task`）。
> - **多档位（224 / 416 / MSR+MNP）切换 + 模型常驻不卸载 + 「检测开关」与「模型就绪」解耦**，
>   由 **`docs/开发文档/多模型加载策略_S1方案.md`** 取代本文（该文的 §3.2 明确"复用现有 4 值"）。
> - ⇒ **要改 face_service 时以 S1 方案为准**；本文保留为「为什么要有 Flag 状态机」的设计依据。
> - ⚠ 本文 §1.1 提到的 "PSRAM 11MB / 内部 RAM 70KB" 是当时的估计值；**实测口径**见
>   `docs/开发文档/内存管理规划.md` §2。

---

## 一、方案概述

### 1.1 需求背景

当前 face_service 存在以下问题：
- 按 FACE 按钮偶发 `ESP_ERR_NO_MEM` 崩溃（esp_timer_create 失败）
- 状态管理不完善：没有"加载中"锁，重复点击 FACE 会并发加载
- 失败后没有明确状态，用户不知道为什么失败
- 加载在 LVGL 回调里同步跑，阻塞 UI 139ms
- 没有重试机制，失败只能重启

引入 **Flag 状态机**，把模型加载的四种状态（未加载/加载中/已加载/失败）显式管理，杜绝重复加载、并发崩溃、失败无提示。

### 1.2 核心目标

- 用 Flag 精准管控模型全生命周期，杜绝重复加载和并发崩溃
- 加载失败时明确提示原因（内存不足/文件缺失），UI 显示状态
- 加载过程不阻塞 LVGL（异步加载，完成后回调 UI）
- 失败后支持重试，不需要重启设备
- 兼容当前 PSRAM 11MB / 内部 RAM 70KB 的资源约束
- **不新增常驻任务**（遵守之前的硬规范）

### 1.3 适用范围

- ESP32-S31-Korvo-1 板
- esp-dl HumanFaceDetect (espdet_pico_224_224_face.espdl)
- 模型文件在 LittleFS `/littlefs/models/`
- 摄像头 tab 的 FACE 按钮

---

## 二、核心设计（Flag 状态机）

### 2.1 状态 Flag 定义

```c
typedef enum {
    FACE_UNLOADED   = 0,   /* 未加载，初始状态 */
    FACE_LOADING    = 1,   /* 加载中，锁定，禁止重复触发 */
    FACE_LOADED     = 2,   /* 加载成功，可推理 */
    FACE_FAIL       = -1,  /* 加载失败（NO_MEM/文件缺失） */
} face_state_t;
```

| Flag 值 | 状态 | UI 表现 | 业务行为 |
|---|---|---|---|
| 0 UNLOADED | 未加载 | 按钮青色 "FACE" | 点击触发加载 |
| 1 LOADING | 加载中 | 按钮黄色 "LOADING..." | 拒绝重复点击，不并发 |
| 2 LOADED | 已加载 | 按钮绿色 "FACE ON" | 直接推理，可停止卸载 |
| -1 FAIL | 加载失败 | 按钮红色 "FAIL" | 点击重试，显示错误原因 |

### 2.2 状态流转

```
0 UNLOADED ──点击 FACE──> 1 LOADING
1 LOADING ──加载成功──> 2 LOADED
1 LOADING ──加载失败──> -1 FAIL
-1 FAIL  ──重试──> 1 LOADING
2 LOADED ──再点 FACE──> 0 UNLOADED（卸载模型，释放内存）
```

**关键约束**：
- LOADING 状态拒绝任何重复触发（按钮回调直接 return）
- 加载完成/失败后通过 `lv_async_call` 回到 LVGL 线程更新 UI
- 卸载时 delete detector，释放内部 RAM

---

## 三、整体执行流程

### 3.1 加载流程（异步，不阻塞 LVGL）

```
用户点 FACE 按钮（LVGL 线程）
  ├─ 当前状态 != UNLOADED/FAIL → 直接 return
  ├─ 状态置 LOADING，按钮变黄 "LOADING..."
  └─ lv_async_call(load_async, NULL)   ← 不阻塞 LVGL
        │
        ▼ (LVGL 线程下一轮 idle 时执行)
  load_async():
    ├─ 检查 LittleFS 模型文件存在
    ├─ new HumanFaceDetect()   ← 这一步偶发 NO_MEM
    ├─ 分配 scratch buffer (PSRAM)
    ├─ 成功 → 状态=LOADED，按钮变绿
    └─ 失败 → 状态=FAIL，按钮变红，记录错误码
```

**为什么用 `lv_async_call` 而不是新任务**：
- `lv_async_call` 在 LVGL 线程 idle 时执行，不新增任务栈
- 加载过程中 LVGL 能处理其他事件（触摸、绘制）
- 加载完直接更新 UI，不需要跨线程通信

### 3.2 推理流程（复用现有 refresh timer）

```
refresh timer (50ms):
  ├─ 状态 != LOADED → 跳过推理
  ├─ 每 3 帧执行一次 detect
  ├─ 结果存全局 face_box_t
  └─ 下一帧画框（在 LVGL 锁里只画框，不做 detect）
```

### 3.3 卸载流程

```
用户点 FACE 按钮（LOADED 状态）
  ├─ 停止推理（状态=UNLOADED）
  ├─ delete HumanFaceDetect()
  ├─ 释放 scratch buffer
  └─ 按钮变青 "FACE"
```

---

## 四、落地代码改造

### 4.1 face_service.h 改造

```c
typedef enum {
    FACE_UNLOADED = 0,
    FACE_LOADING  = 1,
    FACE_LOADED   = 2,
    FACE_FAIL     = -1,
} face_state_t;

/* 启动异步加载（不阻塞调用者） */
esp_err_t face_service_start_async(void);

/* 卸载模型 */
esp_err_t face_service_stop(void);

/* 查询当前状态（UI 用） */
face_state_t face_service_state(void);

/* 获取最后一次失败原因（UI 显示用） */
const char *face_service_last_error(void);

/* 推理（现有接口不变） */
int face_service_detect(const uint16_t *rgb565le, uint16_t w, uint16_t h,
                        face_box_t *out, int max);
```

### 4.2 face_service.c 关键实现

```c
static face_state_t s_state = FACE_UNLOADED;
static const char *s_error = "";
static HumanFaceDetect *s_detector = nullptr;
static uint8_t *s_scratch = nullptr;

/* 异步加载：在 LVGL idle 线程执行 */
static void load_async_cb(void *arg)
{
    (void)arg;
    s_state = FACE_LOADING;
    s_error = "";

    /* 检查模型文件 */
    struct stat st;
    if (stat("/littlefs/models/espdet_pico_224_224_face.espdl", &st) != 0) {
        s_state = FACE_FAIL;
        s_error = "model file not found";
        ESP_LOGE(TAG, "%s", s_error);
        return;
    }

    /* 加载模型（这一步可能 NO_MEM） */
    s_detector = new HumanFaceDetect(HumanFaceDetect::ESPDET_PICO_224_224_FACE, false);
    if (!s_detector) {
        s_state = FACE_FAIL;
        s_error = "detector alloc fail (NO_MEM)";
        ESP_LOGE(TAG, "%s", s_error);
        return;
    }

    /* 分配 scratch (PSRAM，不占内部 RAM) */
    s_scratch = (uint8_t *)heap_caps_aligned_alloc(64, 240*240*3,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
    if (!s_scratch) {
        delete s_detector; s_detector = nullptr;
        s_state = FACE_FAIL;
        s_error = "scratch alloc fail";
        return;
    }

    s_state = FACE_LOADED;
    ESP_LOGI(TAG, "model loaded ok");
}

esp_err_t face_service_start_async(void)
{
    if (s_state == FACE_LOADING) return ESP_ERR_INVALID_STATE;  /* 拒绝重复 */
    if (s_state == FACE_LOADED)  return ESP_OK;

    /* 异步调度，不阻塞 LVGL */
    lv_async_call(load_async_cb, NULL);
    return ESP_OK;
}

face_state_t face_service_state(void) { return s_state; }
const char *face_service_last_error(void) { return s_error; }
```

### 4.3 tab_camera.c 按钮回调改造

```c
static void face_btn_cb(lv_event_t *e)
{
    face_state_t st = face_service_state();

    if (st == FACE_LOADING) {
        lv_label_set_text_static(s_btn_face_label, "LOADING...");
        return;  /* 加载中，拒绝重复 */
    }

    if (st == FACE_LOADED) {
        /* 卸载 */
        face_service_stop();
        lv_label_set_text_static(s_btn_face_label, "FACE");
        lv_obj_set_style_bg_color(s_btn_face, COL_CYAN, 0);
        return;
    }

    /* UNLOADED 或 FAIL → 启动异步加载 */
    lv_label_set_text_static(s_btn_face_label, "LOADING...");
    lv_obj_set_style_bg_color(s_btn_face, lv_color_hex(0xFFFF00), 0);  /* 黄色 */
    face_service_start_async();

    /* 200ms 后检查状态，更新按钮颜色 */
    lv_timer_t *check = lv_timer_create(check_state_cb, 200, NULL);
    lv_timer_set_repeat_count(check, 1);
}

/* 异步轮询加载结果 */
static void check_state_cb(lv_timer_t *t)
{
    face_state_t st = face_service_state();
    if (st == FACE_LOADED) {
        lv_label_set_text_static(s_btn_face_label, "FACE ON");
        lv_obj_set_style_bg_color(s_btn_face, lv_color_hex(0x00FF00), 0);
    } else if (st == FACE_FAIL) {
        lv_label_set_text_static(s_btn_face_label, "FAIL");
        lv_obj_set_style_bg_color(s_btn_face, lv_color_hex(0xFF0000), 0);
        lv_label_set_text_static(s_status, face_service_last_error());
    }
    lv_timer_del(t);
}
```

---

## 五、遵守的硬规范（之前踩坑总结）

| 规范 | 本方案如何遵守 |
|---|---|
| 不新增常驻任务 | 用 `lv_async_call`，不新起 FreeRTOS 任务 |
| 不停 WiFi | 加载前后不碰 WiFi |
| 不改堆布局 | scratch 还是 PSRAM，canvas buf 不动 |
| 不绑新核 | 所有操作在 LVGL 线程 |
| 栈空间不增加 | 没有新任务栈 |
| 模型加载失败有提示 | UI 显示红色 + 错误原因 |
| 重复点击有锁 | LOADING 状态拒绝重复触发 |

---

## 六、测试验证方案

### 6.1 基础功能测试

1. **初始状态**：开机后按钮青色 "FACE"，状态=UNLOADED
2. **正常加载**：点击按钮 → 黄色 "LOADING..." → 2 秒后绿色 "FACE ON"
3. **重复点击**：LOADING 状态再点按钮，无反应，不并发
4. **卸载**：LOADED 状态再点按钮 → 青色 "FACE"，模型释放
5. **失败场景**：如果 NO_MEM，按钮变红 "FAIL"，status 显示错误原因

### 6.2 并发与稳定性测试

1. 快速连续点 FACE 按钮 5 次，验证只加载一次
2. LOADED 状态下点 START 开关摄像头，验证不崩溃
3. 加载失败后重试 3 次，验证每次都能重新进入 LOADING
4. 推理过程中切到其他 tab 再切回，验证状态不丢

### 6.3 内存验证

1. 加载前打印 `free internal / free PSRAM`
2. 加载后再打印，确认 scratch 从 PSRAM 分配
3. 卸载后再打印，确认内存释放
4. 多次加载/卸载循环 10 次，验证无内存泄漏

---

## 七、异常处理

| 异常 | Flag | UI 表现 | 对策 |
|---|---|---|---|
| 模型文件不存在 | FAIL | 红 "FAIL" | status 显示 "file not found"，检查 LittleFS |
| detector new 失败 (NO_MEM) | FAIL | 红 "FAIL" | status 显示 "NO_MEM"，关摄像头后重试 |
| scratch 分配失败 | FAIL | 红 "FAIL" | status 显示 "scratch OOM" |
| 加载中重复点击 | — | 忽略 | 按钮保持 LOADING |
| 推理时模型被卸载 | — | 跳过 detect | 状态检查 |

---

## 八、风险与对策

| 风险 | 对策 |
|---|---|
| `lv_async_call` 仍在 LVGL 线程，加载 900ms 还是卡 UI | 可接受（之前同步 new 也是卡的），至少比之前好 |
| NO_MEM 仍然偶发 | 这是硬件资源问题，Flag 方案只保证不崩溃、有提示 |
| 状态不同步 | 所有状态变更在 LVGL 线程，无需加锁 |

---

## 九、改动文件清单

| 文件 | 改动 |
|---|---|
| `main/middleware/camera/face_service.h` | 新增 state 枚举、async 接口、last_error |
| `main/middleware/camera/face_service.cpp` | 重写 start/stop，加 Flag 状态机 + async |
| `main/ui/tab_camera.c` | face_btn_cb 改造，按状态变色，异步轮询 |

不改动：camera_service.c、color_hsv.c、sdkconfig、分区表。

---

## 十、预期效果

1. **不再崩溃**：LOADING 锁杜绝并发加载，失败有提示不 abort
2. **UI 可见**：按钮四色状态（青/黄/绿/红），用户知道当前状态
3. **可重试**：FAIL 后再点就能重试，不需要重启
4. **资源可控**：卸载时释放内存，多次开关不泄漏
5. **零侵入**：只改 face_service 和按钮回调，不动其他模块
