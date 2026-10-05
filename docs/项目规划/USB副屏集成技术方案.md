# USB 副屏（Windows 扩展屏）集成技术方案

> 版本 **V1.4**（2026-10-05，实施收尾：画质定稿 JPEG 9 / 450KB、预编译烧录包入库 `firmware/uext_screen/`、README 重写）· 分支：**main**
> 例程：`esp-iot-solution/examples/usb/device/usb_extend_screen`（已 sparse-clone 到 `_extern/esp-iot-solution/`）
> 需求：**SET 硬件按键一键进入 Windows 副屏模式（杀死当前项目功能、不影响副屏性能）；
> 再按 SET 恢复当前 LVGL 界面与功能；百分百功能分离、互不影响。**
>
> **已拍板决策（用户 2026-10-05 原话）**：
> **"A 双固件切槽，单击 + 屏幕倒计时确认，走正常 splash，uext 功能全开，uext 首次刷入方式用USB"**
> → D1=方案A双固件切槽；D2=单击+屏幕倒计时确认；D3=走正常 splash；D4=uext 功能全开（PID 0x2986）；D5=USB 首次刷入。
> 授权：更新完文档后直接开工，只在重大问题/决策时停下来询问。
>
> ★ **V1.1 关键修正（相对 V1）**：uext 不再共用 ota_1，改用专属子类型槽
>   （V1 的"uext 放 ota_1"与 main 的 OTA A/B 轮替冲突：首次 OTA 升级后 main 跑到 ota_1、
>   uext 被覆盖、SET 切 ota_1 反而切回 main——结构性缺陷，见 §6.4）。
>
> ★★ **V1.2 关键修正（相对 V1.1，M0 编译期静态查证发现）**：V1.1 选的 `ota_15` 子类型**无法作为
>   启动目标**——`esp_ota_get_app_partition_count()`（esp_ota_ops.c）只**连续**数 OTA 子类型
>   （ota_0, ota_1, ota_15 → app_count=2）；`esp_rewrite_ota_data()` 对
>   `OTA_SLOT(subtype) >= ota_app_count` 直接返回 `ESP_ERR_INVALID_ARG`
>   ⇒ `esp_ota_set_boot_partition(ota_15)` 必然失败；bootloader 选槽
>   `(ota_seq-1) % app_count`（bootloader_utility.c:362，紧凑序号）与槽存储下标
>   `subtype & 0x0F`（:192，ota_15→15）两套索引对非连续表也对不上。
>   **修正：uext 槽改用 `factory` 子类型**（偏移 0xA20000 不变）——`set_boot(factory)` 的 IDF
>   官方语义 = 擦 otadata → 启动 factory 槽；且 `esp_ota_get_next_update_partition` 永远不会
>   选中 factory，main 的 OTA 机制零改动（§6.4 选槽补丁降级为第二道防线）。

---

## 0. 结论速览（先看这里）

| 项 | 结论 |
|---|---|
| **可行性** | ✅ 高。例程**官方支持 ESP32-S31-Korvo-1**：800×480@**60FPS** + 五点触摸 + UAC 音频（48kHz），与我们板子同一块 BSP |
| **推荐架构** | **方案 A：双固件 + OTA 双槽切换**（SET → 切槽重启进副屏固件；再按 SET → 切回 ota_0 重启）。百分百分离是**结构性保证** |
| **备选架构** | 方案 B：单固件热切换（同 app 内杀任务/卸 LVGL/启 TinyUSB）。切换快（~1s）但工程量大、风险高，作为 V2 演进路径保留 |
| **改动量** | 方案 A：main 侧 ~100 行 + 新建 uext 独立工程（例程 95% 零改动）；方案 B：为 ~10 个服务补 deinit + LVGL 拆卸重建（未验证路径） |
| **切换延迟** | 方案 A：~3-5s（重启）；方案 B：~1s |
| **关键前提（已核实）** | ① SET 键完全空闲 ② USB OTG HS PHY 完全空闲（console 走独立的 USB-Serial-JTAG，不冲突）③ 新增 `factory` 子类型专属槽放 uext（OTA 机制永不触碰 factory，见 §3.1/§6.4） ④ `SOC_JPEG_DECODE_SUPPORTED=y`（硬件 JPEG，60FPS 的基础） |

### 0.1 决策锁定（D1~D5，均已拍板）

