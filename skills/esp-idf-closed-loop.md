# Skill：ESP-IDF 终端闭环开发 SOP

> 适用工程：`C:\Users\xiao1\Desktop\esp32\display_audio_photo`
> 硬件：ESP32-S31-Korvo-1 V1.1，烧录端口 COM9
> ESP-IDF：v6.1.0
>
> 本 Skill 记录"改代码 → 编译 → 烧录 → 抓日志 → 看 panic → 修 → 再来"的终端闭环操作，
> 不依赖 VS Code / ESP-IDF 插件，纯 PowerShell + Python。

---

## 0. 硬约束：先读规范再写代码，先读规划再推进

`docs/` 分两层，**写代码前必须先读对应层**：

### 0.1 编写代码 → 遵守 `docs/工程规范/` 里的文档

任何开发新代码 / 新增模块 / 修改现有模块之前，必须先读：

1. `docs/工程规范/01_工程分层架构.md` — 三层依赖方向，禁止跨层调用；
2. `docs/工程规范/02_文件头规范.md` — SPDX 头 + 日志 TAG；
3. `docs/工程规范/03_文件组织管理.md` — CMakeLists SRCS 注册规则；
4. `docs/工程规范/04_命名规则.md` — 文件/函数/变量/宏/枚举命名；
5. `docs/工程规范/05_注释规范.md`；
6. `docs/工程规范/06_数据类型规范.md` — stdint / TickType_t / MALLOC_CAP_DMA；
7. `docs/工程规范/07_模块实现规范.md` — 常驻任务+命令队列模板、8KB 栈、互斥锁；
8. `docs/工程规范/08_代码风格.md`（含日志与调试规范：TAG/级别、关键节点必打、高频路径禁打、错误带 err、grep 验收）；
9. `docs/工程规范/09_常见模式.md` — JPEG 解码、UI 配色、事件回调；
10. `docs/工程规范/10_新增模块CheckList.md` — 新模块逐项打勾；
11. `docs/工程规范/附录_代码文件索引.md` — 当前文件清单。

**红线（违反即返工）：**

- middleware 层不得 `#include "lvgl.h"`；
- UI 事件回调里不得直接建硬件任务、不得直接操作 codec/I2S/I2S 寄存器；
- 业务任务栈不得 < 8192 字节；
- 新增源文件必须同步注册 `main/CMakeLists.txt` 的 SRCS；
- 高频音频链路（每 20ms）禁止打 `ESP_LOGI`；
- 改动必须追加到 `docs/项目规划/改动记录.md`。

### 0.2 项目进度 / 推进方向 → 遵守 `docs/项目规划/` 里的文档

- `docs/项目规划/需求文档.md` — 产品需求、双通道模式、验收指标；
- `docs/项目规划/todolist.md` — 当前阶段任务拆解；
- `docs/项目规划/改动记录.md` — 每轮改动流水。

未读对应层前不得开始动手。

---

## 1. 环境启动（每次新终端必做）

ESP-IDF 不在系统 PATH 里，直接敲 `idf.py` 会报"不是内部或外部命令"。
本机器正确的启动方式是**解析 IDF v6.1 的 PowerShell profile 脚本**，不要用 `export.ps1`。

```powershell
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
Set-Location 'C:\Users\xiao1\Desktop\esp32\display_audio_photo'
```

启动成功后会看到：

```
IDF PowerShell Environment
-------------------------
IDF_PATH: C:\esp\v6.1\esp-idf
IDF_TOOLS_PATH: C:\Espressif\tools
IDF_PYTHON_ENV_PATH: C:\Espressif\tools\python\v6.1\venv
```

### 踩过的坑（不要再试）
- ❌ `export.ps1`：报 `$idf_exports` 未定义；
- ❌ `export.bat`：Python venv 路径不匹配；
- ❌ 双击 `IDF_v6.1_Powershell.lnk` 再在里面 `cd`：可以，但 agent 后台跑命令时拿不到 TTY，monitor 会失败；
- ✅ 后台命令统一用：
  ```powershell
  powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ". '<profile>'; Set-Location '<工程目录>'; idf.py <子命令>"
  ```

