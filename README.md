<div align="center">

# Harness ESP32-S31 · AI Agent 闭环开发终端

**基于 ESP32-S31-Korvo-1 V1.1 的设备 ↔ PC 双通道低延迟语音传声机固件**
<br>
**不只是代码，更是一套 AI Agent 可读规范 → 写代码 → 烧录 → 抓日志 → 反复验证的智能开发闭环**

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.1.0-blue?logo=espressif&logoColor=white)](https://github.com/espressif/esp-idf)
[![Target](https://img.shields.io/badge/MCU-ESP32--S31-red?logo=espressif&logoColor=white)](https://www.espressif.com/en/products/socs/esp32-s3)
[![Board](https://img.shields.io/badge/Board-Korvo--1_V1.1-orange)](https://docs.espressif.com/projects/esp-bsp/en/latest/)
[![License](https://img.shields.io/badge/License-Apache--2.0-green)](LICENSE)
[![Language](https://img.shields.io/badge/C-C99-yellow)](https://en.cppreference.com/w/c)
[![RTOS](https://img.shields.io/badge/FreeRTOS-SMP-purple)](https://www.freertos.org/)

</div>

---

## 📡 这是什么

一台 ESP32-S31 设备，通过 Wi-Fi 与 PC 双向对讲：

| 模式 | 协议 | 端口 | 用途 |
|---|---|---|---|
| 🎙 **UDP 实时通话** | UDP | 50000 / 50001 | 全双工对讲，端到端 ≤100 ms |
| 🎧 **TCP 整段录音** | TCP | 50002 | 整段落盘 WAV，零丢帧 |
| 📢 **信令通道** | UDP 广播 | 50003 | 模式切换、设备发现 |

音频参数：**16 kHz / 16 bit / mono**，20 ms = **640 B/帧**。

---

## 🤖 为什么这仓库不只是固件

传统嵌入式开发：人写代码 → 人编译 → 人插线烧录 → 人开串口看日志 → 人判断对错 → 人改。

本仓库把这套流程**沉淀成 Agent 可执行的 Skill + 工程规范**：

```
Agent 读 docs/工程规范/01~11
        ↓
按模板写 middleware / ui 代码
        ↓
idf.py build                  （自动看 error:）
        ↓
idf.py -p COM9 flash          （自动看 Hash verified）
        ↓
python docs/_serial_capture.py（拉 RTS 复位，抓 12 s 日志）
        ↓
Agent 读日志：panic? 功能日志? 静默?
        ↓
不对就改代码，再走一遍
```

- **[`skills/esp-idf-closed-loop.md`](skills/esp-idf-closed-loop.md)** —— 闭环 SOP，Agent 每次开发前必读；
- **[`docs/工程规范/`](docs/工程规范/)** —— 分层架构、命名、模块模板、CheckList（01~10 + 附录）；
- **[`docs/项目规划/`](docs/项目规划/)** —— 需求文档、todolist、改动记录。

### 🚫 红线（Agent 写代码时必须遵守）

- middleware 层不得 `#include "lvgl.h"`；
- UI 事件回调里不得直接建硬件任务、不得操作 codec / I2S / I2S 寄存器；
- 业务任务栈 ≥ 8192 字节；
- 新增 `.c` 必须注册 `main/CMakeLists.txt` 的 `SRCS`；
- 高频音频链路（每 20 ms）禁止 `ESP_LOGI`。

---

## 🛠 硬件平台

| 项 | 值 |
|---|---|
| 开发板 | **ESP32-S31-Korvo-1 V1.1**（官方） |
| MCU | ESP32-S31 双核 RISC-V @ 320 MHz，512 KB SRAM |
| 存储 | 16 MB Flash + 16 MB Octal PSRAM |
| Codec | ES8389（I2C 0x20，**MCLK=GPIO42 未连 → `use_mclk=false`**） |
| 音频 I/O | 双 MEMS 麦 + 双 NS4150B 功放 |
| 屏幕 | 4.3" RGB 800×480 ST7262E43 + 电容触摸 |
| ESP-IDF | **v6.1.0**（`IDF_TARGET=esp32s31`） |
| 烧录端口 | COM9 |
| 分区表 | nvs 24K / phy 4K / otadata 8K / **ota_0 3M / ota_1 3M** / **storage littlefs 9.87M**（无 factory，A/B OTA） |
| 文件系统 | LittleFS（挂载点 `/littlefs`，素材/录音/壁纸同盘分目录） |

---

## 📁 目录结构

```
display_audio_photo/
├── main/
│   ├── app/main.c                # app_main() 初始化顺序 + 上滑自动播开机提示音
│   ├── middleware/               # 硬件业务，不 include LVGL
│   │   ├── audio_service.c/h     # ES8389 + I2S + 常驻任务+命令队列（文件播放/录音/流式）
│   │   ├── fs_service.c/h        # LittleFS 挂载 + JPEG 解码缓冲
│   │   ├── led_service.c/h        # RGB LED（GPIO37 WS2812，命令队列）
│   │   ├── button_service.c/h    # 4 路 ADC 分压按键 + 音量队列
│   │   ├── wifi_manager.c/h      # STA 扫描/连接/IP
│   │   ├── udp_voice.c/h         # （阶段二待建）UDP 通话
│   │   └── tcp_record.c/h        # （阶段三待建）TCP 录音
│   ├── ui/                       # LVGL 界面（PageManager 栈式导航）
│   │   ├── page_splash.c/h       # 开屏壁纸 + 上滑进主界面
│   │   ├── page_main.c/h         # CALL / REC / SYS tab
│   │   ├── page_wifi.c/page_rgb.c/page_system.c/page_wav_player.c
│   │   ├── ui_manager/page_manager.c/h
│   │   └── overlay_wallpaper.c    # 壁纸浮层
│   └── 3rd/ringbuf/              # 第三方纯 C 环形缓冲
├── docs/
│   ├── 工程规范/                 # Agent 写代码前必读（01~11 + 附录）
│   ├── 项目规划/                 # 需求文档 / todolist / 改动记录 / ICD
│   └── _serial_capture.py        # 替代 idf.py monitor
├── skills/
│   └── esp-idf-closed-loop.md   # 闭环 SOP：build→flash→抓日志→定位→再改
├── littlefs_content/             # LittleFS 素材（xwkkk.jpg / boot_tone.wav）
├── components/esp_littlefs/      # 本地组件（registry 403，Git 拉取）
├── partitions.csv                # OTA 双 3MB + storage littlefs
└── CMakeLists.txt
```

---

## 🚀 快速开始

### 1. 安装 ESP-IDF（用 EIM 一键管理）

**EIM（Espressif IDF Manager）** 是乐鑫官方的 IDF 版本管理器，图形界面，免去手动装 Python / 工具链。

- 下载：[https://idf.espressif.com/](https://idf.espressif.com/)
- GitHub：[https://github.com/espressif/idf-im-ui](https://github.com/espressif/idf-im-ui)
- 教程：
  - 官方文档：[Get Started](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/index.html)
  - B站/EIM 视频教程搜索"乐鑫 EIM"
  - 本仓库用 **v6.1.0**，安装时在 EIM 里选 v6.1.x 即可

安装完后 EIM 会自动配好 Python venv、交叉编译工具链、OpenOCD、CMake。

### 2. 打开 IDF 终端

双击本仓库根目录的 **`IDF_v6.1_Powershell.lnk`**（或手动执行 PowerShell profile）：

```powershell
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
cd <本仓库根>
```

成功标志：看到 `IDF_PATH: C:\esp\v6.1\esp-idf` 等。

### 3. 编译

```powershell
idf.py build
```

### 4. 烧录（COM9）

```powershell
idf.py -p COM9 flash
```

### 5. 看日志

非交互终端跑 `idf.py monitor` 会报 TTY 错误，用本仓库脚本：

```powershell
& "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe" docs\_serial_capture.py
```

自动拉 RTS 复位，读 12 秒启动日志。

---

## 🧠 如何让 AI Agent 接管开发

1. 把本仓库喂给 Agent（豆包 / Claude / Cursor 等）；
2. 让 Agent 先 `Read skills/esp-idf-closed-loop.md`；
3. 再 `Read docs/工程规范/01~11`；
4. 给任务（例如"实现 wifi_manager.c 按 STA 模式连指定 SSID"）；
5. Agent 会自己：
   - 按 07 章模板写代码；
   - 注册 `main/CMakeLists.txt` 的 `SRCS`；
   - `idf.py build`；
   - `idf.py -p COM9 flash`；
   - 跑 `docs/_serial_capture.py` 看启动日志；
   - 遇到 panic 自己定位、改代码、再烧录；
   - 完成后在 `docs/项目规划/改动记录.md` 追加记录。

---

## 🔗 乐鑫官方仓库参考

| 仓库 | 用途 |
|---|---|
| [espressif/esp-idf](https://github.com/espressif/esp-idf) | ESP-IDF 官方框架（本仓库基于 v6.1.0） |
| [espressif/esp-bsp](https://github.com/espressif/esp-bsp) | 板级支持包（Korvo-1 BSP 来源） |
| [espressif/esp-adf](https://github.com/espressif/esp-adf) | 音频开发框架 |
| [espressif/esp-sr](https://github.com/espressif/esp-sr) | 语音识别 / AFE 音频前端 |
| [espressif/esp32-camera](https://github.com/espressif/esp32-camera) | 摄像头驱动（本工程保留了依赖） |
| [espressif/idf-im-ui](https://github.com/espressif/idf-im-ui) | EIM 官方源码 |
| [espressif/esp-jpeg_dec](https://github.com/espressif/idf-extra-components/tree/master/jpeg_dec) | JPEG 硬件解码（开屏壁纸用） |

---

## ✅ 当前进度

- [x] M1 音频链路（ES8389 / I2S DMA / WAV 播放 / 本地录音）
- [x] UI 骨架（CALL / REC / SYS 子页 + 开屏壁纸 + 上拉手势 + RGB / WiFi / System 监视卡）
- [x] RGB LED 呼吸 + 4 路 ADC 分压按键（音量 ± / mode / set）
- [x] Wi-Fi STA（扫描/连接/状态灯）
- [x] 分区表 OTA 双 3MB + SPIFFS→LittleFS 迁移；开机提示音（16k/mono）
- [x] 工程规范 11 篇 + 闭环 SOP（含分场景烧录、日志规范）
- [ ] M2 UDP 通话（audio 流式改造 + link_manager + udp_voice + PC 端）
- [ ] M3 TCP 录音 + M3.5 远程文件管理/文件浏览器 + M4 多任务调度
- [ ] M6 OTA A/B 升级

详见 [`docs/项目规划/todolist.md`](docs/项目规划/todolist.md)。

---

<div align="center">

**License**：Apache-2.0（沿用 esp-bsp） · **Maintainer**：xkk6663

</div>