| # | 决策 | 拍板结果 |
|---|---|---|
| D1 | 总体架构 | **方案 A：双固件切槽**（uext 驻留专属 `factory` 子类型槽 @0xA20000） |
| D2 | SET 防误触 | **单击 → 屏幕 2s 倒计时确认卡 → 再次按 SET 才切**；超时自动取消 |
| D3 | 切回 main 的启动路径 | **走正常 splash**（上滑进主界面，最简单；不做 nvs 跳过标志） |
| D4 | uext 音频/触摸 | **全开**：PID 0x2986（屏+五点触摸+UAC 48kHz） |
| D5 | uext 首次刷入 | **USB**（`esptool write_flash 0xA20000`，见 §4.5）；后续升级也走 USB |

---

## 1. 例程源码研读结论

### 1.1 官方对 S31-Korvo-1 的支持矩阵

| 特性 | S31-Korvo-1 |
|---|---|
| 分辨率/帧率 | 800×480 @ **60FPS**（S3 只有 13FPS，P4 1024×576@60） |
| 触摸 | GT1151 五点 → HID 多点上报给 Windows |
| 音频 | UAC 声卡，扬声器+麦克风，**48kHz** |
| USB | TinyUSB **HS**（UTMI 高速 PHY，OTG 2.0） |
| PC 端 | Windows IDD 间接显示驱动，**官方提供已签名安装包**（免测试模式） |

### 1.2 数据通路（读源码还原）

```
Windows IDD 驱动 ──(USB HS, Vendor bulk, JPEG 逐帧)──▶ ESP32-S31
   ▲                                                    │
   │ HID 多点触摸(触摸屏当扩展屏用)                        │ tud_vendor_rx_cb()  app_vendor.c:89
   │ UAC 音频(Windows 声音走设备扬声器/麦克风)              ▼
   └──────────────────────────────────── frame 池(6×450KB PSRAM≈2.7MB, usb_frame.c:24)
                                                        │ transfer_task() 取满帧
                                                        ▼
                              jpeg_decoder_process() 硬件解码 → 直写 RGB 面板帧缓冲
                              （app_lcd_s31.c:54-61，★ 全程无 LVGL）
```

**核心事实：副屏路径完全不用 LVGL** —— TinyUSB 收 JPEG → 硬件解码 → `esp_lcd_panel_draw_bitmap()` 直写 RGB 面板（app_lcd_s31.c:61）。这是"分离"能做干净的根源。

### 1.3 例程资源占用（S31 配置，Kconfig.projbuild）

| 资源 | 数值 | 出处 |
|---|---|---|
| JPEG 帧池 | `frame_allocate(6, 300000)` ≈ **1.8MB PSRAM**（`jpeg_alloc_decoder_mem`） | usb_frame.c:24、app_vendor.c:43 |
| LCD 帧缓冲 | 2 × 800×480×2B = **1.5MB PSRAM**（`fb_in_psram`） | esp32_s31_korvo_1.c:338 |
| Vendor RX 缓冲 | 512B × 10 = 5KB（`CFG_TUD_VENDOR_RX_BUFSIZE`） | tusb_config.h:121 |
| 任务 | tusb_device_task(4KB, prio6) / transfer_task(4KB, prio5, core1) / touch_task / UAC 内部任务 | app_usb.c:84、app_vendor.c:182 |
| USB 描述符 | VID 0x303A，PID **0x2986**（复合：屏+触摸+音频）/ 0x2987（纯屏） | Kconfig.projbuild:8-11 |

### 1.4 PC 端

- 官方已签名驱动：`xfz1986_usb_graphic_250224_rc_sign.exe`（windows_driver/README_cn.md），双击安装即可，**不需要**测试模式；
- 设备管理器认 `USB\VID_303A&PID_2986`（复合设备）；
- Windows 侧把这块屏当扩展显示器：副屏画面 + 触摸 + 扬声器/麦克风一体。

---

## 2. 当前工程摸底结论（逐项已核实，附证据）