---

## 2. 编译

```powershell
idf.py build
```

- 首次配置或改了 `sdkconfig.bsp.esp32_s31_korvo_1` 后加：
  `-D SDKCONFIG_DEFAULTS=sdkconfig.bsp.esp32_s31_korvo_1`；
- 增量编译只改几个 .c 时不带该参数即可；
- 成功标志：`Project build complete` + `display_audio_photo.bin binary size 0x...`；
- OTA app 槽 3MB，当前 bin ≈ 0x18a360（~1.63MB），槽内剩余约 49%。

---

## 3. 烧录（分场景，避免每次都写 10MB）

> 当前分区：`ota_0/ota_1 各 3MB` + `storage littlefs 9.87MB(@0x620000)`。
> 已去掉 `FLASH_IN_PROJECT`，默认 **不再重烧 storage 素材镜像**，板子上已有素材/录音保留。
> 原则：**只写这次真正变了的那一块**，其余不动。

### 3.1 选哪条命令（按改动内容对号入座）

| 这次改了什么 | 用哪条命令 | 烧写内容 | 耗时 |
|---|---|---|---|
| 只改了 `main/` 业务代码（最常见） | `idf.py -p COM9 app-flash` | 仅 ota_0 app | **~2–5 秒** |
| 改了 `sdkconfig` / bootloader / 分区表 | `idf.py -p COM9 flash` | bootloader+partition+otadata+app（~2MB） | ~10–20 秒 |
| 改了 `littlefs_content/` 里的素材（壁纸/wav） | 先 `flash` 再单独烧 storage | 上述 + storage.bin@0x620000（10.35MB） | ~11 分钟 |
| 新板子 / 整片擦除后首次烧录 | 全套：`flash` + 烧 storage | 全部 | ~11 分钟 |
| 阶段六 OTA 升级 | 不烧录，走 wifi TCP 写 ota_1 | — | — |

> 日常开发 99% 用第一条 `app-flash`。不要无脑 `flash` 更不要每次都写 storage。

### 3.2 单独烧 storage 素材镜像（仅素材变更/首次时）

> 注意：`idf.py flash-storage` **不是有效 target**（会报 unknown target）。
> 去 `FLASH_IN_PROJECT` 后没有自动的 storage 烧录 target，必须用 esptool 手动写：

```powershell
cd build
& "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe" -m esptool `
  --chip esp32s31 -p COM9 -b 460800 --before default-reset --after hard-reset `
  write-flash 0x620000 storage.bin
```

要点：参数顺序是 **`write-flash <地址> <文件>`**（地址在前）；460800 波特 + esptool 压缩约 35 秒写完 10MB。

### 3.3 注意事项
- 不要在这条命令后直接跟 `monitor`（见第 4 节）；
- 成功标志：`Hash of data verified` + `Hard resetting via RTS pin...`，烧完板子自动复位；
- **ESP32-S31 ROM 不支持整片 `erase-flash`**（报 `does not support function erase_flash`），不要用；直接覆盖烧录即可；
- 进下载模式偶发 `Invalid head of packet (0x78)` / `Failed to enter flash download mode`：多半是板子在跑旧固件占串口，等 3–4 秒或重插 USB 后重试；
- 波特率 460800；app 约 1.6MB，`app-flash` 几秒完成。

---

## 4. 抓串口日志（替代 monitor）

`idf.py monitor` 在 agent 的非交互 PowerShell 里必报：

```
Error: Monitor requires standard input to be attached to TTY.
```

替代方案：**用 pyserial 直接读 COM9**。`idf.py flash` 烧录结束会自动拉 RTS 复位并启动程序，烧录完直接跑脚本读串口即可，不需要再手动复位。

脚本见 `docs/_serial_capture.py`：

```python
import serial, time
s = serial.Serial('COM9', 115200, timeout=1)
end = time.time() + 12               # 读 12 秒
while time.time() < end:
    line = s.readline()
    if line:
        print(line.decode('utf-8', 'replace').rstrip())
s.close()
```

用 venv 里的 Python 跑（自带 pyserial）：

