# SD 卡 + ESP-GMF 音频 + MJPEG 视频 实施方案（基于 main 分支源码）

> 2026-09-20 | 基于 main 分支 ca85592
> 工程根：`C:\Users\xiao1\Desktop\esp32\display_audio_photo`

## 一、main 分支现状盘点

### 1.1 已有模块

| 模块 | 文件 | 关键能力 |
|---|---|---|
| 音频服务 | `middleware/audio_service.c/h` | 命令队列模型，`AUDIO_CMD_PLAY_FILE` 播 WAV，ES8389 I2S，16kHz mono |
| 文件系统 | `middleware/fs_service.c/h` | LittleFS 挂 `/littlefs`，JPEG 帧缓冲 |
| WAV 播放 UI | `ui/page_wav_player.c/h` | 已有 WAV 播放界面（但未挂到 tabview） |
| UI 管理 | `ui/ui_manager/page_manager.c` | 页面栈 |
| 主页面 | `ui/page_main.c` | 3 个 tab：Call / Record / Sys |
| 启动 | `app/main.c` | 初始化顺序：fs→bsp_display→audio_service→led→button→wifi→... |

### 1.2 分区表现状

```
ota_0   0x20000  4MB (0x400000)
ota_1   0x420000 4MB (0x400000)
storage 0x820000 8MB (littlefs)
```

### 1.3 idf_component.yml 现有依赖

```yaml
dependencies:
  esp32_s31_korvo_1: '*'
  esp_new_jpeg: ^1
  espressif/zlib: ^1.3.2~1
```

---

## 二、阶段 A：SD 卡挂载 + MP3 播放

### 2.1 新增 SD 卡服务

**新文件**：`main/middleware/sdcard_service.c/h`

```c
// sdcard_service.h
esp_err_t sdcard_service_mount(void);   // 挂载到 /sdcard
bool sdcard_service_is_mounted(void);
```

实现要点：
- 用 `esp_vfs_fat_sdmmc_mount()`，SDIO 4-bit
- GPIO20-25（官方文档确认，BSP 已封装）
- 挂载到 `/sdcard`
- 失败不阻塞启动（无 SD 卡也能跑）

### 2.2 改 main.c 初始化顺序

在 `app/main.c` 里 `fs_service_mount()` 之后加：

```c
ESP_ERROR_CHECK(fs_service_mount());
sdcard_service_mount();          // ← 新增，失败只打日志不阻塞
bsp_i2c_init();
bsp_display_start();
```

### 2.3 引 ESP-GMF 组件

**改 `main/idf_component.yml`**：

```yaml
dependencies:
  esp32_s31_korvo_1: '*'
  esp_new_jpeg: ^1
  espressif/zlib: ^1.3.2~1
  espressif/esp-gmf: '*'                    # ← 新增
  espressif/esp_audio_simple_player: '^1.0.0~2'  # ← 新增
```

### 2.4 新增音乐 Tab

**新文件**：`main/ui/tab_music.c/h`

在 `page_main.c` 的 `tabview_add_tab` 里加第 4 个 tab：

```c
// page_main.c 现有：
lv_obj_t *tab_call = lv_tabview_add_tab(s_tabview, LV_SYMBOL_AUDIO"  CALL");
lv_obj_t *tab_rec  = lv_tabview_add_tab(s_tabview, LV_SYMBOL_FILE"  REC");
lv_obj_t *tab_sys  = lv_tabview_add_tab(s_tabview, LV_SYMBOL_SETTINGS"  SYS");

// 改为加：
lv_obj_t *tab_music = lv_tabview_add_tab(s_tabview, LV_SYMBOL_LIST"  MUSIC");
```

`tab_music_build()` 职责：
- 列出 `/sdcard/music/*.mp3` 文件
- 上一首/播放暂停/下一首按钮
- 进度条
- 调用 ESP-GMF `esp_audio_simple_player_play("/sdcard/music/xxx.mp3")`

### 2.5 改 CMakeLists.txt

```cmake
idf_component_register(SRCS
    ...现有...
    "middleware/sdcard_service.c"    # 新增
    "ui/tab_music.c"                 # 新增
  PRIV_REQUIRES ... fatfs)          # 加 fatfs
```

### 2.6 阶段 A 验证

- [ ] SD 卡插入自动挂载，日志打印 `/sdcard` 可用
- [ ] MUSIC tab 列出 mp3 文件
- [ ] 播放 mp3 无爆音
- [ ] 拔出 SD 卡不崩

---

## 三、阶段 B：MJPEG 视频播放

### 3.1 引视频组件

**改 `main/idf_component.yml`**：

```yaml
dependencies:
  ...现有...
  espressif/esp_video_render: '^1.0.0'   # ← 新增
```

### 3.2 新增动画片 Tab

**新文件**：`main/ui/tab_cartoon.c/h`

在 `page_main.c` 加第 5 个 tab：

```c
lv_obj_t *tab_cartoon = lv_tabview_add_tab(s_tabview, LV_SYMBOL_IMAGE"  ANIM");
```

播放逻辑：
- 列出 `/sdcard/videos/*.avi`
- 选一个 → `esp_video_render_play(path)`
- 硬件 JPEG 解码 → RGB565 → LVGL canvas
- PCM 音频走现有 audio_service I2S

### 3.3 改 CMakeLists.txt

```cmake
idf_component_register(SRCS
    ...现有...
    "ui/tab_cartoon.c"   # 新增
  )
```

### 3.4 视频转码（PC 端）

```powershell
ffmpeg -i input.mp4 `
  -c:v mjpeg -q:v 5 -r 15 -s 640x480 `
  -c:a pcm_s16le -ar 16000 -ac 1 `
  output.avi
```

SD 卡目录：
```
/sdcard/
├── music/   *.mp3
└── videos/  *.avi
```

---

## 四、固件体积预算

| 项目 | 大小 |
|---|---|
| 当前 main 固件 | ~3.5MB |
| ESP-GMF + MP3 解码 | +250KB |
| esp_video_render | +150KB |
| **预计** | **~3.9MB** |
| OTA 分区 | 4MB |
| **余量** | ~0.1MB ⚠️ |

**4MB OTA 分区很紧**，如果超了，把 storage 从 8MB 缩到 6MB，OTA 扩到 5MB：

```csv
ota_0,    app,  ota_0,   0x20000,  0x500000,   # 5MB
ota_1,    app,  ota_1,   0x520000, 0x500000,   # 5MB
storage,  data, littlefs, 0xA20000, 0x5E0000,  # ~5.8MB
```

---

## 五、文件改动清单（汇总）

| 文件 | 操作 |
|---|---|
| `main/middleware/sdcard_service.c/h` | 新建 |
| `main/ui/tab_music.c/h` | 新建 |
| `main/ui/tab_cartoon.c/h` | 新建（阶段 B） |
| `main/app/main.c` | 加 `sdcard_service_mount()` |
| `main/ui/page_main.c` | 加 MUSIC / ANIM tab |
| `main/idf_component.yml` | 加 gmf / simple_player / video_render |
| `main/CMakeLists.txt` | 加新源文件 + fatfs |
| `partitions.csv` | 可选，OTA 扩到 5MB |

---

## 六、风险

| 风险 | 对策 |
|---|---|
| ESP-GMF 和 UDP 语音抢 I2S | 互斥，播放音乐时不开语音通话 |
| SD 卡读取慢 | Class 10 卡，SDIO 4-bit |
| 固件超 4MB OTA | 改分区表 OTA→5MB |
| 音视频不同步 | 音频时钟为主，视频丢帧跟上 |