| # | 事实 | 证据 |
|---|---|---|
| 1 | 分区表：`ota_0` 5MB + `ota_1` **5MB（空闲）** + storage 5.875MB，16MB flash | `partitions.csv` |
| 2 | **SET 键完全空闲**：`BTN_ID_SET` 全工程只有 button_service.c:44 的名字映射，无任何订阅 | `grep -rn BTN_ID_SET main/` |
| 3 | 按键服务现成：ADC 分压 4 键（set/mode/vol-/vol+），事件回调 `button_service_set_evt_cb()`，**不依赖 LVGL** | button_service.h:87 |
| 4 | **USB OTG + HS PHY 空闲**：`SOC_USB_OTG_SUPPORTED=y`、`USB_UTMI_PHY_NUM=1`；main/ 无任何 usb_host 调用（espressif__usb/usb_host_uvc 组件是 BSP 依赖带入的，未使用） | sdkconfig:56/194/198、`grep usb_host main/` 为空 |
| 5 | console 走 **USB-Serial-JTAG**（独立外设），与 OTG 不冲突 | sdkconfig:2880 |
| 6 | 硬件 JPEG 解码可用：`SOC_JPEG_DECODE_SUPPORTED=y`（60FPS 的前提） | sdkconfig:824 |
| 7 | 工程与例程用**同一个 BSP 组件**（`espressif__esp32_s31_korvo_1`），且 BSP 原生提供三套显示 API：`bsp_display_start_with_config()`（LVGL 全栈，现用）/ `bsp_display_new()`（裸面板，例程用）/ `bsp_display_delete()`（拆面板） | bsp_display.c:63/134/293 |
| 8 | LVGL port 有完整拆卸 API：`lvgl_port_remove_touch()` / `lvgl_port_remove_disp()` / `lvgl_port_stop()` / `lvgl_port_resume()` / `lvgl_port_deinit()` | esp_lvgl_port.h:98-143、esp_lvgl_port_disp.h:139 |
| 9 | taskLVGL = **16KB 内部 RAM 栈**（§28.31 提的）+ LVGL 绘制缓冲（`buff_dma=true` → 内部 RAM）；卸掉它们 = 内部 RAM 大头回归 | main.c:215、ESP_LVGL_PORT_INIT_CONFIG task_stack_caps=INTERNAL |
| 10 | RGB 面板当前为 **bounce-buffer 模式**（`CONFIG_BSP_LCD_RGB_REFRESH_AUTO is not set` → `RGB_BOUNCE_BUFFER_MODE` 生效），帧缓冲 2 块已常驻 PSRAM | sdkconfig:4814/4818 |
| 11 | WiFi 那笔 51,200B 内部 DMA、BT controller 大块内部内存 —— 只有"杀掉 WiFi/BT"才能释放（方案 B 的收益，方案 A 天然获得） | MEMORY §2 |
| 12 | 例程要求的 IDF ≥5.3，工程是 v6.1 —— 满足，但**例程未在 v6.1 验证过**（风险 §8-R1） | main/idf_component.yml |

---

## 3. 架构方案对比

### 方案 A：双固件 + OTA 双槽切换（推荐）

```
        ┌─────────── ota_0 (5MB) ───────────┐   ┌─────────── ota_1 (5MB) ───────────┐
        │  main 固件（现工程，一切功能）        │   │  uext 固件（例程改造，只做副屏）      │
        │  LVGL UI / 音频 / 蓝牙 / 副屏推流    │   │  TinyUSB + 硬件JPEG + UAC + HID    │
        └───────────────┬───────────────────┘   └───────────────┬───────────────────┘
                        │      SET 单击                           ▲ │
                        ├──── esp_ota_set_boot_partition(ota_1) ──┘ │
                        │      + esp_restart（重启，~3-5s）           │ SET 单击
                        ◀──────── esp_ota_set_boot_partition(ota_0) ─┘
```

- **两个独立 app**：uext 运行时，main 的任务/驱动/内存**根本不存在**——分离是结构性的，不靠"记得杀干净"；
- 例程保持官方形态（60FPS 路径零干扰），只加"SET 切回"一个功能；
- 复用我们刚验收完的 OTA 设施（`esp_ota_set_boot_partition` + otadata，机制与 §61 同源，但**不占 OTA 传输流程**——uext 首次刷入用 USB）。

### 方案 B：单固件热切换（同 app 内杀/启）

SET 按下 → 在运行中的 main 固件里：卸触摸 → 卸 LVGL 显示（`lvgl_port_remove_disp` 释放绘制缓冲）→ `lvgl_port_deinit`（杀 taskLVGL 释放 16KB 内部栈）→ 逐个停音频/蓝牙/网络服务（释放 I2S/codec/JPEG 引擎/WiFi 51KB DMA/BT controller 内存）→ 初始化 TinyUSB + 帧池 → 副屏跑起来。
再按 SET → 反向恢复 → `lvgl_port_init` + `add_disp_rgb` + `add_touch` → PageManager 重建 UI。

### 决策表

