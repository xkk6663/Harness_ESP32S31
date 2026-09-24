# 阶段 B：MJPEG 视频播放方案

> ✅ **状态（2026-09-24 补注）：已实施** —— 落在 `ui/tabs/tab_video.c`（主界面 VIDEO tab）。
> ⚠ 同款遗留：该文件的音频出口仍在 **UI 层**直持 codec 句柄，见 `12_音频链路架构.md` §5.5。

## 目标
在 MUSIC tab 增加视频播放功能，播放 SD 卡里的 MJPEG 视频（动画片），全屏 800×480。

## 硬件能力
- ESP32-S31 内置 JPEG 硬件编解码器
- PPA（2D DMA）支持缩放/旋转/格式转换
- 16MB PSRAM 足够缓存帧

## 方案选择

### 方案 A：esp_player + esp_video_render（推荐）
直接用官方 video_player 例子的管线：
- **视频管线**：extractor → JPEG 解码 → PPA 缩放/色彩转换 → esp_video_render → LCD
- **音频管线**：（可选）如果视频带 AAC/MP3 音轨，走 audio_render → ES8389

**优点**：
- 官方组件，自动处理 AVI/MJPEG 解封装、JPEG 硬解、PPA 缩放
- 帧率有保证（硬件加速）
- 和 MP3 播放共用 esp_player 框架

**风险**：
- video_render 直接推 LCD，和 LVGL 抢屏幕
- 项目 BSP 不是 esp_board_manager，需要自己适配 LCD handle
- 固件体积可能再涨 200~300KB

### 方案 B：自己解析 AVI + JPEG 解码 + LVGL canvas 显示
自己写 AVI 解析器，逐帧 JPEG 解码到 LVGL canvas。

**优点**：和 LVGL 完全兼容
**缺点**：帧率低（软件解码 + LVGL 刷新慢），代码量大

## 推荐：方案 A

### 实施步骤

1. **PC 端准备视频**
   - 用 FFmpeg 把动画片转成 MJPEG AVI：
     ```
     ffmpeg -i input.mp4 -c:v mjpeg -q:v 5 -vf scale=800:480 -r 15 output.avi
     ```
   - 放到 SD 卡 `/sdcard/video/*.avi`

2. **设备端代码**
   - 复制 video_player_setup.c 的 video render 部分
   - 适配 LCD handle（从项目的 LCD 初始化代码拿 panel_handle）
   - 在 MUSIC tab 加视频文件列表
   - 点选视频 → 全屏播放 → 退出回 MUSIC tab

3. **sdkconfig 开启**
   - `CONFIG_AUDIO_DECODER_AAC_SUPPORT=y`（如果视频带音频）
   - `CONFIG_MJPEG_EXTRACTOR_SUPPORT=y`（已开）
   - `CONFIG_VIDEO_DECODER_JPEG_SUPPORT=y`（需确认）

4. **LCD 双缓冲**
   - video_player_reconfig_lcd 里设 num_fbs=2
   - 我们项目 LCD 已经初始化过，需要改成双 FB

### 工作量
- 代码：~300 行（video render 适配 + UI）
- 调试：1~2 轮（LCD handle 适配 + 帧率调优）
- 固件体积：+200~300KB（video decoder + PPA 元件）

### 验证项
1. 能列出 `/sdcard/video/*.avi`
2. 点选后全屏播放，帧率 ≥15fps
3. 有音频的话音画同步
4. 退出后 LVGL UI 正常