```powershell
& "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe" `
  "C:\Users\xiao1\Desktop\esp32\display_audio_photo\docs\_serial_capture.py"
```

> 想改抓多少秒，改脚本里的 `+ 12`。

---

## 5. 闭环操作模板

每次改完代码，按这个序列走：

```text
改代码
  ↓
idf.py build          （看 error: / warning:）
  ↓ 成功
idf.py -p COM9 flash  （看 Hash verified）
  ↓ 烧完
python _serial_capture.py
  ↓ 抓 12 秒
看日志：
  ├─ 看到 "Example initialization done." → 启动干净
  ├─ 看到 "Guru Meditation Error" → 定位 panic（见第 6 节）
  ├─ 看到预期的业务日志（如 "AUDIO_SVC: ch=1 ..."）→ 功能 OK
  └─ 静默无日志 → 串口被占用 / 波特率错 / 程序卡死
```

agent 后台跑时，把 build+flash 串成一条：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command ". 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'; Set-Location 'C:\Users\xiao1\Desktop\esp32\display_audio_photo'; idf.py -p COM9 flash"
```

跑完再单独跑 pyserial 脚本。`idf.py flash` 结束时板子会自动复位并启动，pyserial 直接读就能抓到完整启动日志，不需要再拉 RTS。

---

## 6. 常见 panic 速查

| 现象 | 根因 | 处理 |
|---|---|---|
| `Guru Meditation Error (Instruction access fault)` + 栈里大量 `0xa5a5a5a5` | **FreeRTOS 栈溢出** | 把任务栈加大（4096→8192）；栈帧里的大结构体（带 `char path[256]` 的命令结构）改 `static` |
| `StoreProhibited` / `LoadProhibited` | 野指针、NULL 解引用 | 看 PC 地址用 `addr2line` 反查 |
| `abort() was called` | assert 失败 / heap_caps_malloc 返回 NULL | 看日志里 assert 前一行 |
| `wdt: Task watchdog got triggered` | 某任务没喂狗 / 长循环没让出 CPU | 在长循环里加 `vTaskDelay(pdMS_TO_TICKS(1))` 或队列超时接收 |
| `Brownout detector was triggered` | 供电不足 | USB 线/口换一个，或外部供电 |

### addr2line 反查 PC 地址
```powershell
& "C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin\riscv32-esp-elf-addr2line.exe" `
  -e build\display_audio_photo.elf -f -C 0x<MEPC>
```

---

## 7. 当前工程已知约束（别重复踩）

- ES8389 MCLK=GPIO42 未连 → BSP 已配 `use_mclk=false`，TDM 帧格式；
- 推荐 16kHz / 16bit / mono，20ms 一帧 = 640B；
- 分区表（OTA 最终布局，无 factory）：nvs 24K / phy 4K / otadata 8K / **ota_0 3MB@0x20000 / ota_1 3MB@0x320000** / **storage littlefs 9.87MB@0x620000**；
- 文件系统：LittleFS（挂载点 `/littlefs`），素材目录 `littlefs_content/`；已去 `FLASH_IN_PROJECT`，日常不重烧 storage（见第 3 节）；
- **音频素材规范（重要）**：播放/录音通路固定 **16kHz / 16bit / mono**。新增 WAV 素材必须满足：
  1. 采样率 16000、单声道、16bit（44.1k/立体声在本板会失真）；
  2. **标准 44 字节 RIFF 头**：导出软件常插 `LIST/fact` chunk，工程里 `dumb_wav_header` 按固定偏移读 `data_size`，非标准头会把长度读错（曾出现 `size=26` 无声 / 数据错乱）；
  3. 用 `python -c`/wave 模块重写一遍即可自动得到标准头并完成下混+重采样；改完素材必须按 3.2 单独烧 storage。
- 板子不支持软件调 LCD 亮度（日志会打 `This board doesn't support to change brightness of LCD`）；
- 编译输出在 `build/`：`display_audio_photo.bin` + `bootloader.bin` + `partition-table.bin` + `storage.bin`（仅素材变更/首次时才烧后两项之外的 storage）。