| 维度 | 方案 A（双固件切槽） | 方案 B（单固件热切换） |
|---|---|---|
| **功能分离度** | ★★★★★ 结构性 100%（两 app 互斥驻留） | ★★★☆☆ 靠纪律：共享面板/I2C/I2S/JPEG 引擎/堆，杀不干净就互相拖累 |
| **副屏性能** | 60FPS 无干扰（无任何后台任务） | 受残留任务/碎片影响，需逐项验证 |
| **切换延迟** | 重启 ~3-5s | ~1s |
| **恢复语义** | UI **全量重建**（回主界面，播放/连接状态丢失，蓝牙重连需几秒） | UI 全量重建 + 各服务重启（状态同样丢，音乐/蓝牙也要重连） |
| **工程量** | **小**：uext 独立工程（例程 95% 原样）+ main 侧 ~100 行 | **大**：~10 个服务补 deinit/重启 API + LVGL 拆卸重建（本工程未验证） + 大量联调 |
| **主要风险** | 每次切换都是一次重启；ota_1 被两个用途共用（§6.4 约定）；例程在 IDF v6.1 的适配 | LVGL remove/deinit→re-init 路径未验证；服务重启顺序坑（F40 cache 窗口、audio I2S 仲裁、BT 重启稳定性）；一次泄漏每次切换都累积 |
| **与用户偏好契合度** | 保守简单、体验可靠（宁要 4s 确定性切换，不要 1s 但可能黑屏的切换） | 激进 |

> ★ 注意"恢复语义"：**两个方案都做不到"UI 状态保持"**——副屏模式期间 LVGL 界面都必须重建（方案 B 甚至连音频/蓝牙都要重连）。"恢复当前 lvgl 的界面以及功能"落实为"恢复到功能完整的主界面"。这是百分百分离的必然代价，请审阅时确认接受。

**已拍板：V1 落地方案 A；方案 B 作为 V2 演进**（若 3-5s 切换延迟实测不可接受再做，且 B 的服务 deinit 工作也可反哺长稳性）。

---

## 3.1 ★ 分区表修订（V1.2 = 最终实施基线）

main 的 OTA 是标准 A/B 轮替（每次升级写对侧槽并轮换运行槽）。uext 用**专属 `factory` 子类型槽**
（OTA 机制永不触碰 factory；V1.1 的 ota_15 方案被 IDF 源码证伪，见文档头部 V1.2 修正说明），
storage 从 5.875MB 缩到 3MB（实测内容 2,875,145 B ≈ 2.74MB，可容纳但紧）：

```
# Name,   Type, SubType, Offset,   Size,     Flags
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
otadata,  data, ota,     0x11000,  0x2000,
ota_0,    app,  ota_0,   0x20000,  0x500000,     # main 槽 A（5MB，不变）
ota_1,    app,  ota_1,   0x520000, 0x500000,     # main 槽 B（5MB，不变）
uext,     app,  factory, 0xA20000, 0x2E0000,     # ★ uext 专属槽（2.875MB，factory 子类型）
storage,  data, littlefs, 0xD00000, 0x300000,    # ★ 缩容 5.875MB → 3MB
```

- nvs / phy / otadata / ota_0 / ota_1 的偏移与尺寸**全部不变**（bootloader/OTA/NVS 无感）；
- 切换语义（IDF 官方机制，无自造轮子）：
  **main→uext = `esp_ota_set_boot_partition(factory)` = 擦 otadata** → 下次启动 factory 槽；
  **uext→main = `esp_ota_set_boot_partition(ota_0)`** → 正常写 otadata；
- **storage 变了**：改分区后 littlefs 元数据偏移失效，必须重跑 `idf.py storage-flash`
  （现 storage 内容本来就要重建——SD 卡损坏后 littlefs_content 需重新导入）；
- uext 工程的 `partitions.csv` 与 main 保持**同一份内容**（防误刷出两套表）。

---

## 4. 方案 A 详细设计（V1）

### 4.1 固件组织

```
display_audio_photo/                 # main 固件（现有工程，不动结构）
uext_screen/                         # ★ 独立 IDF 工程（副屏固件源码），
                                     #   实际位于 Desktop/esp32/uext_screen/（本仓库外）；
                                     #   预编译烧录包已入库 display_audio_photo/firmware/uext_screen/
                                     #   （uext_screen.bin + Windows 签名驱动 + 烧录说明）
├── CMakeLists.txt / sdkconfig.defaults / partitions.csv（同 main）
├── main/                            # ← 例程 main/ 拷入，加 app_switch.c（SET 切回）
├── common_components/esp32_s31_korvo_1/   # ← 例程 override BSP 拷入（无 LVGL/无 bb_mode，官方 60FPS 形态）
└── idf_component.yml                # tinyusb / usb_device_uac / gt1151 从 registry 拉
```

- uext 的 `sdkconfig.defaults`：关 ROLLBACK（专用固件不需要防砖）、PID=0x2986（屏+触摸+音频）、800×480、60FPS、JPEG quality **9**、frame limit **450KB**（【V1.4】画质定稿，落屏仍 RGB565——16-bit 屏总线无 RGB888，JPEG 质量与帧上限是仅有的画质旋钮）、UAC 48kHz；
- 例程源码改动点只有 3 处（见 4.3）。

### 4.1a 离线 vendor 组件清单（registry 403 → 全部本地化）

