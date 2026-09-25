# FACE 加载 NO_MEM 问题优化方案

> ⚠ **2026-09-25 更正（读代码后复核，勿再照本文档的"推荐组合"执行）**
>
> 本文档写于 2026-09-18，是基于**猜测**写的。逐行核对组件源码后，**方案 A 已不存在可做的对象**：
> 本工程 `CONFIG_BSP_DISPLAY_LVGL_AVOID_TEAR=y`，LVGL 画布**就是** PSRAM 里的 RGB framebuffer
> （`esp_lvgl_port_disp.c:361-369` 直接取 `esp_lcd_rgb_panel_get_frame_buffer()`），
> 根本没有"内部 RAM 里的 ~150KB draw buffer"可挪。**方案 D 也不存在**（esp-dl 3.3 无该 Kconfig，
> 且模型张量本来就在 PSRAM——见 §3.3）。
>
> 真正的根因链、可执行改动与验证方法，**以 `docs/开发文档/内存优化可执行方案.md`（2026-09-25）为准**。
> 本文档保留作历史记录。

## 问题根因

模型加载时 `new HumanFaceDetect()` 在独立任务（8KB 栈）里执行，但加载过程中：
1. esp-dl 内部调 `esp_timer_create` 分配内部 RAM
2. 同时 WiFi 心跳反复 OFFLINE/ONLINE，WiFi PHY 重新初始化也要 `esp_timer_create`
3. 两个竞争内部堆，碎片化时 esp_timer 返回 ESP_ERR_NO_MEM → abort

之前直接 `esp_wifi_stop()` 导致 wifi_manager 自动重连，在 STOP 状态下调 `esp_wifi_set_config` → 另一个 abort。

## 优化方案（按推荐顺序）

### 方案 A：LVGL draw buffer 挪到 PSRAM（推荐，根治）

**原理**：LVGL 主 draw buffer 默认在内部 RAM，占 ~150KB。挪到 PSRAM 后，内部 RAM 富余 150KB+，WiFi PHY 和模型加载都够用。

**改动**：
- `lvgl_port` 或 display 初始化时，draw buffer 用 `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`
- sdkconfig 里 `CONFIG_LV_MEM_CUSTOM_ALLOC` 或 `CONFIG_SPIRAM_USE_MALLOC`

**收益**：内部 RAM free 从 73KB → ~200KB，彻底解决 NO_MEM
**风险**：PSRAM 带宽比内部 RAM 慢，LVGL 刷屏可能略卡（但有 DMA cache 对齐，实际无感）

### 方案 B：wifi_manager 加"暂停重连"标志

**原理**：模型加载期间，告诉 wifi_manager 不要自动重连，加载完再恢复。

**改动**：
- `wifi_manager.h` 加 `wifi_manager_pause_reconnect(bool pause)`
- FACE 加载前 pause，加载完 resume
- 不调 `esp_wifi_stop()`，只是阻止重连逻辑

**收益**：避免 WiFi 和模型加载竞争
**风险**：WiFi 暂时断开 ~1 秒（模型加载时间），语音/心跳会掉一下

### 方案 C：模型加载只做一次（lazy once）

**原理**：现在 FACE 按一下开、再按一下关。改成：第一次按 FACE 时加载模型（停 WiFi），之后开/关只是切换检测开关，不卸载模型。

**改动**：
- `face_service_start()` 只加载一次，第二次调用直接返回 ESP_OK
- `face_service_stop()` 只停检测，不 `delete s_detector`
- 退出 CAM tab 或关机时才真正卸载

**收益**：避免每次开 FACE 都重新加载模型（~900ms 停 WiFi）
**风险**：模型常驻内存（PSRAM ~500KB，可接受）

### 方案 D：把 esp-dl 推理激活放 PSRAM

**原理**：esp-dl 推理临时 tensor 从内部 RAM 挪到 PSRAM。

**改动**：
- sdkconfig `CONFIG_ESP_DL_USE_PSRAM=y`（如果组件支持）

**收益**：释放内部 RAM
**风险**：推理从 139ms 涨到 300-500ms，画面卡。**不推荐**

## 推荐组合

**A + C**：
1. LVGL draw buffer 挪 PSRAM（根治内部 RAM 不足）
2. 模型只加载一次（避免反复停 WiFi）
3. 不调 esp_wifi_stop，靠 A 释放的内部 RAM 就够

这样 FACE 按钮第一次按加载模型（~900ms，WiFi 不断），之后开/关秒切，不再有 NO_MEM。

## 待确认
- [ ] 选 A+C 方案？
- [ ] 还是只做 C（模型只加载一次，先看还崩不崩）？
