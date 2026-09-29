<div align="center">

# ESP32-S31 端侧 AI 视觉终端

**人脸识别打卡 · 720p 拍照录像 · 手势检测 · 小智 AI 对话 —— 单芯片离线+云端混合智能**
<br>
**不只是代码，更是一套 AI Agent 可读规范 → 写代码 → 静态校验 → 烧录 → 抓日志 → 反复验证的智能开发闭环**

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.1.0-blue?logo=espressif&logoColor=white)](https://github.com/espressif/esp-idf)
[![Target](https://img.shields.io/badge/MCU-ESP32--S31-red?logo=espressif&logoColor=white)](https://www.espressif.com/en/products/socs/esp32-s3)
[![Board](https://img.shields.io/badge/Board-Korvo--1_V1.1-orange)](https://docs.espressif.com/projects/esp-bsp/en/latest/)
[![License](https://img.shields.io/badge/License-Apache--2.0-green)](LICENSE)
[![Language](https://img.shields.io/badge/C-C99-yellow)](https://en.cppreference.com/w/c)
[![RTOS](https://img.shields.io/badge/FreeRTOS-purple)](https://www.freertos.org/)

</div>

---

## 📡 这是什么

一台跑在 **ESP32-S31-Korvo-1** 上的端侧视觉智能终端：OV3660 摄像头 + 4.3" 800×480 触摸屏，
**不依赖任何云端服务器**完成人脸检测 / 人脸识别 / 手势识别，并与 **小智 AI 云端大脑**混合协作。

### 核心功能一览

| 功能 | 说明 |
|---|---|
| 🧑‍💼 **人脸识别打卡系统（S8，已验收）** | 录入模式（三采三验）+ 用户模式；识别成功自动拍照存 SD 卡专属人物文件夹，附精确到秒的可信时间（SNTP 校时），并生成 CSV 流水 |
| 📷 **720p 拍照** | OV3660 JPEG 1280×720，SD 卡自动编号存档 |
| 🎬 **720p 录像** | AVI/MJPEG 封装，实时取景预览；按住即录、松手即停（deadman 交互，含滑出按钮收尾） |
| 👋 **手势检测** | espdet 检手 + MobileNetV2 手势分类，检测框实时叠加 |
| 🗂 **文件管理** | SD 卡照片/视频/录音浏览回放，缩略图等比 letterbox 预览 |
| 🤖 **小智 AI 对话** | esp_xiaozhi 云端大模型对话 + AI 浮窗 UI + IoT 物体抽象（摄像头/喇叭/灯/设备） |
| 🎵 **本地音频** | MP3 播放（media_player_service）、本地录音 WAV、开机提示音 |
| 📶 **网络** | Wi-Fi STA 扫描/连接、SNTP 校时（CST-8）、和风天气、WiFi 管理页 |
| 💡 **外设** | WS2812 RGB LED、4 路 ADC 分压按键 + 音量、双 MEMS 麦 + 双 NS4150B 功放 |

### 端侧 AI 能力（esp-dl，模型放 LittleFS 不编固件）

| 模型 | 用途 | 规格 |
|---|---|---|
| HumanFaceDetect (espdet p4) | 人脸检测 | 224×224，绿框叠加 |
| HandDetect | 手掌检测 | 与人脸检测共用引擎，按键切换档位 |
| MSR + MNP | 识别对齐 | 单应性配准 + 5 点关键点 |
| MFN | 人脸特征 | 512 维特征向量，比对阈值判定 |
| MobileNetV2 手势分类 | 手势识别 | HAND 档独有第二段模型 |

**人脸库**：本地 NVS 存储，硬上限 **10 人**；识别引擎（约 2.3 MB PSRAM）随打卡/录入会话按需加载、会话结束即释放。

---

## 🖥 用户界面

主界面为 5 个 Tab（LVGL tabview），FACE 记录页为独立栈式页面：

| Tab | 功能 |
|---|---|
| **CAM** | 240×240 实时取景；START / FACE / PUNCH 按钮 + MODE 录入态切换；人脸/手势绿框叠加；点击全屏 2× 放大；PSRAM 即用即释放（关检测即卸模型） |
| **VIDEO** | 720p 取景 + 拍照/录像两颗右侧竖排圆形按键（按下即拍 / 按住录松手停，按下变色）；Q / PREVIEW 辅助按钮 |
| **FILES** | SD 卡照片/视频/录音浏览回放，缩略图等比缩放（不拉伸变形） |
| **REC** | 本地录音：按一次开始、再按一次落盘 WAV 到 `/sdcard/music`，FILES 页回放 |
| **SYS** | 音量、WiFi/蓝牙/RGB/System 入口、系统信息（板卡/IDF/网络） |
| **FACE 页** | 打卡记录浏览：NEW FIRST / OLD FIRST 时间排序、照片等比预览、行内删除（两步确认）、SYNC 一键对账（清理 SD 卡已手删照片的孤儿记录） |

另有：开屏壁纸 + 上滑手势、壁纸浮层下拉时钟天气卡（未校时自动隐藏）、小智 AI 浮窗、RGB 调色页、WiFi 管理页（LVGL 键盘输密码）。

---

## 🧠 内存与实时性设计（本项目最硬核的部分）

PSRAM 16 MB 要同时养活：LVGL 双帧缓冲 + 摄像头流 + AI 模型 + 取景器解码三件套 + 音频链路。
本项目沉淀了一套**显式的资源生命周期纪律**（详见 [`docs/项目规划/改动记录.md`](docs/项目规划/改动记录.md) §73.10 的 12 项资源总表）：

- **即用即释放**：det 模型（~1.25 MB）、识别引擎（~2.3 MB）、打卡抓帧缓冲、录入特征缓冲——用完立刻归还，按钮 OFF = 彻底卸载；
- **租约互斥**：音频通道（media_player / 小智 / 本地录音三方）经 ext 独占租约协议调度，杜绝 codec 抢占；
- **起流自愈**：摄像头起流遇 PSRAM 碎片（REQBUFS / DVP backup ENOMEM）自动卸载 CAM 页住户并重试一次，用户无感；
- **判据先行**：每档相机格式有精确到块的内存预判（`[streammem]` 日志），失败时打**住户报告**点名谁占着大块，排障不用猜；
- **单帧尺度账本**：720p JPEG 帧 = 921,600 B；DVP backup 再加一块同尺寸——这些数字全部来自驱动源码实证（含 IDF 本体 `esp_driver_cam`，勘误史见 §73.15）。

---

## 🛠 硬件平台

| 项 | 值 |
|---|---|
| 开发板 | **ESP32-S31-Korvo-1 V1.1**（官方） |
| MCU | ESP32-S31 双核 RISC-V @ 320 MHz，512 KB SRAM |
| 存储 | 16 MB Flash + 16 MB Octal PSRAM |
| 屏幕 | 4.3" RGB 800×480 ST7262E43 + 电容触摸 |
| 摄像头 | OV3660 DVP（240×240 RGB565 取景 / 720p JPEG 媒体档，寄存器级格式切换） |
| SD 卡 | SPI/SDMMC 挂载 `/sdcard`（照片/视频/录音） |
| Codec | ES8389（I2C 0x20，**MCLK=GPIO42 未连 → `use_mclk=false`**） |
| 音频 I/O | 双 MEMS 麦 + 双 NS4150B 功放 |
| ESP-IDF | **v6.1.0**（`IDF_TARGET=esp32s31`） |
| 烧录端口 | COM9 |

### 分区表（S5 v1.2 定稿）

| 分区 | 类型 | 大小 | 用途 |
|---|---|---|---|
| nvs / phy / otadata | data | 24K / 4K / 8K | 系统配置 |
| ota_0 | app | **7 MB** | 单 app 槽（无唤醒词场景下放弃双 OTA，恢复手段 = USB 全量烧录） |
| storage | littlefs | **9.29 MB** | AI 模型 5.13 MB + 中文 bin 字体 2.74 MB + 图标/音效/壁纸 |

模型与字体全部放 LittleFS **不编进固件**（app bin 实测 6.5 MB，占 7 MB 槽的 93%——体积紧张时按"阉割非核心功能保主功能"的纪律取舍）。

---

## 📁 目录结构

```
display_audio_photo/
├── main/
│   ├── app/main.c                    # app_main() 初始化顺序
│   ├── middleware/
│   │   ├── camera/                   # ★ S8 主战场
│   │   │   ├── camera_service.c/h    #   相机拥有权状态机（CAM 240×240 ↔ VIDEO 720p 转移）
│   │   │   ├── ov3660_fmt.c/h        #   OV3660 带寄存器表的格式切换
│   │   │   ├── face_service.cpp/h    #   人脸/手势检测引擎封装（4 态状态机 + unload_sync）
│   │   │   ├── face_recog_service.*  #   识别引擎（MSR+MNP+MFN，独立加载任务）
│   │   │   ├── face_quality.c/h      #   录入帧质量门槛
│   │   │   ├── punch_service.c/h     #   打卡事务（拍照→JPEG→CSV 流水→记录删除/对账）
│   │   │   ├── capture_service.c/h   #   720p 拍照
│   │   │   ├── viewfinder.c/h        #   JPEG 软解取景器三件套（acquire/release 计账）
│   │   │   └── jpeg_util.c/h         #   JPEG 软件编码
│   │   ├── face_db/face_db.c/h       #   人脸特征库（NVS，≤10 人硬上限）
│   │   ├── media/                    #   SD 卡媒体：file_mgr / record_video / avi_mjpeg(封装+解封装)
│   │   ├── audio/                    #   audio_service（租约互斥）+ media_player + rec_local
│   │   ├── ai/                       #   小智对话 + IoT 物体抽象
│   │   ├── net/                      #   wifi_manager + net_time_service（SNTP+天气）
│   │   └── periph/                   #   sdcard / fs / led / button + mem_diag
│   ├── ui/
│   │   ├── page_main.c/h             # tabview：CAM / VIDEO / FILES / REC / SYS
│   │   ├── page_face.c/h             # 打卡记录页（排序/预览/删除/对账）
│   │   ├── tab_camera / tab_video / tab_files / tab_record / tab_sys_entry
│   │   ├── overlay_wallpaper / overlay_ai / overlay_wp_particles
│   │   ├── ui_manager/page_manager   # 栈式页面导航
│   │   └── ui_binfont.c/h + fonts/   # 中文 bin 字体（CN 20/30/45 + digital 96）
│   └── 3rd/ringbuf/
├── components/                       # esp_littlefs / esp_xiaozhi / hand_detect / hand_gesture_recognition
├── littlefs_content/                 # models（5.13MB AI 模型）+ fonts + icons + 壁纸
├── docs/
│   ├── 工程规范/                     # Agent 写代码前必读（01~11 + 附录代码文件索引）
│   ├── 项目规划/                     # 需求文档 / todolist / S8 方案 / 改动记录（§1~§73）
│   ├── 开发文档/                     # 内存管理 / 方案沉淀
│   └── _serial_capture.py            # 串口日志抓取（替代 idf.py monitor）
├── skills/esp-idf-closed-loop.md     # 闭环 SOP
├── partitions.csv                    # 单 app 7MB + storage littlefs 9.29MB
└── sdkconfig.defaults
```

---

## 🚀 快速开始

### 1. 安装 ESP-IDF（用 EIM 一键管理）

**EIM（Espressif IDF Manager）** 是乐鑫官方的 IDF 版本管理器，图形界面，免去手动装 Python / 工具链。

- 下载：[https://idf.espressif.com/](https://idf.espressif.com/)
- 本仓库用 **v6.1.0**，安装时在 EIM 里选 v6.1.x 即可

安装完后 EIM 会自动配好 Python venv、交叉编译工具链、OpenOCD、CMake。

### 2. 打开 IDF 终端

双击本仓库根目录的 **`IDF_v6.1_Powershell.lnk`**（或手动执行 PowerShell profile）：

```powershell
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
cd <本仓库根>
```

### 3. 编译 + 烧录（全量，一步到位）

```powershell
idf.py reconfigure
idf.py build
idf.py -p COM9 flash        # 全量烧录（app + storage.bin，分区偏移变更时必须全量）
```

### 4. 看日志

非交互终端跑 `idf.py monitor` 会报 TTY 错误，用本仓库脚本：

```powershell
& "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe" docs\_serial_capture.py
```

自动拉 RTS 复位，读 12 秒启动日志。

### 5. 第一次体验

1. 开机 → 上滑进主界面 → 连 Wi-Fi（SYS → WiFi）；
2. **CAM 页** → MODE 切录入 → 对准人脸按 PUNCH 三次完成录入 → MODE 切回用户态；
3. 换个人脸对着摄像头按 PUNCH：识别成功自动存照 + 记录一条打卡；
4. **FACE 记录页**浏览打卡记录、看照片、删记录、SYNC 对账；
5. **VIDEO 页**：按住圆钮录像、按下拍照，插 SD 卡后 **FILES 页**回看。

---

## 🤖 如何让 AI Agent 接管开发

本仓库从第一天起就是按"AI Agent 可读"标准维护的：

1. 把本仓库喂给 Agent（豆包 / Claude / Cursor 等）；
2. 让 Agent 先 `Read skills/esp-idf-closed-loop.md`（闭环 SOP）；
3. 再 `Read docs/工程规范/01~11`（分层架构 / 命名 / 模块模板 / CheckList）；
4. 给任务 —— Agent 会自己按模板写代码、注册 CMakeLists、跑静态校验、
   交叉读源码定位根因（含 IDF 本体与 managed_components 驱动源码）、
   完成后在 `docs/项目规划/改动记录.md` 按章节编号追加记录。

### 🚫 红线（Agent 写代码时必须遵守）

- middleware 层不得 `#include "lvgl.h"`；
- UI 事件回调里不得直接建硬件任务、不得操作 codec / I2S 寄存器；
- 业务任务栈 ≥ 8192 字节；
- 新增 `.c` 必须注册 `main/CMakeLists.txt` 的 `SRCS`；
- 高频链路（音频 20ms 帧 / 相机帧处理）禁止 `ESP_LOGI`；
- **PSRAM 大块分配必须有明确的生命周期归属**（谁分配谁释放、页面退出必归还）；
- **驱动结论必须搜遍 IDF 本体 + managed_components 两处**（§73.15 的血泪教训）。

---

## 📊 项目进度（2026-09-29 定稿）

- [x] UI 骨架 + 开屏 + 中文 bin 字体 + 栈式页面管理
- [x] Wi-Fi STA + SNTP 校时 + 天气卡
- [x] OV3660 双档相机（CAM 240×240 / VIDEO 720p）+ 相机拥有权状态机
- [x] **720p 拍照 + AVI/MJPEG 录像**（封装器过原生单测）
- [x] **S8 人脸识别打卡系统**（录入/识别/打卡/记录管理，已上机验收）
- [x] 手势检测（espdet + MobileNetV2）
- [x] 文件管理页（照片/视频/录音浏览回放）
- [x] 小智 AI 对话 + AI 浮窗 + MP3 播放 + 本地录音
- [x] PSRAM 全量生命周期审计（即用即释放 + 自动腾地 + 住户报告）
- [ ] 阶段 3.5：远程文件管理（fs_http_service / 壁纸热更新）
- [ ] OTA A/B 升级（分区已占坑，单槽）
- [ ] file_mgr 缩略图缓冲按需化（921,600 B 常驻 → 按需）

详见 [`docs/项目规划/todolist.md`](docs/项目规划/todolist.md)。

---

## 🔗 关键依赖

| 组件 | 用途 |
|---|---|
| [espressif/esp-idf](https://github.com/espressif/esp-idf) v6.1.0 | 框架（含 esp_driver_cam / esp_video / esp_cam_sensor） |
| [espressif/esp-bsp](https://github.com/espressif/esp-bsp) | Korvo-1 板级支持 |
| [espressif/esp-dl](https://github.com/espressif/esp-dl) | 端侧 AI 推理（检测/识别/手势模型运行时） |
| [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)（esp_xiaozhi 组件化） | 小智 AI 对话客户端 |
| [joltwallet/esp_littlefs](https://github.com/joltwallet/esp_littlefs) | LittleFS 文件系统 |
| LVGL v9 | UI 框架 |

---

<div align="center">

**License**：Apache-2.0（沿用 esp-bsp） · **Maintainer**：xkk6663

</div>