uext 工程 `components/` 目录（全部本地组件，component manager 零联网）：

| 组件目录 | 来源 | 必要改动 |
|---|---|---|
| `esp32_s31_korvo_1/` | 例程 `common_components/esp32_s31_korvo_1`（override BSP：无 LVGL、无 bb_mode，官方 60FPS 形态） | yml 只留 `idf>=5.5`；CMakeLists REQUIRES 补 `esp_codec_dev esp_lcd_touch esp_lcd_touch_gt1151` |
| `tinyusb/` | GitHub `espressif/tinyusb` tag **v0.19.0.3**（对应 registry 0.19.0~3） | 无 |
| `usb_device_uac/` | esp-iot-solution `components/usb/usb_device_uac` v1.3.1 | yml 去掉 `espressif/tinyusb` 与 `cmake_utilities` 依赖；CMakeLists 去掉 `include(package_manager)`/`cu_pkg_define_version` 两行 |
| `esp_codec_dev/` | main `managed_components/espressif__esp_codec_dev` v1.5.11（例程 BSP 的 UAC 音频真的走它） | 目录改名去前缀；yml 只留 idf 依赖 |
| `esp_lcd_touch/` | main `managed_components/espressif__esp_lcd_touch` v1.2.1 | 同上 |
| `esp_lcd_touch_gt1151/` | main `managed_components/espressif__esp_lcd_touch_gt1151` v1.1.1 | 同上（yml 的 esp_lcd_touch 公共依赖由本地同名组件满足，不联网） |
| `button/` | main `managed_components/espressif__button` v4.2.1（app_switch 的 ADC 按键用） | 去 cmake_utilities 依赖 + 剥 CMakeLists 的 package_manager 两行 |

- 关键事实（已核实源码）：iot_button `button_adc.c:312` 在 `adc_handle=NULL` 时**自建 ADC 单元**
  并自配通道衰减（`ADC_BUTTON_ATTEN`=11/12dB）⇒ uext 的 app_switch 不需要复刻 BSP 的
  `bsp_adc_initialize()`；SET 键窗口沿用 main 实测真值（ADC1_CH0，min=180/max=360，按下≈260mV）。
- `usb_device_uac` 保持 `CONFIG_USB_DEVICE_UAC_AS_PART=y`：其 tusb 描述符由**例程 main/CMakeLists**
  注入 tinyusb 库（`${UAC_PATH}/tusb_uac`），组件自身不做注入。

### 4.2 切换时序

**main → uext（SET 单击 + 倒计时确认，D2）**

```
[iot_button 任务] SET 单击回调
  1. 只置 volatile 标志（F44 线程纪律：回调不碰 LVGL）
  2. taskLVGL 的 lv_timer 消费标志 → 弹出确认卡（"再次按 SET 进入副屏"，2s 倒计时进度条）
  3a. 2s 内再次 SET 单击 → esp_ota_set_boot_partition(uext) + esp_restart()
  3b. 2s 超时 → 自动收卡，无事发生
```

防护：OTA 升级进行中（`ota_service_is_active()`）不接受切换请求。

**uext 启动（走 D3：main 回来时走正常 splash，无 nvs 标志）**

```
app_main: app_switch_init()（SET 键订阅）→ app_usb_init → app_lcd_init → app_touch_init
  → 插着 USB 线，Windows 装过驱动的话 2s 内出副屏
  → esp_ota_mark_app_valid_cancel_rollback()（幂等；main 的 bootloader 开了 ROLLBACK，
    原始烧写的镜像若被置为 PENDING_VERIFY，必须立刻转正，否则被回滚）
```

**uext → main（SET 单击，在 uext 固件里）**

```
[iot_button 任务] SET 单击回调
  1. esp_ota_set_boot_partition(ota_0)   ← uext 侧直接切，无倒计时（回来的是自己家）
  2. esp_restart()
```

main 重启后走正常启动流程（splash → 上滑 → 主界面，D3 拍板：不做跳过标志）。

### 4.3 uext 工程改造清单（在例程基础上的全部改动）

| # | 改动 | 规模 |
|---|---|---|
| 1 | 新增 `app_switch.c`：拷 main 线 `button_service.c`（ADC 4 键、无 LVGL 依赖），订阅 SET 单击 → 切回 ota_0 重启 | ~80 行 |
| 2 | `app_main()` 里加 `app_switch_init()`（**放最前**，保证副屏没起来时也能切回） | 1 行 |
| 3 | `app_uac.c` 的音频回调改走例程 BSP 的 `bsp_extra_i2s_*`（S31 override BSP 已带，无需改）——若 registry 版 `usb_device_uac` 接口有出入则适配 | 0~30 行 |
| 4 | sdkconfig：按 4.1 固定 | 配置 |

