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
Agent 读 docs/工程规范/01~10
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
| 分区表 | nvs 24K / factory app 2M / spiffs 5M |

---

## 📁 目录结构

```
display_audio_photo/
├── main/
│   ├── app/main.c                 # app_main() 只做初始化顺序
│   ├── middleware/                # 硬件业务，不依赖 LVGL
│   │   ├── audio_service.c/h      # ES8389 + I2S + 常驻 audio_task + 命令队列
│   │   ├── fs_service.c/h         # 文件类型识别 + JPEG 解码缓冲
│   │   ├── wifi_manager.c/h       # （待建）STA 连接
│   │   ├── udp_voice.c/h          # （待建）UDP 通话
│   │   └── tcp_record.c/h         # （待建）TCP 录音
│   └── ui/                        # LVGL 界面
│       ├── ui_disp.c/h            # tabview 主框架（CALL/REC/SYS）
│       ├── ui_splash.c/h          # 开屏壁纸 + 上拉手势
│       ├── ui_call.c/h            # CALL tab
│       ├── ui_record.c/h          # REC tab
│       ├── ui_settings.c/h        # SYS tab
│       ├── ui_windows.c/h         # WAV 播放弹窗
│       └── ui_state.h
├── docs/
│   ├── 工程规范/                  # Agent 写代码前必读
│   │   ├── 01_工程分层架构.md
│   │   ├── 02_文件头规范.md
│   │   ├── ... 03~10 ...
│   │   └── 附录_代码文件索引.md
│   ├── 项目规划/                  # 推进方向必读
│   │   ├── 需求文档.md
│   │   ├── todolist.md
│   │   └── 改动记录.md
│   └── _serial_capture.py         # 替代 idf.py monitor
├── skills/
│   └── esp-idf-closed-loop.md    # 闭环开发 SOP（硬约束：先读规范再写代码）
├── spiffs_content/                # SPIFFS 素材（xwkkk.jpg / test_16kHz.wav）
├── IDF_v6.1_Powershell.lnk       # 一键打开 IDF v6.1 终端
├── partitions.csv
├── sdkconfig / sdkconfig.defaults / sdkconfig.bsp.esp32_s31_korvo_1
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
3. 再 `Read docs/工程规范/01~10`；
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

- [x] M1 音频链路（ES8389 / I2S DMA / WAV 播放 / 5 s 录音）
- [x] UI 骨架（CALL / REC / SYS 三 tab + 开屏壁纸 + 上拉手势）
- [x] 工程规范 11 篇 + 闭环 SOP
- [ ] M2 UDP 通话（wifi_manager + udp_voice）
- [ ] M3 TCP 录音（tcp_record）
- [ ] PC 端 Python 客户端

详见 [`docs/项目规划/todolist.md`](docs/项目规划/todolist.md)。

---

<div align="center">

**License**：Apache-2.0（沿用 esp-bsp） · **Maintainer**：xkk6663

</div>
