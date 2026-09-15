# SPIFFS → LittleFS 迁移指南

> 适用仓库：[Harness_ESP32S31](https://github.com/xkk6663/Harness_ESP32S31)
> 目标芯片：ESP32-S31（ESP32_S31_KORVO_1 BSP）
> 适用 IDF 版本：ESP-IDF v5.x / v6.x（本仓库当前为 v6.1）
> 编写日期：2026-09-15

---

## 1. 背景与目标

### 1.1 为什么迁移

| 维度 | SPIFFS（现状） | LittleFS（目标） |
|---|---|---|
| 维护状态 | ESP-IDF 官方已标记弃用 | 持续维护，乐鑫官方推荐 |
| 掉电安全 | 弱，意外断电易损坏文件系统 | 元数据双备份 + 原子提交，断电不易崩 |
| 磨损均衡 | 静态磨损均衡 | 动态全局磨损均衡，Flash 寿命更长 |
| 目录支持 | 无真实目录，靠文件名前缀模拟 | 原生多级目录 |
| 小文件性能 | 碎片多，GC 代价高 | GC 轻量，大量小文件更友好 |

本项目存储分区为 5 MB，内含 JPEG 壁纸、WAV 测试音频，运行时还会写入 `recording.wav`（录音），属于"静态资源 + 频繁小文件写入"场景，正是 LittleFS 的优势区间。

### 1.2 迁移目标

- 把 `storage` 分区从 SPIFFS 切换为 LittleFS；
- 业务代码（LVGL 读壁纸、音频服务播 WAV / 录音）**零改动或极小改动**；
- 静态资源镜像（`spiffs_content/`）继续随固件烧录；
- 保留 `format_if_mount_failed` 自动格式化能力。

---

## 2. 现状盘点（基于仓库实际代码）

迁移前先明确当前仓库里所有与 SPIFFS 耦合的位置，共 **7 处**：

| # | 文件 | 现状代码 | 作用 |
|---|---|---|---|
| 1 | `partitions.csv` | `storage, data, spiffs, 0x210000, 0x500000,` | 分区表，subtype 为 `spiffs` |
| 2 | `CMakeLists.txt`（根目录） | `spiffs_create_partition_image(storage spiffs_content FLASH_IN_PROJECT)` | 把 `spiffs_content/` 打包成 SPIFFS 镜像烧录 |
| 3 | `main/CMakeLists.txt` | `PRIV_REQUIRES spiffs ...` | 组件依赖声明 |
| 4 | `main/idf_component.yml` | 仅依赖 `esp32_s31_korvo_1`、`esp_new_jpeg` | 未声明 littlefs 组件 |
| 5 | `main/app/main.c` | `bsp_spiffs_mount();` | 启动时挂载文件系统（BSP 封装） |
| 6 | `main/middleware/fs_service.h` | `#define FS_MNT_PATH BSP_SPIFFS_MOUNT_POINT` | 挂载点宏，壁纸路径用它拼接 |
| 7 | `main/middleware/audio_service.h` | `#define REC_FILENAME BSP_SPIFFS_MOUNT_POINT"/recording.wav"` | 录音文件路径 |

**间接耦合点**（无需改代码，但需知道）：

- `main/ui/overlay_wallpaper.c`：`WALLPAPER_PATH = FS_MNT_PATH"/xwkkk.jpg"`，用 POSIX `open()/read()` 读 JPEG。**只要 `FS_MNT_PATH` 宏改了，这里自动适配。**
- `main/ui/tab_record.c`：UI 提示文字硬编码 `"/spiffs/recording.wav"`，需要顺手改成新挂载点。
- `spiffs_content/` 目录：内含 `test_16kHz.wav`、`xwkkk.jpg`，是编译期打包的静态资源。

> 业务层全部走标准 POSIX / C stdio（`fopen/fread/fwrite/open/read/stat`），这正是 VFS 的好处——**文件系统换了，读写 API 不变**。

---

## 3. 迁移步骤

### 步骤 1：修改分区表 `partitions.csv`

把 `storage` 分区的 subtype 从 `spiffs` 改为 `littlefs`：

```csv
# Name,   Type, SubType, Offset,  Size, Flags
# Note: if you change the phy_init or app partition offset, make sure to change the offset in Kconfig.projbuild
nvs,      data, nvs,     0x9000,  0x6000,
phy_init, data, phy,     0xf000,  0x1000,
factory,  app,  factory, 0x10000, 2M,
storage,  data, littlefs, 0x210000,0x500000,
```

> Offset 和 Size 保持不变（`0x210000` / `0x500000`），只改 subtype。这样不需要重新规划 Flash 布局。

---

### 步骤 2：添加 esp_littlefs 组件依赖

编辑 `main/idf_component.yml`，在 `dependencies` 下追加 `espressif/esp_littlefs`：

```yaml
dependencies:
  esp32_s31_korvo_1:
    version: '*'
  esp_new_jpeg: ^1
  espressif/esp_littlefs: "^2"
description: BSP Display Audio Photo Example
```

> 版本号 `^2` 对应 esp_littlefs v2.x 系列（适配 IDF v5.x/v6.x）。执行 `idf.py reconfigure` 时会自动从组件管理器下载。

---

### 步骤 3：修改根目录 `CMakeLists.txt`

把 SPIFFS 的镜像打包函数换成 LittleFS 的：

```cmake
cmake_minimum_required(VERSION 3.5)

set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(display_audio_photo)

# 旧：spiffs_create_partition_image(storage spiffs_content FLASH_IN_PROJECT)
littlefs_create_partition_image(storage spiffs_content FLASH_IN_PROJECT)
```

说明：
- 函数名从 `spiffs_create_partition_image` 改为 `littlefs_create_partition_image`；
- 第一个参数 `storage` 是分区表中的分区名，保持不变；
- 第二个参数 `spiffs_content` 是源目录名，**可以保留不改**（里面就是静态资源文件，目录名不影响功能）。如果你想让命名更语义化，也可以把目录改名为 `littlefs_content`，但需要同时 `git mv spiffs_content littlefs_content` 并同步这里的参数。本指南推荐**保留目录名**，减少无关改动。
- `FLASH_IN_PROJECT` 表示镜像随 `idf.py flash` 一起烧录，行为与原来一致。

---

### 步骤 4：修改 `main/CMakeLists.txt` 的组件依赖

把 `PRIV_REQUIRES` 里的 `spiffs` 换成 `esp_littlefs`：

```cmake
idf_component_register(SRCS
                        "app/main.c"
                        "middleware/audio_service.c"
                        "middleware/fs_service.c"
                        "middleware/led_service.c"
                        "middleware/button_service.c"
                        "middleware/wifi_manager.c"
                        "3rd/ringbuf/ringbuf.c"
                        "ui/ui_manager/page_manager.c"
                        "ui/overlay_manager.c"
                        "ui/ui_common.c"
                        "ui/page_splash.c"
                        "ui/page_main.c"
                        "ui/tab_call.c"
                        "ui/tab_record.c"
                        "ui/tab_sys_entry.c"
                        "ui/overlay_wallpaper.c"
                        "ui/page_wav_player.c"
                        "ui/page_wifi.c"
                        "ui/page_bluetooth.c"
                        "ui/page_rgb.c"
                        "ui/page_system.c"
                      INCLUDE_DIRS "."
                                    "ui/ui_manager"
                      PRIV_REQUIRES esp_littlefs esp_wifi esp_event esp_netif nvs_flash)
```

> BSP（`esp32_s31_korvo_1`）仍会间接拉取 spiffs 组件用于它自己的 `bsp_spiffs_mount()`，但你的 `main` 组件不再直接依赖 spiffs。

---

### 步骤 5：替换挂载代码（核心改动）

#### 5.1 修改 `main/middleware/fs_service.h`

把挂载点宏从 BSP 的 SPIFFS 宏改为自定义的 LittleFS 挂载点：

```c
/* 旧：
#define FS_MNT_PATH     BSP_SPIFFS_MOUNT_POINT
*/

/* 新：统一挂载点，业务代码全部通过此宏拼接路径 */
#define FS_MNT_PATH     "/littlefs"
```

同时在头文件顶部增加挂载接口声明（见 5.3）。

#### 5.2 修改 `main/middleware/audio_service.h`

录音文件路径不再直接引用 BSP 的 SPIFFS 宏，改为复用 `FS_MNT_PATH`：

```c
/* 旧：
#define REC_FILENAME    BSP_SPIFFS_MOUNT_POINT"/recording.wav"
*/

/* 新： */
#include "middleware/fs_service.h"
#define REC_FILENAME    FS_MNT_PATH"/recording.wav"
```

> 这样做的好处：以后如果再改挂载点，只需要改 `fs_service.h` 一处。

#### 5.3 在 `fs_service.c` 中实现 LittleFS 挂载

把原来由 BSP 封装的 `bsp_spiffs_mount()` 替换为直接调用 esp_littlefs API。

**`main/middleware/fs_service.h`** 增加声明：

```c
/**
 * @brief 挂载 LittleFS 文件系统到 FS_MNT_PATH
 * @note  在 app_main() 最早期调用，先于任何文件读写
 */
esp_err_t fs_service_mount(void);
```

（需要在头文件顶部 `#include "esp_err.h"`。）

**`main/middleware/fs_service.c`** 增加实现：

```c
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_littlefs.h"      /* ← 新增 */
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "esp_jpeg_dec.h"
#include "fs_service.h"

static const char *TAG = "FS_SVC";

uint8_t *file_buffer = NULL;
size_t file_buffer_size = 0;

/* ===== 新增：LittleFS 挂载 ===== */
esp_err_t fs_service_mount(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = FS_MNT_PATH,          /* "/littlefs" */
        .partition_label = "storage",       /* 与 partitions.csv 中的分区名一致 */
        .format_if_mount_failed = true,     /* 首次挂载 / 格式损坏时自动格式化 */
        .read_only = false,                 /* 需要写入 recording.wav，不能只读 */
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find LittleFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
        }
        return ret;
    }

    /* 打印一下分区信息，方便调试 */
    size_t total = 0, used = 0;
    esp_littlefs_info(conf.partition_label, &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted at %s, partition '%s', total=%uKB used=%uKB",
             FS_MNT_PATH, conf.partition_label,
             (unsigned)(total / 1024), (unsigned)(used / 1024));
    return ESP_OK;
}
```

> `esp_vfs_littlefs_conf_t` 的字段名以你安装的 esp_littlefs 版本为准。v2.x 版本字段为 `base_path / partition_label / format_if_mount_failed / read_only / dont_mount`。如果编译报字段不存在，用 `idf.py menuconfig` 查看组件说明，或查阅 [esp_littlefs 文档](https://github.com/joltwallet/esp_littlefs)。

#### 5.4 修改 `main/app/main.c`

把 `bsp_spiffs_mount()` 替换为新的挂载函数：

```c
void app_main(void)
{
    /* 旧：bsp_spiffs_mount(); */
    ESP_ERROR_CHECK(fs_service_mount());   /* ← 新增：挂载 LittleFS */

    bsp_i2c_init();
    bsp_display_start();
    fs_service_init();
    audio_service_init();
    /* ... 其余不变 ... */
}
```

> 注意挂载顺序：必须在 `bsp_display_start()` 之后、任何读取 `/littlefs/xwkkk.jpg` 的代码之前。当前 `fs_service_init()` 只分配 JPEG 缓冲，不读文件；壁纸解码发生在 `overlay_wallpaper_init()` → `PageManager_Load(&Page_Main)` 时，所以放在最前面最安全。

---

### 步骤 6：修改 UI 提示文字

`main/ui/tab_record.c` 底部有一行硬编码提示：

```c
/* 旧： */
lv_obj_set_text_static(hint, "5s local test  |  /spiffs/recording.wav");

/* 新： */
lv_obj_set_text_static(hint, "5s local test  |  /littlefs/recording.wav");
```

---

### 步骤 7：清理 sdkconfig（可选但推荐）

执行一次全量重新配置，让构建系统清理掉旧的 SPIFFS Kconfig 选项：

```bash
idf.py fullclean
idf.py reconfigure
```

`idf.py menuconfig` 中可以检查：
- `Component config → SPIFFS Configuration` 相关选项不再影响本项目（因为 main 组件已不依赖 spiffs）；
- `Component config → LittleFS Configuration` 出现，默认参数即可（块大小、读缓冲等保持默认）。

> 本项目不需要在 `sdkconfig.defaults` 里额外加 LittleFS 配置。

---

## 4. 不需要改动的文件（确认清单）

以下文件**不需要修改**，因为它们要么走标准 POSIX API，要么已经通过 `FS_MNT_PATH` 宏间接引用：

| 文件 | 为什么不用改 |
|---|---|
| `main/ui/overlay_wallpaper.c` | `WALLPAPER_PATH` 用 `FS_MNT_PATH` 拼接，宏改了自动生效；`open()/read()` 是 VFS 标准调用 |
| `main/middleware/audio_service.c` | `fopen/fread/fwrite/fseek` 全部是 stdio，VFS 透明 |
| `main/ui/page_wav_player.c` | 只把 path 字符串透传给 audio_service，不关心底层 FS |
| `spiffs_content/` 目录及其中的 `xwkkk.jpg`、`test_16kHz.wav` | 作为镜像源目录，内容不变 |
| `main/ui/page_wifi.c` / `page_bluetooth.c` / `page_rgb.c` / `page_system.c` | 不直接操作文件系统（WiFi 凭证走 NVS） |

---

## 5. 数据迁移说明

### 5.1 静态资源（壁纸、测试音频）

`xwkkk.jpg`、`test_16kHz.wav` 是编译期通过 `littlefs_create_partition_image(... FLASH_IN_PROJECT)` 打包进镜像的，每次 `idf.py flash` 都会重新写入，**不需要手动迁移**。

### 5.2 运行时生成的 `recording.wav`

- 旧设备上 SPIFFS 里可能存有用户之前录的 `recording.wav`；
- SPIFFS 与 LittleFS 二进制格式不兼容，**无法直接读取**；
- 由于 `format_if_mount_failed = true`，首次挂载 LittleFS 时会自动把 `storage` 分区格式化为 LittleFS，旧数据自然丢失；
- 本项目录音只是 5 秒测试音，丢失可接受。

> 如果未来需要保留用户录音，需要在固件升级时增加一段"从旧 SPIFFS 读出 → 写入新 LittleFS"的一次性迁移逻辑。当前阶段不需要。

---

## 6. 验证步骤

烧录后按以下顺序验证：

1. **编译通过**
   ```bash
   idf.py build
   ```
   确认没有 `spiffs` 相关的链接错误，且 `esp_littlefs` 被正确拉取。

2. **首次烧录**
   ```bash
   idf.py -p /dev/ttyUSB0 flash monitor
   ```

3. **串口日志检查**，应看到类似：
   ```
   I (xxx) FS_SVC: LittleFS mounted at /littlefs, partition 'storage', total=5120KB used=xxxKB
   ```
   如果看到 `Failed to mount or format filesystem`，检查分区表 subtype 是否改成了 `littlefs`。

4. **壁纸显示**：开机进入主界面后，下拉调出壁纸 overlay，确认 `xwkkk.jpg` 正常解码显示。日志应出现：
   ```
   I (xxx) WP_OVERLAY: wallpaper 800x480 decoded
   ```

5. **WAV 播放**：进入 REC tab，点击 Play，确认 `recording.wav`（或镜像里的 `test_16kHz.wav`）能正常出声。

6. **录音写入**：点击 REC 录 5 秒，再点 Play 回放，确认 `fopen(path, "wb")` 写入成功（LittleFS 可写）。

7. **掉电稳定性**：录音过程中直接断电，重新上电后文件系统应能自动恢复挂载（LittleFS 的原子提交优势），不会出现 SPIFFS 那种挂载失败需要手动擦除的情况。

---

## 7. 常见问题与注意事项

### Q1：BSP 里的 `bsp_spiffs_mount()` 还在，会不会冲突？
不会。你在 `main.c` 里不再调用它即可。BSP 组件本身保留这个函数不影响编译，只是没人调用。如果 BSP 内部在初始化时自动挂载了 SPIFFS（检查 BSP 源码确认），需要在 `menuconfig` 里关掉对应的 BSP 配置项，或确保两者挂载到不同挂载点、不同分区。

### Q2：分区大小需要调整吗？
不需要。`0x500000`（5 MB）对 LittleFS 完全够用。LittleFS 的元数据开销比 SPIFFS 略大，但在 MB 级别分区上可忽略。

### Q3：Flash 使用率多少合适？
建议保持在 70% 以下。LittleFS 在空间紧张时 GC 会变慢，和 SPIFFS 类似。当前静态资源只有两张小文件，空间充裕。

### Q4：能不能同时挂载 SPIFFS 和 LittleFS？
技术上可以（划两个分区），但本项目**不推荐**——会增加分区表复杂度、ROM 占用和维护成本。一个 LittleFS 分区同时放静态资源和用户配置即可。

### Q5：OTA 升级后旧固件写的文件还在吗？
`storage` 是 data 分区，OTA 只替换 factory app 分区，data 分区不动。但由于文件系统格式从 SPIFFS 变成了 LittleFS，首次升级后挂载会失败 → 自动格式化 → 旧文件丢失。这是预期行为，见第 5 节。

### Q6：`esp_littlefs_info()` 链接报错？
确认 `main/CMakeLists.txt` 的 `PRIV_REQUIRES` 里已经加了 `esp_littlefs`。

---

## 8. 改动文件汇总（Checklist）

> 说明：本节是 **SPIFFS→LittleFS 文件系统迁移**的过渡布局（storage 仍为 5 MB）。若你同时要做 OTA（方案二），不必分两步——**直接按 10.9 步骤 1 的最终分区表一次性替换 `partitions.csv`**，本节其余文件改动（组件依赖、fs_service、main.c、tab_record.c）保持不变。下文第 10.9 步骤已给出最终版完整改动。

按顺序提交以下改动：

- [ ] `partitions.csv`：subtype `spiffs` → `littlefs`（或直接采用 10.9 步骤 1 的 OTA 最终布局）
- [ ] `main/idf_component.yml`：追加 `espressif/esp_littlefs: "^2"`
- [ ] `CMakeLists.txt`（根目录）：`spiffs_create_partition_image` → `littlefs_create_partition_image`
- [ ] `main/CMakeLists.txt`：`PRIV_REQUIRES` 中 `spiffs` → `esp_littlefs`
- [ ] `main/middleware/fs_service.h`：`FS_MNT_PATH` 改为 `"/littlefs"`，新增 `fs_service_mount()` 声明
- [ ] `main/middleware/fs_service.c`：新增 `fs_service_mount()` 实现（调用 `esp_vfs_littlefs_register`）
- [ ] `main/middleware/audio_service.h`：`REC_FILENAME` 改用 `FS_MNT_PATH`
- [ ] `main/app/main.c`：`bsp_spiffs_mount()` → `ESP_ERROR_CHECK(fs_service_mount())`
- [ ] `main/ui/tab_record.c`：提示文字 `/spiffs/` → `/littlefs/`
- [ ] 执行 `idf.py fullclean && idf.py build` 验证编译
- [ ] 烧录后按第 6 节逐项验证

---

## 9. 回滚方案

如果迁移后出现问题需要回退到 SPIFFS：

1. `git checkout` 上述 9 个文件；
2. `idf.py fullclean && idf.py build flash`；
3. 由于分区表 subtype 改回了 `spiffs`，首次烧录会重新烧录分区表，旧 LittleFS 数据会被 SPIFFS 镜像覆盖，无残留。

> 建议在一个独立分支上做这次迁移，验证通过后再合入主干。

---

## 10. OTA 分区设计与跨文件系统迁移方案

### 10.1 Flash 空间盘点

芯片为 ESP32-S31，Flash 容量 **16 MB**（`CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`）。

当前（迁移前）分区占用：

| 分区 | 起止地址 | 大小 |
|---|---|---|
| bootloader + partition table | `0x00000 ~ 0x09000` | 36 KB |
| nvs | `0x09000 ~ 0x0F000` | 24 KB |
| phy_init | `0x0F000 ~ 0x10000` | 4 KB |
| factory app | `0x10000 ~ 0x210000` | 2 MB |
| storage (littlefs) | `0x210000 ~ 0x710000` | 5 MB |
| **已用合计** | | **~7.06 MB** |
| **空闲** | `0x710000 ~ 0x1000000` | **~8.9 MB** |

OTA 需要新增：`otadata`（启动标记，8 KB）+ 两个 OTA app 槽位（各 3 MB）。空间足够，方案二见 10.2。

---

### 10.2 OTA 分区表规划（方案二：纯 A/B 双槽，无 factory）

采用 **ota_0 + ota_1** 双槽位方案（无 factory 分区）：首次烧录直接写入 ota_0，OTA 升级在 ota_0/ota_1 之间交替，otadata 记录当前启动槽位，新固件启动失败自动回滚旧槽位。牺牲 52 KB 对齐填充，换取 **3 MB App 槽 + ~9.87 MB LittleFS**。

#### 最终 `partitions.csv`

```csv
# Name,   Type, SubType, Offset,   Size,     Flags
# Note: if you change the phy_init or app partition offset, make sure to change the offset in Kconfig.projbuild
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
otadata,  data, ota,     0x11000,  0x2000,
ota_0,    app,  ota_0,   0x20000,  0x300000,
ota_1,    app,  ota_1,   0x320000, 0x300000,
storage,  data, littlefs, 0x620000, 0x9E0000,
```

#### 布局一览（16 MB Flash，全盘用尽）

```
0x000000  +----------------------------+
          |  bootloader (32K)          |
0x008000  +----------------------------+
          |  partition table (4K)      |
0x009000  +----------------------------+
          |  nvs (24K)                 |  ← WiFi/BT 凭证、OTA 断点、系统配置
0x00F000  +----------------------------+
          |  phy_init (4K)             |  ← 射频校准参数
0x010000  +----------------------------+
          |  (对齐填充 4K)             |
0x011000  +----------------------------+
          |  otadata (8K)              |  ← A/B 双扇区启动标记
0x013000  +----------------------------+
          |  (对齐填充 52K)            |  ← app 分区 64K 对齐要求
0x020000  +----------------------------+
          |  ota_0 (3M)                |  ← 运行槽（出厂固件烧这里）
0x320000  +----------------------------+
          |  ota_1 (3M)                |  ← 备用槽 / 回滚
0x620000  +----------------------------+
          |  storage / littlefs (~9.87M)| ← 壁纸、WAV、录音（data 分区，OTA 不动）
0x1000000 +----------------------------+
```

#### 空间账（精确验算）

| 分区 | 大小 | 说明 |
|---|---|---|
| nvs | 24 KB | `0x6000` |
| phy_init | 4 KB | `0x1000` |
| otadata | 8 KB | `0x2000` |
| ota_0 | 3 MB | `0x300000` |
| ota_1 | 3 MB | `0x300000` |
| storage | **0x9E0000 = 10,354,688 B ≈ 9.87 MiB** | `0x1000000 - 0x620000` |
| **合计** | | **16 MB 整**，全盘用尽，无空闲 |

**LittleFS 容量对账（按你的 5 分钟 PCM 需求）：**
- PCM 码率：16 kHz × 16 bit × 1ch = 32,000 B/s ≈ 31.25 KiB/s
- 5 分钟录音：31.25 × 300 ≈ **9,375 KiB ≈ 9.15 MiB**
- storage 剩余：9.87 - 9.15 ≈ **0.72 MiB**（≈ 737 KiB）用于壁纸/字体/静态资源
- 当前静态资源仅 `xwkkk.jpg` + `test_16kHz.wav`（约几百 KB），够用但不算宽裕

#### 关键设计决策

1. **App 槽位 3 MB**：当前固件约 2.27 MB，剩余 ~0.73 MB 余量，为后续 LVGL 组件/功能迭代留空间（这正是你选方案二的原因）。
2. **无 factory**：`idf.py flash` 首次烧录时固件自动写入 **ota_0**（ESP-IDF 行为：无 factory 分区时 app 默认烧第一个 ota 槽位），otadata 为空时 bootloader 默认从 ota_0 启动。省下 2 MB 给 storage。
3. **otadata 放 0x11000**：紧贴 phy_init，8 KB 双扇区；0x13000~0x20000 是 64 KB 对齐填充，属正常开销（52 KB）。
4. **storage = 0x9E0000 正好用完 16 MB**：不再有方案一的 4.9 MB 空闲，后续扩容只能靠压缩 App 槽或减小 storage（见 10.7）。
5. **两个 App 槽大小必须一致**（都是 3 MB），否则 OTA 写不进。
6. **LittleFS 分区对齐**：`0x620000` 是 4 KB 对齐，`0x9E0000` 大小满足分区 4 KB 对齐要求。

> ⚠️ 若未来固件逼近 3 MB（当前 2.27 MB，余 0.73 MB），需同时收缩：App 槽降到 2.5 MB（`0x280000`）并把 storage 扩到 ~10.4 MB；或减小 storage 换 App 空间。两者必须同步调整 offset。

---

### 10.3 OTA 相关配置改动

#### 10.3.1 `sdkconfig.defaults` 追加

```ini
## OTA ##
CONFIG_BOOTLOADER_OFFSET_IN_RESERVED_HEADER=y
# 分区表已在 partitions.csv 自定义（原已有 CONFIG_PARTITION_TABLE_CUSTOM=y）
# OTA 启用后，bootloader 自动支持多槽位选择，无需额外开关
```

> 本项目已设 `CONFIG_PARTITION_TABLE_CUSTOM=y` 指向 `partitions.csv`，分区表改完后 `idf.py reconfigure` 即可识别 otadata/ota_0/ota_1。

#### 10.3.2 组件依赖

OTA 本身走 `esp_ota_ops`、`esp_app_format`、`esp_http_client`（如果走 HTTPS OTA），这些是 ESP-IDF 内置组件，不需要在 `idf_component.yml` 里加第三方依赖。在 `main/CMakeLists.txt` 的 `PRIV_REQUIRES` 追加：

```cmake
PRIV_REQUIRES esp_littlefs esp_wifi esp_event esp_netif nvs_flash
              app_update esp_http_client esp_https_ota
```

#### 10.3.3 OTA 业务代码

完整可编译的 `middleware/ota_service.c` 骨架见 **10.9 步骤 5**（含 `esp_https_ota` 下载、`esp_ota_set_boot_partition` 切换、`esp_ota_mark_app_valid_cancel_rollback` 确认回滚）。核心流程：

```c
/* 标准 OTA 升级流程（伪代码，完整版见 10.9 步骤 5） */
esp_err_t ota_service_update(const char *url)
{
    esp_https_ota_config_t ota_config = { .http_config = { .url = url } };

    /* 1. 选中非当前运行的槽位（ota_0 <-> ota_1 交替） */
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

    /* 2. 下载固件并写入分区（esp_https_ota 内部自动 begin/write/end） */
    esp_err_t ret = esp_https_ota(&ota_config);

    /* 3. 校验合法后设置启动分区并重启 */
    esp_ota_set_boot_partition(update_partition);
    esp_restart();
    return ESP_OK;
}
```

> storage 是 data 分区，OTA 写 app 分区时**完全不触碰 storage**，这是后面跨文件系统迁移策略的前提。

---

### 10.4 跨文件系统 OTA 迁移策略（SPIFFS → LittleFS）

这是本次 OTA 设计的核心风险点。场景：

```
已出厂设备：跑旧固件（SPIFFS），storage 分区是 SPIFFS 格式
            ↓ OTA 推新固件（LittleFS）到 ota_0
新固件启动：bootloader 切到 ota_0，尝试挂载 /littlefs
            ↓
        storage 分区还是 SPIFFS 格式，LittleFS 挂载失败
```

#### 方案 A：自动格式化（推荐，当前项目采用）

`fs_service_mount()` 里 `format_if_mount_failed = true`，新固件首次启动时：

1. LittleFS 检测到分区不是自己的格式 → 挂载失败；
2. 自动擦除整个 storage 分区，格式化为 LittleFS；
3. 重新挂载成功；
4. `littlefs_create_partition_image(... FLASH_IN_PROJECT)` 烧录的静态资源**不会**在 OTA 后自动写入（因为 OTA 不重写 data 分区），所以这里需要应用层补一次静态资源初始化。

**需要补的一步**：新固件首次启动后，检测 storage 是否为空（比如检查 `/littlefs/xwkkk.jpg` 是否存在），如果不存在，则从固件内嵌的资源（或 ROM 里的镜像）把壁纸、测试音频写进去。实现方式：

- 把 `xwkkk.jpg`、`test_16kHz.wav` 用 `#embed`（GCC 13+）或 EMBED_FILES 编译进固件 binary；
- 挂载后若文件缺失，`fopen("/littlefs/xxx", "wb")` 把内嵌资源写一遍。

```cmake
# main/CMakeLists.txt 中嵌入资源
target_add_binary_data(${COMPONENT_TARGET} "spiffs_content/xwkkk.jpg" BINARY)
target_add_binary_data(${COMPONENT_TARGET} "spiffs_content/test_16kHz.wav" BINARY)
```

```c
/* fs_service.c 挂载后调用一次（完整实现见 10.9 步骤 4） */
extern const uint8_t xwkkk_jpg_start[] asm("_binary_spiffs_content_xwkkk_jpg_start");
extern const uint8_t xwkkk_jpg_end[]   asm("_binary_spiffs_content_xwkkk_jpg_end");

static void restore_asset_if_missing(const char *dst, const uint8_t *start, size_t len)
{
    struct stat st;
    if (stat(dst, &st) == 0) return;   /* 已存在，不动 */
    FILE *f = fopen(dst, "wb");
    if (!f) return;
    fwrite(start, 1, len, f);
    fclose(f);
    ESP_LOGI(TAG, "restored asset: %s (%u bytes)", dst, (unsigned)len);
}

/* 在 fs_service_mount() 末尾调用： */
restore_asset_if_missing(FS_MNT_PATH"/xwkkk.jpg", xwkkk_jpg_start,
                        xwkkk_jpg_end - xwkkk_jpg_start);
restore_asset_if_missing(FS_MNT_PATH"/test_16kHz.wav", test_16kHz_wav_start,
                         test_16kHz_wav_end - test_16kHz_wav_start);
```

> 符号名规则：`target_add_binary_data` 相对 `main/` 组件目录生成符号，`spiffs_content/xwkkk.jpg` → `_binary_spiffs_content_xwkkk_jpg_start`。若链接报未定义，用 `nm build/main/libmain.a | grep xwkkk` 确认实际符号名。

> 这样即使 OTA 后 storage 被格式化，静态资源也能自动恢复；而 `recording.wav`（用户录音）本来就是测试数据，丢了不影响功能。

#### 方案 B：保留旧数据的迁移路径（可选，后续量产用）

如果未来 `recording.wav` 或其他用户数据需要保留，新固件需要在挂载 LittleFS 前，先临时挂载 SPIFFS 把关键文件读出来：

```c
/* 仅在 OTA 迁移首版执行一次，完成后删除 */
static esp_err_t migrate_spiffs_to_littlefs(void)
{
    /* 1. 先尝试以 SPIFFS 挂载到 /spiffs_old */
    esp_vfs_spiffs_conf_t spiffs_conf = {
        .base_path = "/spiffs_old",
        .partition_label = "storage",
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    if (esp_vfs_spiffs_register(&spiffs_conf) != ESP_OK) {
        return ESP_ERR_NOT_SUPPORTED;   /* 不是 SPIFFS 格式，直接走 LittleFS */
    }

    /* 2. 把需要保留的文件（如 recording.wav）逐个拷到 PSRAM 临时缓冲 */
    /*    （文件不大，5 秒录音约 160 KB，PSRAM 放得下） */

    /* 3. 卸载 SPIFFS，格式化 storage 为 LittleFS */
    esp_vfs_spiffs_unregister("/spiffs_old");
    esp_partition_erase_range(esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                              ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, "storage"),
                              0, ESP_PARTITION_TABLE_OFFSET);  /* 注意：此处需用 storage 的实际 size */

    /* 4. 挂载 LittleFS，把临时缓冲里的文件写回去 */
    /* ... */
}
```

> 当前项目不需要这个复杂度，方案 A 足够。文档保留方案 B 作为量产阶段的升级路径。

---

### 10.5 OTA 升级后文件系统行为对照

| 场景 | storage 分区内容 | 新固件挂载结果 |
|---|---|---|
| 首次烧录（量产/开发） | littlefs 镜像（含壁纸、测试音频） | 直接挂载成功（固件在 ota_0） |
| 同版本 OTA 升级（LittleFS → LittleFS） | 旧 littlefs 数据不动 | 挂载成功，用户录音保留 |
| **跨格式 OTA（SPIFFS → LittleFS）** | 旧 SPIFFS 数据 | 挂载失败 → 自动格式化 → 补写静态资源（10.4 方案 A） |
| 掉电断电 | littlefs 原子提交保护 | 下次启动自动恢复，不损坏 |

---

### 10.6 首次切换分区表的烧录注意事项

**纯 A/B 方案没有 factory 分区**，首次烧录行为与之前不同：

```bash
# 第一次烧录新分区表（必须全片擦除，清掉旧 factory 分区残留）
idf.py -p /dev/ttyUSB0 erase-flash
idf.py -p /dev/ttyUSB0 flash monitor
```

说明：
- `idf.py flash` 在无 factory 分区时，会把 app 固件**自动烧录到 ota_0**（地址 `0x20000`），otadata 为空 → bootloader 默认启动 ota_0，开箱即用；
- 若想验证回滚路径，可手动把固件烧到备用槽：
  ```bash
  # 烧到 ota_1（备用槽），配合 otadata 测试切换
  esptool.py -p /dev/ttyUSB0 write_flash 0x320000 build/<project>.bin
  ```
- 之后日常 OTA 升级不需要再擦除，应用层走 `esp_ota_set_boot_partition()` 切换。

### 10.7 空间预留说明（方案二已全盘用尽）

方案二 16 MB **无空闲分区**，后续扩容只有三种途径：

1. **压缩 App 槽换取 storage**：若固件稳定在 ~2.3 MB，可把 ota_0/ota_1 缩到 2.5 MB（`0x280000`），storage 随之扩到 `0x1000000 - 0x620000 = 0x9E0000`（不变）→ 实际上 0x620000 起点的 storage 已经是 9.87 MB，App 槽压缩后 ota_1 结束地址前移，storage 起点才能前移增大。计算示例：App 槽 2.5 MB 时 ota_1 结束 `0x20000+0x280000*2 = 0x520000`，storage 起点 0x520000，大小 `0xAE0000` ≈ 10.4 MiB，5 min PCM 后剩余 ~1.3 MiB；
2. **压缩 storage 换 App**：未来固件逼近 3 MB 时，把 storage 减到 8 MB（`0x800000`），腾出 ~1.9 MB 给 App 槽（各 ~3.9 MB，需同步改两个槽位 offset）；
3. **日志分区**：当前无空闲，不建议再单独划日志分区，可用 LittleFS 内建目录存日志。

> 扩容硬约束：**两个 App 槽大小必须一致**；App 槽起点必须是 0x10000（64 KB）对齐；storage 起点必须 4 KB 对齐。

### 10.8 OTA 阶段 Checklist（在第 8 节基础上追加）

- [ ] `partitions.csv`：按 10.2 节更新为 otadata + ota_0 + ota_1 + storage(littlefs) 布局（无 factory）
- [ ] `main/CMakeLists.txt`：`PRIV_REQUIRES` 追加 `app_update esp_http_client esp_https_ota`
- [ ] `main/CMakeLists.txt`：用 `target_add_binary_data` 把 `xwkkk.jpg`、`test_16kHz.wav` 编译进固件
- [ ] `fs_service.c`：挂载后新增 `restore_asset_if_missing()` 逻辑，OTA 格式化后自动补回静态资源
- [ ] 新增 `middleware/ota_service.c/.h`（见 10.9 步骤 5，提供可编译骨架）
- [ ] 首次烧录执行 `idf.py erase-flash && idf.py flash`
- [ ] 验证：连续两次 OTA（ota_0 → ota_1 → ota_0）后，壁纸、录音功能正常
- [ ] 验证：跨格式升级（SPIFFS 固件 → LittleFS 固件）后自动格式化并恢复静态资源
- [ ] 验证：烧录损坏固件到备用槽，bootloader 自动回滚到原槽

---

### 10.9 结合当前源码的 OTA 改造步骤（方案二）

以下按你仓库 `Harness_ESP32S31` 的实际文件给出逐步改造。

#### 步骤 1：替换 `partitions.csv`（整表替换）

```csv
# Name,   Type, SubType, Offset,   Size,     Flags
# Note: if you change the phy_init or app partition offset, make sure to change the offset in Kconfig.projbuild
nvs,      data, nvs,     0x9000,   0x6000,
phy_init, data, phy,     0xf000,   0x1000,
otadata,  data, ota,     0x11000,  0x2000,
ota_0,    app,  ota_0,   0x20000,  0x300000,
ota_1,    app,  ota_1,   0x320000, 0x300000,
storage,  data, littlefs, 0x620000, 0x9E0000,
```

#### 步骤 2：`main/CMakeLists.txt` 两处改动

**① `PRIV_REQUIRES` 追加 OTA 组件**（`app_update` 提供 `esp_ota_ops`，`esp_http_client`/`esp_https_ota` 提供下载能力）：

```cmake
PRIV_REQUIRES esp_littlefs esp_wifi esp_event esp_netif nvs_flash
              app_update esp_http_client esp_https_ota
```

**② 嵌入静态资源**（在 `idf_component_register(...)` 之后追加，供 10.4 方案 A 的资源恢复使用）：

```cmake
# 嵌入壁纸与测试音频，OTA 格式化 storage 后可自动恢复
target_add_binary_data(${COMPONENT_TARGET} "spiffs_content/xwkkk.jpg" BINARY)
target_add_binary_data(${COMPONENT_TARGET} "spiffs_content/test_16kHz.wav" BINARY)
```

> `spiffs_content/` 目录名保留不改（见迁移文档第 3 步说明），只作为资源源目录。

#### 步骤 3：`main/idf_component.yml` 不需要改

`app_update`、`esp_http_client`、`esp_https_ota` 都是 ESP-IDF 内置组件，无需在 `dependencies` 声明；`espressif/esp_littlefs` 已在迁移步骤 2 添加。

#### 步骤 4：`main/middleware/fs_service.c` 增加资源恢复逻辑

在 `fs_service_mount()` 末尾（挂载成功后）追加（需 `#include <sys/stat.h>`）：

```c
#include <sys/stat.h>   /* stat() */

/* 固件内嵌资源（由 target_add_binary_data 生成符号） */
extern const uint8_t xwkkk_jpg_start[] asm("_binary_spiffs_content_xwkkk_jpg_start");
extern const uint8_t xwkkk_jpg_end[]   asm("_binary_spiffs_content_xwkkk_jpg_end");
extern const uint8_t test_16kHz_wav_start[] asm("_binary_spiffs_content_test_16kHz_wav_start");
extern const uint8_t test_16kHz_wav_end[]   asm("_binary_spiffs_content_test_16kHz_wav_end");

static void restore_asset_if_missing(const char *dst, const uint8_t *start, size_t len)
{
    struct stat st;
    if (stat(dst, &st) == 0) {
        return;                              /* 文件已存在，不覆盖 */
    }
    FILE *f = fopen(dst, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot create %s", dst);
        return;
    }
    fwrite(start, 1, len, f);
    fclose(f);
    ESP_LOGI(TAG, "restored asset: %s (%u bytes)", dst, (unsigned)len);
}

/* 在 fs_service_mount() 返回 ESP_OK 之前调用 */
restore_asset_if_missing(FS_MNT_PATH"/xwkkk.jpg",
                         xwkkk_jpg_start, xwkkk_jpg_end - xwkkk_jpg_start);
restore_asset_if_missing(FS_MNT_PATH"/test_16kHz.wav",
                         test_16kHz_wav_start, test_16kHz_wav_end - test_16kHz_wav_start);
```

> 符号名规则：`_binary_<相对路径转下划线>_start`，路径相对 `main/` 组件目录，所以 `spiffs_content/xwkkk.jpg` → `_binary_spiffs_content_xwkkk_jpg_start`。若编译链接报未定义，用 `idf.py build -v` 或 `nm build/main/libmain.a | grep xwkkk` 确认实际符号名。

#### 步骤 5：新增 `middleware/ota_service.c` / `ota_service.h`（可编译骨架）

**`main/middleware/ota_service.h`**：

```c
/**
 * @file ota_service.h
 * @brief 中间层：OTA 升级服务（esp_https_ota + esp_ota_ops）
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 从 URL 下载固件并升级到备用槽，成功后重启
 * @param url  HTTPS 固件下载地址（如 https://server/fw.bin）
 * @note  阻塞直到升级完成或失败；调用前需已联网
 */
esp_err_t ota_service_update(const char *url);

#ifdef __cplusplus
}
#endif
```

**`main/middleware/ota_service.c`**（核心逻辑，直接可编译）：

```c
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "ota_service.h"

static const char *TAG = "OTA_SVC";

esp_err_t ota_service_update(const char *url)
{
    if (url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "OTA start, url=%s", url);

    /* 1. 选中非当前运行槽位（ota_0 <-> ota_1 交替） */
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    assert(update_partition != NULL);
    ESP_LOGI(TAG, "writing to partition subtype %d at offset 0x%lx",
             update_partition->subtype, (unsigned long)update_partition->address);

    /* 2. HTTPS 下载并写入（内部自动调用 esp_ota_begin/write/end） */
    esp_https_ota_config_t ota_config = {
        .http_config = {
            .url = url,
            .timeout_ms = 10000,
            .buffer_size = 4096,
        },
    };
    esp_err_t ret = esp_https_ota(&ota_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 3. 校验新固件合法后切换启动槽并重启 */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_check_rollback_is_possible() &&
        esp_ota_get_state_partition(running, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    ret = esp_ota_set_boot_partition(update_partition);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "OTA done, rebooting...");
    esp_restart();
    return ESP_OK;
}
```

> 回滚机制：新槽首次启动处于 `PENDING_VERIFY` 状态，应用正常跑起来后调用 `esp_ota_mark_app_valid_cancel_rollback()` 确认；若新固件启动即崩溃/看门狗复位，bootloader 自动回滚到旧槽。建议在 `app_main()` 里启动后尽早调用确认函数（见步骤 6）。

#### 步骤 6：`main/CMakeLists.txt` 注册新源文件 + `main.c` 接入

**`main/CMakeLists.txt` 的 `SRCS` 列表追加**：

```cmake
"middleware/ota_service.c"
```

**`main/app/main.c` 两处接入**：

① 启动后尽早确认新固件有效（OTA 回滚机制必需）：

```c
#include "esp_ota_ops.h"          /* 新增 */
#include "middleware/ota_service.h"  /* 新增 */

void app_main(void)
{
    /* 新增：标记本次启动的新固件有效，避免被回滚（仅 OTA 升级后首启有意义） */
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    ESP_ERROR_CHECK(fs_service_mount());
    /* ... 其余保持不变 ... */
}
```

② 升级触发点：推荐在 `wifi_manager.c` 的 `WIFI_EVT_GOT_IP` 回调里（`main.c` 的 `on_wifi_evt` 已注册该回调）检查服务器版本并调 `ota_service_update(url)`；或后续在 `page_system.c` 增加"检查更新"UI 入口。本次改造先接入回调钩子：

```c
static void on_wifi_evt(wifi_evt_t evt, const char *info)
{
    switch (evt) {
    case WIFI_EVT_GOT_IP:
        led_service_set_status(LED_STATUS_CONNECTED);
        /* TODO(OTA): 在此检查服务器版本，命中新版本时调用
         *   ota_service_update("https://your-server/fw.bin");
         *   需自行实现版本比较逻辑（可读 nvs 或远端 json）
         */
        break;
    /* ... 其余分支不变 ... */
    }
}
```

#### 步骤 7：首次烧录与验证

```bash
idf.py fullclean
idf.py build
idf.py -p /dev/ttyUSB0 erase-flash        # 关键：清掉旧 factory 分区残留
idf.py -p /dev/ttyUSB0 flash monitor
```

验证顺序：
1. 串口日志出现 `FS_SVC: LittleFS mounted at /littlefs` 与 `restored asset: /littlefs/xwkkk.jpg`（首次）；
2. 壁纸显示、WAV 播放、录音回放正常；
3. 用 `esptool.py write_flash 0x320000 build/<project>.bin` 把同版本固件烧到 ota_1，重启后正常（验证双槽可用）；
4. 跨格式迁移验证：先烧旧 SPIFFS 固件（从 git 历史）→ 再 OTA/烧录 LittleFS 固件 → 确认自动格式化 + 资源恢复。
#（注：内容由AI生成）