### 4.4 main 工程改造清单（实施基线）

| # | 改动 | 规模 |
|---|---|---|
| 1 | `partitions.csv`：§3.1 新布局（uext@factory + storage 缩 3MB） | 表 |
| 2 | `main.c`：`button_service_set_evt_cb()` 订阅 SET 单击（回调槽全工程空闲，已核实）→ 置 volatile 标志 | ~10 行 |
| 3 | 新增 `main/ui/overlays/overlay_uext.{c,h}`：2s 倒计时确认卡（复用 overlay_ota 的线程模型：按钮任务只写 volatile、lv_timer 消费；二次 SET → 查 uext 分区 → `esp_ota_set_boot_partition` + restart；OTA 活跃时拒绝；超时自动收卡）；零堆分配 | ~200 行 |
| 4 | `overlay_manager.{c,h}`：加 `OVERLAY_UEXT` 分派 | ~6 行 |
| 5 | `ota_service.c`：**选槽补丁**——新增 `ota_pick_update_slot()` 显式在 ota_0/ota_1 之间取对侧槽，替换 `handle_offer` check#6 与 `run_update` 两处 `esp_ota_get_next_update_partition()`；诊断行同步改。【V1.2】factory 槽本就不会被该 API 选中，补丁降级为第二道防线（防未来有人改表加 OTA 槽） | ~25 行 |
| 6 | `main/CMakeLists.txt`：登记 `ui/overlays/overlay_uext.c` | 1 行 |
| 7 | 文档：README / 使用流程加"SET 一键副屏"章节 | 文档 |

### 4.5 uext 固件的首次刷入与升级（D5：USB）

- 首次/升级一律 USB（uext 无 OTA 服务）：
  ```
  cd Desktop/esp32/uext_screen && idf.py build   # ★ 只 build，绝不 idf.py flash（见下方警告）
  python -m esptool -p COMx write-flash 0xA20000 build/uext_screen.bin
  ```
  不维护源码的使用者直接用仓库内预编译包：`display_audio_photo/firmware/uext_screen/`
  （含 bin、Windows 签名驱动、烧录步骤说明），烧录命令同上。
- ★★ **uext 工程里禁止 `idf.py flash`**：它会按分区表把 app 烧到 ota_0 偏移并可能重刷
  分区表 → **直接毁掉 main 固件**。只允许 `idf.py build` + 上面这条定点 write_flash。

### 6.4 ★★ uext 专属槽与 OTA 选槽补丁（V1.1 修正，取代 V1 的"槽位共用约定"）

V1 曾设计 uext 与 main OTA 共用 ota_1 —— **已废弃**：OTA A/B 轮替会覆盖 uext、且 SET 切换
目标会漂移。V1.1 起三方彻底分家：

| 槽 | 用途 | 谁写它 |
|---|---|---|
| ota_0 / ota_1 | main 的 A/B 轮替对 | 只允许 main 的 ota_service（OTA 升级） |
| **uext (factory)** | 副屏固件常驻 | 只允许 USB 定点烧写（§4.5）；`esp_ota_get_next_update_partition` 永不选中 factory，结构上隔离 |

**ota_service.c 选槽补丁（必须）**：`esp_ota_get_next_update_partition()` 的语义是"按 ota_seq
取下一个 OTA 槽"，一旦未来分区表再加 OTA 槽它可能轮换出错。补丁新增 `ota_pick_update_slot()`：

```c
/* 升级目标槽显式锁定：只允许 ota_0 <-> ota_1 对写（uext=factory 槽本就不在此列，此为第二道防线）。 */
static const esp_partition_t *ota_pick_update_slot(void)
{
    const esp_partition_t *run  = esp_ota_get_running_partition();
    const esp_partition_t *a = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_MIN + 0, NULL);
    const esp_partition_t *b = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_MIN + 1, NULL);
    if (!a || !b) return NULL;
    if (run && run->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_MIN + 0) return b;
    if (run && run->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_MIN + 1) return a;
    return NULL;   /* 运行槽不是 ota_0/ota_1（异常态）：拒绝升级，防误写 */
}
```

替换点：`handle_offer()` check#6（容量校验）与 `run_update()`（真写入）两处；
init 诊断行同步换掉。运行槽不是 ota_0/ota_1 时返回 NULL → 升级被拒绝（保守正确）。

★ 藏在补丁后面的红利：**PC 端 `--ota` 推 main 新固件不再波及副屏**；uext 升级 = USB 重烧
（D5 拍板），V2 若要免线升级 uext，再给 uext 加精简 OTA 服务。

---

