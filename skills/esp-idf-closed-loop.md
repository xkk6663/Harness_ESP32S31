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

> 当前分区（**唯一真源 = 工程根 `partitions.csv`；本分支 S5 v1.2，2026-09-28**）：
> `nvs 24KB(0x9000)` + `phy_init 4KB` + `otadata 8KB(0x11000)`
> + `ota_0 app 7MB(@0x20000)`（**单槽，已无 ota_1**）
> + `storage littlefs 8.875MB(@0x720000, size=0x8E0000)`，0x720000+0x8E0000=0x1000000 排满 16MB。
> 已去掉 `FLASH_IN_PROJECT`，默认 **不再重烧 storage 素材镜像**，板子上已有素材/录音保留。
> 原则：**只写这次真正变了的那一块**，其余不动。
> ★★ **改过 `partitions.csv` ⇒ 那一次必须 `idf.py flash`（含 0x8000 分区表），不能只用
>    `app-flash`** —— 否则设备留在旧表上，littlefs 会挂到旧地址并被静默格式化（血案见 3.4）。

### 3.1 选哪条命令（按改动内容对号入座）

| 这次改了什么 | 用哪条命令 | 烧写内容 | 耗时 |
|---|---|---|---|
| 只改了 `main/` 业务代码（最常见） | `idf.py -p COM9 app-flash` | 仅 ota_0 app | **~2–5 秒** |
| 改了 `sdkconfig` / bootloader / **`partitions.csv`** | `idf.py -p COM9 flash` | bootloader(0x2000)+partition(0x8000)+otadata(0x11000)+app(0x20000) | ~10–20 秒 |
| 改了 `littlefs_content/` 里的素材（壁纸/wav/models） | `idf.py -p COM9 storage-flash` | 仅 storage.bin@**0x720000**（8.875MB） | ~40–60 秒 |
| 换过分区表 / 新板子 / 整片擦除后首次 | `flash -a` + `storage-flash` | 全部 | ~1–2 分钟 |
| 阶段六 OTA 升级 | 不烧录，走 wifi TCP 写 app 槽 | — | — |

> 日常开发 99% 用第一条 `app-flash`。不要无脑 `flash` 更不要每次都写 storage。

### 3.2 单独烧 storage 素材镜像（仅素材变更/首次时）

`littlefs_create_partition_image(storage littlefs_content)`（根 CMakeLists 末行）**自带
`storage-flash` target**，偏移/大小由分区表自动推出，零硬编码：

```powershell
idf.py -p COM9 storage-flash      # ✅ 先生成 storage.bin，再按分区表偏移写入
```

- **构建期偏移真源** = `build/storage-flash_args`（当前应为 `0x720000 storage.bin`）。
  烧前扫一眼这一行，就知道这次写到哪，不用背数字、更别抄注释；
- ❌ `idf.py flash-storage` 是**错的**（unknown target，词序反了，旧笔记写反过）；
- 手动兜底（仅在 target 异常时用，地址同样以 `storage-flash_args` 为准）：

```powershell
cd build
& "C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe" -m esptool `
  --chip esp32s31 -p COM9 -b 921600 --before default-reset --after hard-reset `
  write-flash 0x720000 storage.bin
```

要点：参数顺序是 **`write-flash <地址> <文件>`**（地址在前）；921600 波特 + esptool 压缩约 40 秒写完 8.875MB。

### 3.3 注意事项
- 不要在这条命令后直接跟 `monitor`（见第 4 节）；
- 成功标志：`Hash of data verified` + `Hard resetting via RTS pin...`，烧完板子自动复位；
- **ESP32-S31 ROM 不支持整片 `erase-flash`**（报 `does not support function erase_flash`），不要用；直接覆盖烧录即可；
- 进下载模式偶发 `Invalid head of packet (0x78)` / `Failed to enter flash download mode`：多半是板子在跑旧固件占串口，等 3–4 秒或重插 USB 后重试；
- 波特率 921600；app 约 6.4MB（S5 起含小智），`app-flash` 十几秒完成。

### 3.4 ★★ 血案：分区表没烧 ⇒ littlefs "挂错地址" ⇒ 素材静默全灭（2026-09-28）

**症状**：日志满屏 `WP_PART: wallpaper not found` / `VISION: model file missing/empty`，
但 `FS_SVC` 挂载行**不报错**、app 不 panic、相机/网络/LVGL 全正常 ⇒ 很像"littlefs 挂到了错误地址"。

**真因**：`partitions.csv` 升级 S5 v1.2（storage `0xA20000/0x5E0000` → `0x720000/0x8E0000`）后，
只跑了 `app-flash` + `storage-flash`（新镜像写到**新**地址 0x720000），**没跑 `idf.py flash`**：
1. 设备 0x8000 处仍是**旧分区表** ⇒ app 按 label `"storage"` 拿到**旧地址 0xA20000**；
2. 0xA20000 处的内容已被新镜像（覆盖 0x720000..0x1000000）冲掉 ⇒ 不是合法 littlefs ⇒ 挂载失败；
3. `fs_service.c` 里 `format_if_mount_failed = true` ⇒ **就地格式化** ⇒ 挂载"成功"但**空 FS**；
4. 更糟：格式化范围 0xA20000..0x1000000 与刚写的新镜像**重叠** ⇒ 新素材也被擦掉一截。

**判据（一眼定案，不用读片）**：
- 开机 `FS_SVC: LittleFS mounted ... @0x?????? size=????KB` —— `@` 地址/容量与
  `partitions.csv` 不符 = 分区表没烧（旧表会显示 `size=6016KB`，新表 `size=9088KB`）；
- `build/partition_table/partition_table_flashed.bin` 是 IDF 快速重烧的比对副本
  （`--diff-with` + `ONLY_IF_DIFFERENT`）：**它的 mtime 早于 `partition-table.bin`
  ⇒ 上次烧录根本没写分区表**（IDF v6.1 默认"没变就不写"）。

**修复配方（顺序不可换）**：

```powershell
idf.py -p COM9 flash -a        # -a 关掉 fast-reflash，确保 0x8000 新分区表真的写进去
idf.py -p COM9 storage-flash   # 再重写素材镜像（上一步的错位挂载已污染过它）
```

**教训**：动过 `partitions.csv` 的那一次必须 `flash`；`format_if_mount_failed = true`
会把"分区表没烧"这类配置错误**静默降级**成"素材全丢"，排查时先看 FS_SVC 的 `@地址`，
别先怀疑素材没打包进镜像。

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

---

## 9. 推送远程前必做 Checklist

一旦要执行 git push，必须按顺序完成以下步骤，缺一不可：

1. 删除临时文件：根目录 _*.py、docs/_*.txt、docs/_*.py 等调试残留；
2. 删除旧日志：docs/_log_*.txt 全部删除；
3. 更新 docs/项目规划/改动记录.md：追加本轮日期、设备端/PC端改动清单、已知问题；
4. 更新 docs/项目规划/需求文档.md 和 todolist.md：标记完成项、新增后续项；
5. 遇到的重大问题写入 docs/工程规范/11_遇到的问题.md：现象/根因/解决方案/经验；
6. git add -A，commit 后 push；
7. SSH 失败时先确认代理已开，再重试；不要改 remote URL。