## 5. 方案 B 详细设计（V2 演进，留档）

### 5.1 资源停止/恢复清单（kill 表）

| 资源/任务 | 栈 | 优先级 | 停止方式 | 现有 API | 需新增 |
|---|---|---|---|---|---|
| taskLVGL | 16KB 内部 | 4 | 卸触摸→卸显示→deinit | `lvgl_port_remove_touch/remove_disp/deinit` ✓ | 无（但路径未验证） |
| audio_task / audio_stream | 8K×2 | 6 | deinit | 无 | `audio_service_deinit()`（含 I2S+ES8389 释放） |
| record_task | 8K | 5 | stop+删任务 | 部分 | 补 deinit |
| udp_voice_ctl/tx/rx | 8K×3 | 5/7 | 删任务 | TX/RX_OFF 只关流 | 补 deinit |
| bt_push_proc / bt_sink_proc | — | — | 停管线 | 有 stop 雏形 | 补齐；`esp_bt_controller_deinit()` 释放大块 |
| link_manager | 8K | 4 | 删任务 | 无 | 补 deinit |
| pcnow_rx / pcnow_fetch | 8K×2 | 5 | 删任务 | 无 | 补 deinit |
| net_time | — | — | 删任务 | 无 | 补 deinit |
| led_task | 4K | 5 | 可保留 | — | — |
| **WiFi** | — | — | `esp_wifi_stop()+deinit()` | ✓ | 释放 **51.2KB 内部 DMA**（MEMORY §2 结构性风险的大头） |
| JPEG 硬件引擎 | — | — | VIDEO 取景器/预览先停 | camera_service 已有 owner 机制 ✓ | 无 |
| I2C 总线 | — | — | 共享：touch/codec/亮度同总线，UAC 与 GT1151 并存时注意竞态 | — | 总线互斥 |

### 5.2 切换顺序（方向敏感，必须按此序）

```
进入副屏：bsp_display_lock → remove_touch → remove_disp（释放 LVGL 内部绘制缓冲）
          → lvgl_port_deinit（杀 taskLVGL）→ 停 WiFi/BT → 停音频（放 I2S/codec）
          → 停其余服务 → jpeg 引擎独占 → TinyUSB+帧池 → transfer/UAC/HID 起飞
恢复主界面：反向 + lvgl_port_init → add_disp_rgb（bb_mode 按 sdkconfig）
          → add_touch → PageManager_Init → splash/main 重建
```

### 5.3 方案 B 的主要风险

- `lvgl_port_remove_disp → deinit → re-init` 全链路在本工程**从未验证**（bb_mode + 24bit 色深 + bin 字体动态加载都是放大器）；
- 服务重启顺序坑：F40（PSRAM 栈任务 vs flash IO cache 窗口）、audio 的 i2s_data_if 多组件共享（本就是已知 P0）、BT controller 重启稳定性；
- 泄漏累积：每切一次走一遍 deinit/init，任何一处漏 1KB，切十次就是慢性死亡——必须配三元组打点做 N 次切换回归。

---

## 6. 内存预算

| 项 | 方案 A（uext 独立固件） | 方案 B（main 内热切换） |
|---|---|---|
| JPEG 帧池 | 1.8MB PSRAM（独立 app，独占） | 1.8MB PSRAM（从 main 的 ~11.5MB 空闲里拿，OK） |
| LCD 帧缓冲 | 1.5MB PSRAM（uext 自己建面板） | 0（复用现有面板 2×fb） |
| TinyUSB 结构+EP 缓冲 | ~10-20KB 内部 | 同左，但要从"杀 WiFi 51KB DMA + LVGL 16KB 栈 + LVGL 绘制缓冲"里腾 |
| UAC/触摸任务 | 各 ~4KB | 同左 |
| **内部 RAM 结论** | 无压力（全新 app） | 有条件成立：必须真的杀掉 WiFi + LVGL 才宽裕；依赖 kill 表执行质量 |

---

## 7. 验收计划（方案 A）

| # | 验收项 | 判据 |
|---|---|---|
| A1 | uext 固件独立可跑 | USB 烧 ota_1 → Windows 出现第二显示器，800×480 |
| A2 | 60FPS 达标 | 设备日志 `fps: ≥55`（例程每 50 帧打印一次，app_lcd_s31.c:47-52） |
| A3 | 触摸可用 | Windows"平板设置"映射后，扩展屏可点 |
| A4 | UAC 音频 | Windows 声音输出到设备扬声器；麦克风可用 |
| A5 | main→uext 切换 | main 运行中 SET 单击 → ~5s 内副屏出现，日志 `USB Mount` |
| A6 | uext→main 切回 | 副屏中 SET 单击 → main 正常启动到主界面，UI/音频/蓝牙/推流全恢复 |
| A7 | 循环稳定性 | 连续切换 10 次，无崩溃无内存劣化（main 侧 boot-diag 对比） |
| A8 | 互斥防护 | 副屏运行时 main 的蓝牙/推流确实不工作（结构性，抽查） |
| A9 | PC 驱动 | 官方签名驱动免测试模式安装成功 |

## 8. 风险清单

| # | 风险 | 等级 | 对策 |
|---|---|---|---|
| R1 | 例程未在 IDF v6.1 验证（tinyusb/usb_device_uac registry 版本兼容性） | 高 | 先做 1 天的"uext 裸工程编译+烧录"预研（M0），挂了再评估降级 IDF 或打补丁 |
| R2 | 切换 = 重启，非即切 | 中 | 用户预期管理（本文档已声明）；实测若 <3s 可接受性较高 |
| R3 | ~~ota_1 双用途~~ **已消除（V1.2）**：uext 移入专属 factory 槽（OTA 机制永不选中 factory），OTA 目标槽显式锁定 ota_0/ota_1（§6.4 补丁为第二道防线） | — | — |
| R4 | ROLLBACK 联动：main 开了 `BOOTLOADER_APP_ROLLBACK_ENABLE`，切到 uext 后若 otadata 指向的镜像状态异常会被回滚 | 低 | uext 固件启动即调 `esp_ota_mark_app_valid_cancel_rollback()`（幂等）；实测 A5 覆盖 |
| R5 | uext 的 USB 与 main 的 USB-Serial-JTAG console 共用物理口？ | 低 | **不共用**：S31 有独立 USJ 外设（sdkconfig:2880 console 用 USJ），OTG 独立。但要核实板子上"高速 USB 口"接线对应 OTG（例程即此板，视为已验证） |
| R6 | 误触 SET 导致误切换 | 低 | 已拍板 D2：单击 + 2s 倒计时确认 |
| R7 | 分区表改动后 storage 偏移/容量变化，旧 littlefs 元数据失效 | 中 | 改表后必须 `idf.py storage-flash` 重刷 storage 分区（内容本就要随 SD 事故重建）；改动记录中登记 |
| R8 | uext 例程组件（tinyusb/usb_device_uac）与 IDF v6.1 兼容性未验证 | 高 | 即 R1：M0 预研 Go/No-Go；组件已全部离线 vendor（见 §4.1a），不受 registry 403 影响 |

## 9. 里程碑

| 阶段 | 内容 | 产出 |
|---|---|---|
| **M0 预研（1 天）** | uext 独立工程立起来：拷例程 + registry 组件在 v6.1 下编译通过 + USB 烧 ota_1 + Windows 出屏（A1/A2） | Go/No-Go |
| **M1 切换闭环** | main 侧 SET 订阅 + 切槽；uext 侧 app_switch 切回；A5/A6 | 可演示 |
| **M2 完整功能** | UAC 音频 + 触摸映射 + 防误触浮层（如选） + A3/A4 | 功能完整 |
| **M3 稳定性** | 10 次循环 + boot-diag 基线 + OTA 槽位防护 + 文档回填 | 验收 |

## 10. 决策点（全部已拍板 → 见 §0.1）

| # | 决策 | 选项 |
|---|---|---|
| D1 | **总体架构** | A 双固件切槽（推荐） / B 单固件热切换 / 先 A 后 B |
| D2 | SET 键防误触 | 单击即切（最简单）/ 双击才切 / 单击+屏幕 2s 倒计时确认（推荐） |
| D3 | 切回 main 的启动路径 | 走正常 splash（上滑进主界面，最简单）/ nvs 标志跳过 splash 直达主界面（省一步上滑） |
| D4 | uext 的音频/触摸 | 全开（PID 0x2986，推荐，例程原生）/ 只留屏（PID 0x2987） |
| D5 | uext 首次刷入方式 | USB（最简单，推荐）/ 直接做成 PC 端 --ota 推 uext.bin |

---

### 附：本文档证据文件索引

- 例程：`_extern/esp-iot-solution/examples/usb/device/usb_extend_screen/`（README_cn.md、main/app_usb.c、app_vendor.c、app_lcd_s31.c、usb_frame.c、tusb/tusb_config.h、Kconfig.projbuild、windows_driver/README_cn.md）
- 工程：`partitions.csv`、`sdkconfig`（USB/JPEG/RGB 行）、`managed_components/espressif__esp32_s31_korvo_1/src/bsp_display.c`、`managed_components/espressif__esp_lvgl_port/include/esp_lvgl_port*.h`、`main/middleware/system/button_service.{c,h}`
