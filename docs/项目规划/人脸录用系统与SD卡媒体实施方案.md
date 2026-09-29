# 人脸录用系统 + SD 卡媒体（拍照 / 录像 / 文件管理）实施方案

> **★★ 状态：S1~S4 + S8 全部实施完毕并上机验收通过（2026-09-29）**。
> S2（双 tab + 拍照/录像）、S3（人脸录用）、S4（文件管理页）、S8（人脸识别打卡，
> 细化见 `S8_人脸识别打卡系统方案.md` §14~§18）均已闭环；
> 验收期修复见 `改动记录.md` §73（预览 NO_MEM、PSRAM 即用即释放审计、letterbox、
> 记录删除/对账、HSV 阉割、VIDEO 圆钮改版等）。**本文正文保留为设计依据。**
>
> **历史修订**（v2~v4，2026-09-25）：已并入用户决策"**新建 VIDEO tab 专做 720p 拍照/录像，CAM tab 240×240 专做模型/算法**"
> ⇒ 新增 **§2.5 双 tab 架构与相机拥有权转移**，并据此重写 §4 / §5 / §9 / §12；
> 实施中发现的本分支特有地雷 **`CONFIG_BSP_SD_MOUNT_POINT="/littlefs"`**（§3.1 坑③）；
> v4 关键勘误：切格式走 esp_video 私有 ioctl **`VIDIOC_S_SENSOR_FMT`** 且必须自备
> **带 `regs` 寄存器表**的描述（以 `改动记录.md` §23.1 为准）。
>
> **实施进度（2026-09-29 定稿）**：
> - ✅ **S1**：`middleware/periph/sdcard_service.{c,h}`（SD 挂载）+ 分区表 + 烧录链路；
> - ✅ **S2-0/2a/2b**：`ov3660_fmt` + camera_service 接口升级 + `tab_video` 骨架 +
>   `capture_service`（拍照）+ `avi_mjpeg_muxer`（单测全过）+ `record_video_service`（录像）；
> - ✅ **S3**：face_db / face_quality / face_recog_service（MSR+MNP+MFN）/ ENROLL 录入流程；
> - ✅ **S4**：`file_mgr_service` + `tab_files`（FILES 页浏览回放）+ viewfinder 预览；
> - ✅ **S8**：punch_service 打卡事务 + page_face 记录页 + CAM 页 MODE/FACE/PUNCH；
>   最终验收结论见 `S8_人脸识别打卡系统方案.md` §18；
> - 📋 遗留候选（不阻塞）：file_mgr `s_pv[2]` 按需化、阶段 3.5 fs_http_service。
> 分支：`端侧人脸识别`｜ 工程根：`C:\Users\xiao1\Desktop\esp32\display_audio_photo`
>
> 本文所有"事实"都带**源码行号或实测值**；凡属推断的，一律标 **⚠ 未实测**。
> **注意**：§2.5.4 / §4.2 的流程图里"用 `VIDIOC_S_FMT` 切格式""capture_service 自己管 V4L2"
> 这两处已过期，实际实现见上文 v4 勘误与 `改动记录.md` §23/§24。

---

## 0. 需求拆解（用户原话 → 四个子系统）

> "推进一下人脸录用系统吧，后续我会引入 main 主线里的 SD 卡系统，你先帮我移过来，
> 然后视频界面可以实现拍照、录像，然后人脸录用识别的文件管理一并放到 SD 卡里面"

| # | 子系统 | 交付物 | 依赖 |
|---|---|---|---|
| **S1** | **SD 卡系统**（移植 main） | `sdcard_service` + 挂载流程 + 目录规范 | 无（可独立验收） |
| **S2** | **拍照 / 录像**（**新建 VIDEO tab**） | 照片存 JPEG；录像存 MJPEG AVI | S1 |
| **S3** | **人脸录用（enroll / recognize）** | 特征库 + 录入 UI + 识别显示 | S1 + 新组件 |
| **S4** | **文件管理**（人脸库 / 照片 / 录像统一在 SD） | 设备端浏览页 +（可选）PC HTTP | S1 |

**建议实施顺序 = S1 → S2 → S3 → S4**，理由：S1 是其余三项的地基，且**不需要新组件、风险最低**，
一次烧录就能独立验收。

### ★ 用户已决策（三个决策点全部落定，本文后续章节已按此调整）

**① 【20:27】双 tab 划分** —— 原话：
> "新建一个 tab 专门 1280×720@12fps 用来录制视频和拍照存入 SD 卡；
> 现在 240×240 的就用来跑模型跑算法人脸识别。"

**② 【20:37】录像一期不带音频** —— 原话："**先不录音**"
⇒ 与 §5.4 一致，AVI 只做视频轨（`strh` 单条 `vids/MJPG`），不做音轨与音视频同步。

**③ 【20:37】人脸库 < 10 人** —— 原话："**小于10人**"
⇒ 索引**全量常驻内部 RAM**：10 人 × (id 2 B + name 32 B + off 4 B + len 2 B + schema 等) ≈ **0.5 KB**
（即便按最宽松的 128 B/人算也仅 1.25 KB）⇒ **UI 不必分页、特征无需按需 mmap**，
§6.2 的索引结构可以取**最简单**的形态（见该节 ★ 标注）。

⇒ **用"两个 tab = 两种相机格式"把"格式互斥"这个硬约束架构化掉**（见 §2.5）。
这条决策顺带解决了原方案的三个难题：

1. **"录制时预览中断"不再刺眼** —— 录制发生在专用 tab，该 tab 本来就不需要实时预览（只显示 `● REC` 覆盖层）；
2. **CAM 页按钮不再挤** —— 拍照/录像按钮不塞进 CAM 页，CAM 页专注"检测 + 识别 + 录入"；
3. **职责边界清晰** —— "跑模型"与"存媒体"分属两页，符合本工程"一页一职责"的既有风格。

**代价（必须知道）**：sensor 同一时刻只能有一种格式 ⇒ **两个 tab 不能同时工作**，
切换 = 停流 + 改格式 + 重启流。§2.5 给出让这件事**不违反本工程 UI 约定**的做法。

---

## 1. 两分支现状盘点（逐项 `ls`/`grep`，非凭印象）

### 1.1 当前分支（`端侧人脸识别`）已有什么

| 模块 | 文件 | 关键事实 |
|---|---|---|
| 相机服务 | `middleware/camera/camera_service.c`（287 行） | V4L2 DVP，**RGB565 240×240**，4 buffer mmap，`cam_task` 6144 栈/Core1；**每帧回调** `camera_frame_cb_t(rgb565,w,h,user)`（`camera_service.h:11`） |
| 相机页 UI | `ui/tab_camera.c`（415 行） | canvas 240×240 + **START / HSV / FACE / MODEL** 四按钮 + 全屏 zoom；`refresh()` 50ms 定时器 |
| 人脸检测 | `middleware/camera/face_service.cpp`（426 行）+ `.h` | 3 档（224 / 416 / MSR+MNP，**p4 变体**）、模型**常驻**、异步加载（S1 方案已落地） |
| 文件系统 | `middleware/periph/fs_service.c/h` | LittleFS 挂 `/littlefs`；JPEG 解码帧缓冲 |
| 依赖 | `main/idf_component.yml` | `esp-dl ^3.3` + `human_face_detect ^0.4` + `esp_new_jpeg ^1` |

**★ 关键接口**：`camera_service_start(camera_frame_cb_t cb, void *user)` —— 已有"每帧回调"，
**拍照/录像只需在回调里多挂一个消费者**，不必改相机流水线。

**★ 一个必须注意的语义**：`tab_camera.c:40` 的 `on_frame()` **只记录指针**（`s_last_frame = rgb565`），
**不拷贝**。而 `cam_task` 回调返回后立刻 `VIDIOC_QBUF`（`camera_service.c:94`）把 buffer 还给驱动
⇒ **回调返回后指针即失效**。拍照若在 LVGL 线程里异步读 `s_last_frame`，读到的是**已被复用的缓冲**。
（这正是 `tab_camera.c:33` 用 `volatile` + `s_frame_new` 表达"新帧已到"的脆弱之处：
显示可以容忍撕裂，但**存盘不可以**。）⇒ §4 的拍照实现必须**在回调内同步完成拷贝**。

### 1.2 main 分支可借鉴的部分

| 资产 | 位置（main） | 价值 |
|---|---|---|
| **SD 卡挂载服务** | `main/middleware/system/sdcard_service.c`（3,573 B）+ `.h` | ★ 核心：SDMMC 4-bit → FATFS `/sdcard`，含 **32KB 常驻 DMA bounce buffer**（F23 的修复） |
| SD 卡方案文档 | `docs/项目规划/SD卡音频视频实施方案-main分支.md` | 目录规范 / 分区表 / 组件依赖 |
| 实施踩坑记录 | `docs/项目规划/改动记录.md` §18 + F23 | DMA bounce 分配失败的真凶与修法 |
| MJPEG 播放 | `middleware/network/...` + `esp_video_render` 组件 | 反向参考（播放），我方要的是**录制** |
| 目录结构 | `middleware/{audio,bluetooth,network,system}` | ⚠ **与本分支不同**（本分支是 `{audio,camera,net,periph}`） |

**⚠ main 的 SD 卡方案是"播放"方向**（`/sdcard/music/*.mp3`、`/sdcard/videos/*.avi`），
**没有拍照/录像**。所以：**SD 卡挂载可整段搬**，拍照/录像要**新设计**（§4/§5）。

---

## 2. 硬件可行性核实（★ 决定方案形态的一节）

### 2.1 引脚：**不冲突**（这是最大的一颗定心丸）

来自 `managed_components/espressif__esp32_s31_korvo_1/include/bsp/esp32_s31_korvo_1.h`：

| 功能 | GPIO | 行号 |
|---|---|---|
| **摄像头 DVP** | XCLK 55 / PCLK 54 / VSYNC 56 / HSYNC 57 / **D0–D7 = 46–53** | `:133-145` |
| **SD 卡 SDMMC 4-bit** | **D0=20 / D1=21 / D2=22 / D3=23 / CLK=24 / CMD=25**；电源使能 **EN=39** | `:165-176` |
| LCD RGB | 8–19 / 33–36 / 40 / 43–45 | `:95-118` |
| I2S | SCLK 3 / LCLK 4 / DOUT 5 / DSIN 6 | `:83-88` |
| USB | POS 20 / NEG 19 | `:125-126` |

⇒ **相机(46–57)、SD(20–25)、LCD(8–19,33–36,40,43–45) 三者完全不相交**。

> ⚠ **唯一重叠点**：`GPIO20` 同时是 `BSP_SD_D0` 与 `BSP_USB_POS`。**本工程不用 USB 主机**
> ⇒ 无实际冲突；但若将来启用 USB（如 UVC 摄像头），**必须**重新评估。
> ⚠ **未实测**：main 分支从未同时跑过 SD 卡 + 摄像头（其 `middleware/` 下**没有** `camera/`）。
> ⇒ "两者同时工作"属于**推断**，实机第一次上电必须重点看 SD 读写是否报 `sdmmc_read_blocks failed`。

### 2.2 ★★ 摄像头**自带 JPEG 编码器**（本方案的技术转折点）

`sdkconfig` 里现有一条**被关掉**的选项：

```
4095:# CONFIG_CAMERA_OV3660_DVP_JPEG_1280X720_12FPS is not set
4096:CONFIG_CAMERA_OV3660_DVP_RGB565_BE_240X240_24FPS=y          ← 当前生效（为 esp-dl 224×224 准备）
```

对应 sensor 侧格式表（`esp_cam_sensor/sensors/ov3660/ov3660.c:141-152`）：

```c
.format   = ESP_CAM_SENSOR_PIXFORMAT_JPEG,
.port     = ESP_CAM_SENSOR_DVP,
.xclk     = 20000000,
.width    = 1280, .height = 720,
.fps      = 12,
```

⇒ **OV3660 内置硬件 JPEG 编码**，1280×720 @ **12 fps**，另支持运行时调质量
（`ov3660.c:496-498`：`if (cur_format->format == JPEG) ov3660_set_jpeg_quality(dev, value)`）。

而 esp_video 的 DVP 设备**支持运行时切格式**：

```
esp_video_dvp_device.c:381  static esp_err_t dvp_video_set_format(video, format)
esp_video_dvp_device.c:457  esp_cam_sensor_set_format(dvp_video->cam.sensor, format)
esp_video_dvp_device.c:510  .set_format = dvp_video_set_format,     → 即 VIDIOC_S_FMT
```

**⇒ 拍照/录像可以做到"零软件编码"**：让 sensor 出 JPEG 流，直接落盘。
这比"RGB565 → `esp_jpeg_enc` 软件编码"干净得多（省 CPU、省一次 YUV 转换、画质更高）。

**备选**（`esp_new_jpeg` 也带软件编码器，`include/esp_jpeg_enc.h`）：
`jpeg_enc_open/process/close`，输入需 **16 字节对齐**且格式为 **YUV（`JPEG_PIXEL_FORMAT_YCbYCr`）**
—— 我们的帧是 **RGB565**，走这条路要先做 RGB565→YUV 转换（240×240 = 57,600 像素/帧）。
**仅在"必须边预览边录制"时才值得**（见 §4/§5 的路线对比）。

### 2.3 ⚠ 硬约束：JPEG 模式与 RGB565 模式**互斥**

- 人脸检测（`face_service_detect`）与 LVGL 预览**都依赖 RGB565**；
- 切到 JPEG 后，帧是压缩流 ⇒ 预览需**软件解码**（`esp_jpeg_dec`），检测需"解码 + RGB565"；
- 而 `VIDIOC_S_FMT` **只能在非 streaming 状态调用**（V4L2 常规约束）⇒ 切格式 = **停流 → 改格式 → 重启流**。

⇒ **结论**：**预览/检测（RGB565）与拍照/录像（JPEG）是两种互斥的工作模式**，切换有可见中断。
这直接决定了 §4/§5 的方案取舍。

### 2.4 带宽与容量（粗算，⚠ 需实测校准）

| 项 | 估算 | 说明 |
|---|---|---|
| JPEG 720p 单帧 | ≈ 60–120 KB（quality 40–80） | 取决于画面复杂度 |
| 录像码流 @12fps | **≈ 0.7–1.4 MB/s** | SDMMC 4-bit 高速（理论 ~20 MB/s）**余量充足** |
| 8 GB 卡可录 | ≈ **1.6–3 小时** | 按 1 MB/s 估 |
| 照片单张 | ≈ 100 KB（720p） | 8 GB 可存 ≈ 8 万张 |

---

## 2.5 ★ 双 tab 架构与"相机拥有权转移"（用户决策的落地设计）

### 2.5.1 职责划分

| Tab | 格式 | 职责 | 变化 |
|---|---|---|---|
| **CAM**（现有，`tab_camera.c`） | **RGB565 240×240** | 预览 · HSV · **FACE 检测** · **人脸识别 / 录入** | **不加拍照/录像按钮**（原方案想加，现取消） |
| **VIDEO**（新建，`tab_video.c`） | **JPEG 1280×720@12fps** | **拍照** · **录像** → SD 卡 | 全新 |

`page_main.c` 的 tab 从 4 个变 5 个：

```c
lv_obj_t *tab_call  = lv_tabview_add_tab(s_tabview, LV_SYMBOL_AUDIO"  CALL");
lv_obj_t *tab_rec   = lv_tabview_add_tab(s_tabview, LV_SYMBOL_SAVE"  REC");
lv_obj_t *tab_cam   = lv_tabview_add_tab(s_tabview, LV_SYMBOL_VIDEO"  CAM");
lv_obj_t *tab_video = lv_tabview_add_tab(s_tabview, LV_SYMBOL_IMAGE"  VIDEO");  /* ← 新增 */
lv_obj_t *tab_sys   = lv_tabview_add_tab(s_tabview, LV_SYMBOL_SETTINGS"  SYS");
```

⚠ **tab 数从 4 → 5，要确认 tab 栏标签不挤压**（`page_main.c:47` tab bar 高 48、
`BSP_LCD_H_RES = 800` ⇒ 每 tab 160 px，`LV_SYMBOL_IMAGE"  VIDEO"` 需实测宽度）。
若挤，退化为 `"  VID"`。（本工程有 F46 的 tabview 裁剪教训，**必须实测**。）

### 2.5.2 ★★ 硬约束：sensor 同一时刻只有一种格式

只有一个 `esp_video` DVP 设备、一个 sensor ⇒ **CAM 模式与 VIDEO 模式互斥**。
切换 = `STREAMOFF` → `VIDIOC_S_FMT` → `REQBUFS`/`mmap` → `STREAMON`。

### 2.5.3 ★ 为什么不能"切 tab 就切格式"（要守本工程的 UI 约定）

本工程有明确约定（`docs/工程规范/` 与本文件 §6.5 同源）：

> **模式/角色切换用"页内按钮"触发，不用 tab 切换触发；`tab_changed_event` 里不调底层切换。**

现状代码正是这么写的（`page_main.c:35-40`）：

```c
static void tab_changed_event(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        page_main_refresh_group();      /* 只重绑 encoder group，不碰任何硬件 */
    }
}
```

而"切格式"是**最重的一类底层切换**（含 `STREAMOFF` + sensor 重配置 + `mmap` 重建，
**⚠ 实测耗时未知，可能数百 ms**）⇒ **绝不能塞进 `tab_changed_event`**。

### 2.5.4 做法：**"接管 → 干活 → 归还"事务**（由页内按钮驱动）

```
VIDEO tab 内的按钮 = 事务入口（符合"页内按钮触发"约定）

  ┌─ 用户按【拍照】 ─────────────────────────────────────────────┐
  │ ① camera_service_stop()           停 RGB565 流               │
  │ ② 切 JPEG 1280×720 + 设 quality                              │
  │ ③ 丢掉前 3~5 帧（等 AE/AWB 稳定）                            │
  │ ④ 取一帧 → **回调内同步写 SD**（§4.3）                        │
  │ ⑤ 切回 RGB565 + camera_service_start(原回调)   ← 归还        │
  └──────────────────────────────────────────────────────────────┘

  ┌─ 用户按【开始录像】 ─────────────────────────────────────────┐
  │ ①②③ 同上                                                    │
  │ ④ 进入**录制循环**（持续时间长）                              │
  │ ⑤ 用户按【停止】→ 回填 AVI 头 → **归还**（切回 RGB565）       │
  └──────────────────────────────────────────────────────────────┘
```

- **拍照**：归还很快（用户感知 = 快门后黑一下）。
- **录像**：整个录制期间**持有独占权**（见 2.5.5）。

### 2.5.5 录像期间别的 tab 想用相机怎么办（必须定规则）

录像可能持续几分钟，用户完全可能切回 CAM tab。**必须显式规定**：

| 情况 | 行为 |
|---|---|
| 录像中切到 CAM tab | **检测功能不可用**，页面显示 **"相机被录像占用"** 并置灰 START/FACE 按钮（**不要静默失败**） |
| 录像中切到 VIDEO tab | 显示录制中状态（时长 / 已写 MB），可停止 |
| 录像中切到其他 tab（CALL/REC/SYS） | 录像**继续**（后台任务继续写盘） |

⇒ 需要一个**极简的"相机占用"查询**，供 CAM 页读取：

```c
/* camera_service.h 新增 */
typedef enum { CAM_OWNER_NONE, CAM_OWNER_PREVIEW, CAM_OWNER_MEDIA } cam_owner_t;
cam_owner_t camera_service_owner(void);
```

`CAM_OWNER_MEDIA` = 正在拍照/录像 ⇒ CAM 页据此置灰并提示。
**这是唯一能让两个 tab 和平共处的机制，不能省。**

### 2.5.6 必须配套的两处接口变更（否则 JPEG 模式跑不起来）

**变更 ①：帧回调要带"帧长度"**（JPEG 是**变长帧**，RGB565 是定长）

现有签名（`camera_service.h:11`）**没有 size**：

```c
typedef void (*camera_frame_cb_t)(const uint8_t *rgb565, uint32_t width, uint32_t height, void *user);
```

而 `cam_task` 回调处（`camera_service.c:91-92`）只传 `s_buf[buf.index]` + `CAM_W`/`CAM_H`
—— 对 RGB565 够用（定长），对 JPEG **完全不够**。

**依据（本轮已核实）**：`esp_video` 在 DQBUF 时把实际帧长填进 `bytesused`
（`esp_video_ioctl.c:198`：`vbuf->bytesused = element->valid_size;`，
且 `bytesused == 0` 时置 `V4L2_BUF_FLAG_ERROR` 而非 `DONE`）。

⇒ **改用**：

```c
typedef void (*camera_frame_cb_t)(const uint8_t *data, uint32_t size,
                                  uint32_t width, uint32_t height, void *user);
```

并在 `cam_task` 里传 `buf.bytesused`。

> ★ **好消息**：现有代码的 `if (s_cb && (buf.flags & V4L2_BUF_FLAG_DONE))`（`camera_service.c:91`）
> **天然正确** —— 因为只有 `bytesused != 0` 才会被置 `DONE`。
> 即"空帧/坏帧被自动跳过"这件事**现在就成立**，不用改。
>
> ⚠ **改签名会影响现有调用者**：`tab_camera.c:40` 的 `on_frame()` 需同步改（仅此一处 + 内部）。

**变更 ②：`camera_service_start()` 要能指定格式**

现在是硬编码（`camera_service.c:162-167`：`CAM_W`/`CAM_H`/`V4L2_PIX_FMT_RGB565`）。
建议**保留原函数**（CAM 页不动），**新增**一个显式版本：

```c
typedef enum { CAM_FMT_RGB565, CAM_FMT_JPEG } cam_fmt_t;

/* 原 start() 内部改为调用它（RGB565 240×240），语义不变 */
esp_err_t camera_service_start_fmt(camera_frame_cb_t cb, void *user,
                                   cam_fmt_t fmt, uint32_t w, uint32_t h);
```

新增格式时**不要忘记**：`CAM_W`/`CAM_H`（`camera_service.c:22-23`）是**编译期常量**，
被 `s_cb(..., CAM_W, CAM_H, ...)` 与 `tab_camera` 的 canvas 尺寸共用
⇒ 必须改成"当前流的实际 w/h"（由 `VIDIOC_G_FMT` 或参数带出），否则 JPEG 模式下会报错误的尺寸。

### 2.5.7 ⚠ 两个未实测风险（必须上机验证，方案不掩饰）

1. **JPEG 模式下 mmap buffer 是否够大**：`esp_video` 按"当前格式"算
   `CAPTURE_VIDEO_BUF_SIZE`（`esp_video_internal.h:84` → `STREAM_BUF_INFO(s)->size`）。
   1280×720 JPEG 最坏情况帧长可能接近上限 ⇒ 若 buffer 不足会表现为
   **帧被截断 / `bytesused` 异常 / DQBUF 拿不到帧**。
   **验证方法**：上机后打印前 20 帧的 `bytesused`，看是否稳定且 < buffer 长度（`s_buf_len[]`，`camera_service.c:222`）。
2. **切格式的可靠性与耗时**：`VIDIOC_S_FMT` 在 DVP 设备上会重配 sensor
   （`esp_video_dvp_device.c:457`），是否需要在**非 streaming** 状态（已按 V4L2 常规做法先 STOP）。
   **验证方法**：日志打"停流→切格式→重启流"三段耗时；反复切 30 次看是否逐渐退化（P4-3 的教训：**反复 START/STOP 会重复建任务**，见 `camera_service.c:130-136`）。
   ⚠ 注意 `camera_service.c:51-54` 已注明：本分支 BSP **无 `bsp_camera_stop()`**，
   所以 `s_bsp_started` 保证 `bsp_camera_start()` **只调一次** ⇒ 反复切格式**不会**重复初始化 sensor 硬件，这是好事。

---

## 3. S1：SD 卡系统移植（★ 含 main 代码的两个坑）

### 3.1 main 的 `sdcard_service.c` **不能照抄**——有三处必须改

**坑 ①（致命）**：main 的实现**没有使能 SD 卡电源**。

BSP 的挂载路径里有这一步（`src/bsp_storage.c:190`）：

```c
BSP_ERROR_CHECK_RETURN_ERR(bsp_feature_enable(BSP_FEATURE_SD, true));
```

它做的是（`src/bsp_feature_en.c:19-29`）：

```c
case BSP_FEATURE_SD: {
    ... gpio_config(BSP_SD_EN = GPIO39, OUTPUT) ...
    gpio_set_level(BSP_SD_EN, !enable);     /* enable=true → 输出低电平 */
}
```

而 main 的 `sdcard_service_mount()` 用的是 `SDMMC_HOST_DEFAULT()` + 手配 slot，
**通篇没有 `bsp_feature_enable`**（已全文核对）。
⇒ 意味着 main 上 SD 能挂成功**要么依赖 GPIO39 的上电默认电平恰好为低**，**要么是侥幸**。
**移植时绝不能沿用这个省略。**

**坑 ②**：main 手写 tick 了 **32KB DMA bounce buffer**（`sdcard_service.c` 的
`sdcard_setup_dma_buffer()`），解决的是 F23：

```
E sdmmc_cmd: allocate_dma_buf: not enough mem, err=0x101   ← 真凶
E diskio_sdmmc: sdmmc_read_blocks failed (0x101)
```
根因：`SDMMC_HOST_DEFAULT()` 的 `dma_aligned_buffer = NULL` ⇒ 驱动在"目标缓冲未 DMA 对齐"时
**每次现场 `heap_caps_malloc(MALLOC_CAP_DMA)`**；内部 DMA 堆被 BT/LCD/WiFi/音频切碎后申请失败。

**这块必须保留**（32KB，优先 `MALLOC_CAP_SPIRAM|MALLOC_CAP_DMA`，本板可 PSRAM 直访 DMA）。

**坑 ③（★★ 反向陷阱：本分支的 `CONFIG_BSP_SD_MOUNT_POINT="/littlefs"` 是**故意的**，千万别改）**

> ⚠ **本条是 v3 的自纠**：v2 曾断言"这是把挂载点改错了，应改回 `/sdcard`"。
> **那是错的** —— 实施时逐行读组件源码才看清真实机制。下面是修正后的结论。

```
managed_components/.../Kconfig:49-53    default "/sdcard"                      ← BSP 的原意
sdkconfig:4008 / sdkconfig.defaults     CONFIG_BSP_SD_MOUNT_POINT="/littlefs"  ← ★ 本分支**有意**复用
main 分支 sdkconfig:4803                CONFIG_BSP_SD_MOUNT_POINT="/sdcard"    ← main 没有复用
```

**它被复用成了"模型所在文件系统的根"**：

| 位置 | 表达式 | 求值结果 |
|---|---|---|
| 组件 `human_face_detect.cpp:23` | `path(CONFIG_BSP_SD_MOUNT_POINT) / CONFIG_..._SDCARD_DIR / model_name` | `/littlefs/models/<model>.espdl` |
| 本工程 `face_service.cpp:53` | `FACE_MODEL_DIR = CONFIG_BSP_SD_MOUNT_POINT "/" CONFIG_..._SDCARD_DIR` | `/littlefs/models` |

配套配置（`sdkconfig:4237/4240`）：
`HUMAN_FACE_DETECT_MODEL_IN_SDCARD=y` + `HUMAN_FACE_DETECT_MODEL_SDCARD_DIR="models"`。

而 `littlefs_content/models/` 正好有那 4 份 `.espdl`（**共 1,186,432 B，已 `ls` 核对**）
⇒ 经 `littlefs_create_partition_image(storage littlefs_content)` 编进 storage 分区、挂在 `/littlefs`
⇒ **路径与文件严丝合缝**（`MODEL_IN_SDCARD` 只是"从文件系统读"的开关，
真正决定"哪个文件系统"的是 `BSP_SD_MOUNT_POINT`）。

> ★★ **最容易看错的一点**：组件 `human_face_detect.cpp:10-13` 确实有一句兜底
> ```c
> #if !defined(CONFIG_BSP_SD_MOUNT_POINT)
> #define CONFIG_BSP_SD_MOUNT_POINT "/sdcard"
> #endif
> ```
> 但它是 **`!defined` 保护的**，而 sdkconfig **定义了**该宏 ⇒ **兜底根本不生效**，
> 组件实际用的就是 `/littlefs`。（只看这一句会得出相反结论。）

**⇒ 结论：`BSP_SD_MOUNT_POINT` 保持 `/littlefs` 不动。**
改成 `/sdcard` 的连带后果：模型路径变 `/sdcard/models/...`，而文件仍在 littlefs
⇒ **三档人脸模型全部加载失败、FACE 彻底废掉**。

**⇒ 连带的设计后果（必须知道）**：挂载点被"语义占用"了，所以 **SD 卡服务不能再用
BSP 的 `bsp_sdcard_sdmmc_mount()`**（它内部写死用 `BSP_SD_MOUNT_POINT`）⇒ 见 §3.2 的修正版做法。

### 3.2 ★ 正确做法：**自己 mount + 手动补 SD 电源使能**（BSP 的封装在本工程不可用）

> ⚠ **本条是 v3 的自纠**：v2 曾推荐"用 `bsp_sdcard_sdmmc_mount()` 复用 BSP 的 `feature_enable`"。
> 但 §3.1 坑③ 查明"挂载点被模型路径语义占用"之后，**那个函数在本工程用不了** ——
> 它内部写死 `esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, ...)`（`bsp_storage.c:141`）
> ⇒ 会把 FATFS 挂到 `/littlefs`，与 LittleFS **同点注册、必然失败**。
> 下面是**实施后的真实做法**（代码已在 `main/middleware/periph/sdcard_service.c`）。

BSP 仍有两个**可复用**的部件（只是不能复用它的 mount）：

```c
/* esp32_s31_korvo_1.h:425 / 441 —— 拿 host 与 slot（slot 已含 width=4 + 全部引脚） */
void bsp_sdcard_get_sdmmc_host(const int slot, sdmmc_host_t *config);
void bsp_sdcard_sdmmc_get_slot(const int slot, sdmmc_slot_config_t *config);
/* esp32_s31_korvo_1.h:707 —— ★ 这一步 BSP 的 mount 里有、而 main 版漏了 */
esp_err_t bsp_feature_enable(bsp_feature_t feature, bool enable);  /* BSP_FEATURE_SD → GPIO39 */
```

**正确写法 = BSP 的 host/slot ＋ 自己的 DMA buffer ＋ 手动 `feature_enable` ＋ 自己的挂载点**：

```c
esp_err_t sdcard_service_mount(void)
{
    if (s_mounted) return ESP_OK;

    bsp_sdcard_get_sdmmc_host(SDMMC_HOST_SLOT_0, &s_host);
    s_host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;   /* BSP 只给 DEFAULT(20M)；录像要吞吐 */
    sdcard_setup_dma_buffer(&s_host);             /* ← 保留 main 的 32KB 常驻 DMA 缓冲 */

    bsp_sdcard_sdmmc_get_slot(SDMMC_HOST_SLOT_0, &s_slot);   /* 已含 width=4 + 引脚 */

    /* ★ main 版漏掉的一步：SD 卡电源使能（GPIO39，低有效） */
    bsp_feature_enable(BSP_FEATURE_SD, true);

    esp_vfs_fat_sdmmc_mount_config_t mnt = {
        .format_if_mount_failed = false,          /* ★ 绝不要 true：SD 上是用户数据 */
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    /* ★ 挂载点用 "/sdcard"，**不是** BSP_SD_MOUNT_POINT（它已被模型路径占用，见 §3.1 坑③） */
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SDCARD_MNT_PATH, &s_host, &s_slot, &mnt, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s (no card?)", esp_err_to_name(ret));
        if (s_dma_bounce) { heap_caps_free(s_dma_bounce); s_dma_bounce = NULL; }  /* 归还 32KB */
        return ret;
    }
    sdmmc_card_print_info(stdout, s_card);
    s_mounted = true;
    return ESP_OK;
}
```

**与 BSP 全局句柄的关系**：这条路绕过了 BSP 的 `bsp_sdcard` 指针
⇒ 将来**不要**调 `bsp_sdcard_unmount()`（它会解引用一个从未被赋值的指针）。
一期不做热插拔卸载，无影响；若二期要做，就在 `sdcard_service` 里自己调
`esp_vfs_fat_sdcard_unmount(SDCARD_MNT_PATH, s_card)`。

**两个部件的真实行为**（读源码确认，非猜测）：
- `bsp_sdcard_get_sdmmc_host()` = `SDMMC_HOST_DEFAULT()`，**不含 max_freq**
  （`bsp_storage.c:58-64`）⇒ 必须自己覆盖（已做）。
- `bsp_sdcard_sdmmc_get_slot()` 已设 `width=4` + `cmd/clk/d0-d3`（`bsp_storage.c:75-90`）⇒ 不必自己填。

### 3.3 移植的**具体操作**（不切分支，保护工作区 36 处未提交改动）

```bash
cd "C:/Users/xiao1/Desktop/esp32/display_audio_photo"
# 从 main 取出两个文件作为"参考素材"（不放工程内，放 .workbuddy/ref/ 只读参考）
mkdir -p .workbuddy/ref
git show main:main/middleware/system/sdcard_service.h > .workbuddy/ref/sdcard_service.h.main
git show main:main/middleware/system/sdcard_service.c > .workbuddy/ref/sdcard_service.c.main
```

然后**新建**（不覆盖）本分支版本：

| 新文件 | 内容 |
|---|---|
| `main/middleware/periph/sdcard_service.h` | 照搬 main 的头（`SDCARD_MNT_PATH "/sdcard"` + 2 个 API），**放在 `periph/`**（对齐本分支目录约定） |
| `main/middleware/periph/sdcard_service.c` | 依 §3.2 重写 mount + 保留 32KB DMA buffer |

**接线 3 处**：

1. `main/app/main.c`：在 `fs_service_mount()` 之后、`bsp_display_start()` **之前**插入
   `sdcard_service_mount();` —— 与 main 的顺序一致（`main:main/app/main.c:138-141`），
   **理由**：此时内部 RAM 最空、PSRAM 未被切碎，DMA buffer 与 FATFS 分配最容易成功。
2. `main/CMakeLists.txt`：SRCS 加 `"middleware/periph/sdcard_service.c"`；
   PRIV_REQUIRES 加 **`fatfs` `sdmmc`**。
3. **`sdkconfig` / `sdkconfig.defaults` 双写**（本工程铁律，否则 reconfigure 掉回 n）：

```
# ★ 必须改回 /sdcard（本分支被错误地设成了 /littlefs，见 §3.1 坑③）
CONFIG_BSP_SD_MOUNT_POINT="/sdcard"
# ★ 必须保持 n —— 绝不允许"挂不上就格式化"（SD 卡上是用户数据！）
# CONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL is not set
CONFIG_FATFS_LONG_FILENAMES=y          # 否则 BSP 会警告（bsp_storage.c:184），且"IMG_20260925_201633.jpg" 这类名会受损
CONFIG_FATFS_MAX_LFN=255
CONFIG_FATFS_ALLOC_PREFER_EXTRAM=y     # FATFS 工作区放 PSRAM，省内部 RAM
```

> ★ 判据：改完 `grep -n "BSP_SD_MOUNT_POINT" sdkconfig sdkconfig.defaults` 应**两处都是 `/sdcard`**。

> ⚠ BSP 的 `bsp_storage.c:184-186` 会在**未开长文件名**时打 warning ⇒ 若日志里看到它，
> 说明上面没生效（Kconfig 改了要**同时**写 `sdkconfig` 与 `sdkconfig.defaults`，否则 reconfigure 掉回 n）。

### 3.4 挂载点分工（**两套文件系统并存**，边界要写死）

| 介质 | 挂载点 | 放什么 | 理由 |
|---|---|---|---|
| 内部 flash（LittleFS，6,160,384 B） | `/littlefs` | **系统资源**：图标、bin 字体、**人脸检测模型**（4 份 `.espdl` = 1,186,432 B） | 必须**无 SD 卡也能开机可用**（模型在 SD 上会让"没插卡 = 功能全废"） |
| SD 卡（FATFS） | `/sdcard` | **用户数据**：照片、录像、**人脸特征库**、可选音乐 | 容量大、可拔插、PC 直接读 |

⇒ **红线**：`fs_service`/`fs_service.h` 里的 `FS_MNT_PATH` **保持 `/littlefs` 不变**；
新增 `SDCARD_MNT_PATH`。**业务代码禁止跨挂载点硬编码路径**。

---

## 4. S2-a：拍照（**VIDEO tab** 的【拍照】按钮）

> 事务骨架见 §2.5.4（停流 → 切 JPEG → 抓帧 → 落盘 → 归还）；本节只展开**落盘细节**。

### 4.1 两条路线（必须二选一，本文推荐 A）

| | **路线 A：切 sensor 到 JPEG** | 路线 B：RGB565 + 软件编码 |
|---|---|---|
| 画质 | **1280×720**（0.92 MP） | 240×240（0.06 MP） |
| CPU | ≈ 0（硬件编码） | 每帧 RGB565→YUV + JPEG 编码 |
| 预览/检测 | **中断**（需停流切格式） | **不中断** |
| 依赖 | 打开 `CONFIG_CAMERA_OV3660_DVP_JPEG_1280X720_12FPS` | 现有 `esp_jpeg_enc` |
| 实现量 | 中（格式切换状态机） | 小 |
| 风险 | 切格式耗时 & 回切可靠性 **⚠ 未实测** | 编码缓冲占内存；耗时 **⚠ 未实测** |

**推荐 A**：拍照是**低频、一次性**动作，用户能接受"快门黑一下"；
换来 720p 画质 + 零 CPU，对"人脸录用"尤其重要（**录入用高分辨率底片，识别率更高**）。

### 4.2 路线 A 的流程（写进 `camera_service` 还是新模块？）

**新增中间层模块 `middleware/camera/capture_service.c/h`**（不塞进 `camera_service`，
避免把"取流"与"存盘"耦合成一团）。职责：单次"格式切换 → 抓帧 → 落盘 → 切回"事务。

```
capture_photo(path)
  ├─ ① camera_service_stop()                    ← 必须停流才能 S_FMT（V4L2 约束）
  ├─ ② 打开 fd，VIDIOC_S_FMT = V4L2_PIX_FMT_JPEG, 1280×720
  ├─ ③ VIDIOC_S_CTRL = ESP_CAM_SENSOR_JPEG_QUALITY（默认 80）
  ├─ ④ REQBUFS + QUERYBUF + mmap（沿用 camera_service 的 BUF_CNT 做法）
  ├─ ⑤ STREAMON → 循环 DQBUF **丢弃前 N 帧**（等 AE/AWB 稳定，建议 3–5 帧）
  ├─ ⑥ 取到第 N+1 帧：**当场写入 SD**（回调内同步，见 §1.1 的语义陷阱）
  ├─ ⑦ STREAMOFF → munmap → close
  ├─ ⑧ 恢复 RGB565 240×240 + camera_service_start(原回调)   ← 回到预览
  └─ ⑨ 返回耗时与文件大小（供 UI 显示 / 内存诊断）
```

**⚠ 步骤 ⑧ 是风险点**：`camera_service_start()` 里有 `s_bsp_started` 防重入
（`camera_service.c:139-148`），且 `bsp_camera_start()` 只能调一次 ⇒ 走"重新 start"路径
**不会再初始化 sensor**，这正是我们要的（避免重复建 pipeline）。
**但**：本分支 BSP **没有 `bsp_camera_stop()`**（`camera_service.c:51-54` 已注明）
⇒ 切格式期间 sensor 仍供电、XCLK 常开，**无法真正断电**。这不影响功能，只影响功耗。

### 4.3 文件规范

```
/sdcard/photos/IMG_20260925_201633.jpg        ← 时间戳命名（Y/M/D_HMS）
```
- 命名用 **RTC/SNTP 时间**（本工程已有 `net_time_service` 可取 epoch 秒，`:60 updated_s`）；
  未校时时退化为 `IMG_0001.jpg` 递增（**不要用 0 秒时间戳** —— 会互相覆盖）。
- 目录不存在则 `mkdir`（FATFS 需逐级建）。
- **写入必须查返回值**（`record_service.c` 的 `fwrite` 静默冻结就是前车之鉴，见 `内存管理规划.md` §2.4）。

---

## 5. S2-b：录像（MJPEG AVI）

### 5.1 方案：JPEG 模式下**每帧直存**，封装 AVI(MJPEG)

同样是"零软件编码"：sensor 出 JPEG 帧 → 按 AVI 容器写 `00dc` 块 → 收尾回填索引。

```
/sdcard/videos/VID_20260925_201633.avi
```

**AVI 结构（本方案只需最简合法子集）**：

```
RIFF('AVI ')
 └ LIST('hdrl')
    ├ 'avih'  主头（width=1280 height=720 fps=12 microSecPerFrame=83333）
    └ LIST('strl') └ 'strh'('vids','MJPG') + 'strf'(BITMAPINFOHEADER)
 └ LIST('movi')
    └ '00dc' <size> <JPEG frame>   × N
 └ 'idx1'   每帧偏移索引（收尾回填）
```

**关键实现点**：
1. `avih` 的 `dwTotalFrames` / `idx1` 需要**收尾回填** ⇒ 用 `fseek` 写回头部；
   **掉电**会产生"头部帧数为 0 但 movi 有数据"的文件 ⇒ **回退策略**：每写 1 帧顺手记一个
   `.idx` 边车文件，或接受"最后一段可丢"（**推荐后者，简单**，并在 UI 提示"拔卡前请先停止录像"）。
2. **写盘缓冲**：FATFS 写入是 `fwrite` → 底层 `diskio`。建议**单帧一次 fwrite**（60–120KB 已是好的块大小），
   `setvbuf` 不要设太小（默认 512B 会放大写调用次数）。
3. **帧率不稳定**：`fps=12` 是 sensor 上限，实际取决于 SD 写入是否跟得上。
   ⇒ `avih` 里写**实测平均 fps**，而不是硬写 12（否则播放器会音画不同步/变速）。
   **⚠ 未实测**：实际可达帧率。
4. **丢帧策略**：**不丢**。宁可降 fps 也不要丢帧（MJPEG 是帧独立，丢帧 = 时间戳不连续 = 变速）。
   若写入慢，就自然降低帧率，并把它写进 `avih`。

### 5.2 VIDEO tab 的界面：**不做实时预览**（这是设计，不是妥协）

用户决策已把"录制"独立成 tab（§2.5.1）⇒ **原方案里"预览中断"这个难题直接消失了**：
该 tab 从来就没有预览，所以录制期间"看不到画面"是**预期行为**，不是缺陷。

| 界面元素 | 说明 |
|---|---|
| 状态区 | `● REC 00:12` + 已写 MB + **实测 fps**（不硬写 12，见 §5.1-③） |
| 按钮 | 【拍照】/【开始录像】/【停止】—— 全部页内按钮（守 §2.5.3 约定） |
| 提示行 | "录制中相机被占用，CAM 页检测暂不可用"（对应 §2.5.5） |
| 空闲态 | 显示 SD 卡剩余空间 + 已存照片/录像数量 |

**可选增强（二期）**：拍照成功后，把刚拍的 JPEG 用 `esp_jpeg_dec` 解出缩略图显示 1~2 秒
（"看看拍成什么样"）。成本是**一次性**解码，不影响录制 ⇒ 可做可不做。

> 为什么不做"降分辨率预览"当折中：OV3660 的 JPEG 档**只有 1280×720 一种**
> （`sdkconfig:4095`，没有 640×480 JPEG 选项）⇒ 无法靠"sensor 出小图"来省解码。

### 5.3 停止与时长限制

- 支持**手动停止**（按钮）与**上限保护**（建议 5 分钟或 200 MB，可配）。
- 到达上限或 SD 写失败 ⇒ **立即停止并回填 AVI 头**（不能留一个坏文件）。
- **不允许**在录像中拔卡/断电（FATFS 会留脏簇）⇒ UI 提示。

### 5.4 音频：**一期不做** ✅ **已由用户确认**（2026-09-25 20:37 "先不录音"）

录像带声音要：mic 采集（`audio_service` 已有）+ 与视频**时间戳对齐** + AVI 里加 PCM 音轨
（多一条 `strl`）+ 收尾回填。复杂度远高于视频轨，且需要**音视频同步**调试。

⇒ **一期做纯视频 MJPEG AVI**（AVI 里只有一条 `vids/MJPG` 流）；
二期若要做，可复用 `record_service` 的 **LOCAL/STREAM 双模**经验，但**必须新开议题**。

---

## 6. S3：人脸录用（enroll / recognize）

### 6.1 依赖：**已实测可满足，不再阻塞**（2026-09-25 核实）

现状（已核对）：

- ✅ 本工程**已有** `espressif/human_face_detect`，**锁定版本 0.4.2**
  （`dependencies.lock:251-260`，含 `human_face_detect.hpp` + p4/s3 两套模型）；
- ✅ esp-dl **已内置识别基础设施**：`managed_components/espressif__esp-dl/vision/recognition/`
  （`dl_feat_base` / `dl_feat_image_preprocessor` / `dl_feat_postprocessor` / **`dl_recognition_database`**）；
- ❌ 工程内**没有** `espressif/human_face_recognition`（即 `HumanFaceRecognizer` 封装 + MFN 特征模型）。

**★ 本轮实测结论（原担心的"注册表 403"已不适用）**：

| 核查项 | 结果 |
|---|---|
| 组件注册表 API 可达性 | ✅ 可达（`GET components.espressif.com/api/components/espressif/human_face_recognition` 正常返回版本表） |
| 最新版本 | **0.3.2** |
| 它要求什么 | 依赖 `espressif/human_face_detect` 版本 **`~0.4.1`**（即 ≥0.4.1 且 <0.5.0） |
| 本工程现状 | `human_face_detect` = **0.4.2** ⇒ **满足 `~0.4.1`** ✅ |
| esp-dl 侧 | 本工程 `esp-dl` = 3.3.11，recognition 要求 `^3.0.0` ⇒ **满足** ✅ |

⇒ **只需在 `main/idf_component.yml` 加一行**，无需换版本、无需离线 clone：

```yaml
dependencies:
  espressif/human_face_recognition: '^0.3'    # ← 新增；会连带锁定 human_face_detect ~0.4.1（现有 0.4.2 兼容）
```

**⚠ 仍需实测的一点**：该组件在 **esp32s31** 上是否有可用模型变体。
`human_face_detect` 有明确的 `models/p4/` 与 `models/s3/` 两套（本工程已选 **p4**，见 S1 方案 §6），
而 recognition 的模型目录结构**尚未核实** ⇒ 拉下来后**第一件事**是确认
`managed_components/espressif__human_face_recognition/models/` 下的变体与 p4 的选择方式
（若只有 s3 变体，则参考 S1 的经验：**必须换成 p4**，否则 RISC-V 后端不匹配）。

**降级路（若上表某项在实机上不成立）**：
1. 用 esp-dl 的 `dl_recognition_database` **自己实现**特征提取 + 比对（基础设施已在，只缺模型）；
2. 同 `esp_littlefs` / `cjson` 的做法，**离线 clone 到 `components/`**。

### 6.2 人脸库设计（**放 SD 卡**，符合用户要求）

```
/sdcard/face_db/
├── persons.idx          ← 索引：N 条 {id:u16, name[32], feat_off:u32, feat_len:u16, thumb[?]}
├── feats.bin            ← 特征向量拼接（每条形如 512 × float32 = 2,048 B，或量化后 512 × int8）
└── thumbs/0001.jpg      ← 每人一张录入底片（供 UI 列表显示）
```

- ★ **索引全量常驻内部 RAM**（用户已确认 **< 10 人**）：
  10 人 × (id 2 B + name 32 B + off 4 B + len 2 B + thumb_off 4 B + 对齐) ≈ **0.5 KB**
  ⇒ **L1 内部 RAM 零压力**，**UI 不必分页**，**特征也无需按需 mmap**（10 × 2 KB = 20 KB 特征全读进来都行）。
- **特征按需从 SD 读**（一期简单实现即可；<10 人时也可一次性全读，但按需读更省 PSRAM）。
- **原子更新**：写 `persons.idx.tmp` → `rename()` 覆盖（LittleFS 的原子 rename 经验同样适用 FATFS）。
  - ⚠ FATFS 的 `rename` **不是原子**（无日志），掉电有风险 ⇒ 更稳的做法是
    **双份 + 世代号**（`persons.idx.0` / `.1` 交替写，头里带 seq，读时取 seq 大的）。
- 元数据里带 **`schema_version`** 与 **`feat_dim`**，防止将来换模型后读出"看起来正常但算错"的数据。

### 6.3 录入流程（enroll）

```
① 用户在人脸页选"录入" → 输入名字（LVGL 键盘，本工程 page_wifi 已有键盘可复用）
② 进入"对准镜头"提示界面（采集 3–5 张）
③ 每张：detect → 取置信度最高的框 → 按框 + 关键点对齐裁剪 → 提特征
④ 多张特征**平均/择优**（推荐：取与均值余弦相似度最高的那张作为入库特征）
⑤ 写 SD（feats.bin + persons.idx 双份）+ 存缩略图
⑥ 提示"录入成功：<名字>"
```

**质量门槛**（否则录进去的是垃圾）：
- 检测 `score < 阈值` ⇒ 拒绝该张（提示"请正对镜头"）；
- 人脸框占画面比例过小（如 `w < 40 px`，即 240 宽里的 1/6）⇒ 拒绝；
- 多张特征之间的**一致性**（两两相似度低于阈值）⇒ 提示"光线/角度差异太大"。

**★ 240×240 的底片够不够？（用户决策后的必答题）**

CAM tab 只有 RGB565 **240×240**，而人脸识别模型（MFN 系）输入是 **112×112** ⇒
**只要人脸占画面 1/2 以上（≥120 px 宽），240×240 底片缩到 112 几乎无损**。
喂 720p 进去最终也要缩到 112，**不会更准，只会更慢**。

⇒ **一期就用 240×240 录入**，与用户"240×240 跑模型跑算法"的决策完全一致。
**唯一例外**：远距离录入（人脸 <100 px）时信息量确实不足。
若将来需要"远距离录入"，再走 §4 的拍照通路抓 720p 底片 → 存 SD → 回 CAM 页从文件加载录入
（**二期增强，本文不做**）。

### 6.4 识别流程（recognize）

```
在 tab_camera 的 refresh() 里（**LVGL 线程**，与现有 face_service_detect 同路径）：
  detect → 对每个框：对齐裁剪 → 提特征 → 与库比对（余弦相似度）
         → 最高分 > 阈值 ⇒ 显示 "张三 (0.87)"；否则 "未知"
```

**性能预算**：录入/识别都要跑 2 个模型（detect + recognition）。
- 现有 detect 已是"隔帧推理"（`tab_camera.c` 的 `s_frame_skip`）；
- recognition 更轻（MFN 输入 112×112）但**每张脸一次**；
- ⚠ 必须实测：**detect + 多脸 recognition 是否超过一帧周期**，超了就要把 recognition 也挪到 `cam_task`
  （与 `face_service_poll()` 同路径），**绝不能在 LVGL 线程里堆到卡顿**。

### 6.5 UI（复用 F47 教训）

- **按钮压力已缓解**：拍照/录像移去 VIDEO tab 后，CAM 页只需容纳
  `START / HSV / FACE / MODEL` + 新增的 `录入 / 库`。
  ⇒ **仍建议**把 `HSV / FACE / MODEL` 收敛成一个"模式"循环按钮（页内按钮循环，
  与现有 MODEL 按钮同风格），腾出 2 个位给 `录入` 与 `库`。
- **要随 state 变色的图标，颜色设在按钮/容器上，子 label 一律不设 `text_color`**（F47）；
- 非 LVGL 线程**不得**碰 LVGL：识别结果只置 volatile 标志，由 `refresh()` 渲染；
- CAM 页必须处理 §2.5.5 的 `CAM_OWNER_MEDIA`：按钮置灰 + 明示"相机被录像占用"。

---

## 7. S4：文件管理（统一在 SD 卡）

### 7.1 设备端浏览页（推荐先做）

新增 `ui/page_sdcard.c/h`（走 `page_manager` 的页面栈，不新增 tab）：

| 功能 | 说明 |
|---|---|
| 目录树 | `/photos` `/videos` `/face_db` 三个入口 + 剩余空间（`esp_vfs_fat_info`） |
| 列表 | 名称/大小/时间；长列表用分页（每页 8 条），**不要一次性建全部 item**（PSRAM 教训） |
| 预览 | 照片 → 复用现有 `esp_jpeg_dec` + PPA 路径；录像 → 复用 `esp_video_render`（main 已在用） |
| 删除 | **二次确认**；删人脸库条目 = 同时改 `persons.idx`（走双份世代号） |
| 格式化 | **不提供**（危险且无必要） |

### 7.2 PC 端 HTTP（可选，二期）

阶段 3.5 的 `fs_http_service` 若将来要做，把 `/sdcard` 一并纳入即可（**当前分支尚未开工**，
见 `todolist.md` 的状态勘误）。**本文不展开。**

---

## 8. 内存 / 分区 / 预算

### 8.1 内存（口径见 `docs/开发文档/内存管理规划.md`）

| 项 | 增量 | 区域 | 说明 |
|---|---|---|---|
| SD DMA bounce buffer | 32 KB | **PSRAM** | 常驻，不占内部 RAM（`MALLOC_CAP_SPIRAM\|MALLOC_CAP_DMA`） |
| FATFS 工作区 | 数 KB | PSRAM | `CONFIG_FATFS_ALLOC_PREFER_EXTRAM=y` |
| 拍照/录像 JPEG 帧缓冲 | ≈ 128 KB（720p 单帧上限） | PSRAM | 直接指向 mmap 的 V4L2 buffer，**不必额外拷贝**（若走"回调内同步写盘"） |
| 人脸特征索引 | ≈ 0.4–20 KB | **内部 RAM** | 按人数线性增长 |
| 特征模型（recognition） | ≈ 0.5–1 MB | PSRAM | 与 detect 模型同理，**常驻**（S1 已证常驻只花 264 B 内部） |

**★ 判读口径**：只看内部 RAM 的 `largest_free_block`。
当前上电实测 `INT free≈127 KB / largest≈55 KB`（WiFi 重建一次要 51 KB 连续）
⇒ **内部 RAM 余量只剩 4 KB**，**本方案所有新增缓冲一律走 PSRAM**，绝不动内部堆。

### 8.2 分区表：**不需要改**

`storage`（LittleFS 6,160,384 B @0xA20000）继续只放系统资源；SD 卡不占 flash。

> 但要注意：**加入 recognition 模型后 littlefs 会变紧**。
> 现状：模型 4 份 = 1,186,432 B；若新增 recognition 模型（≈ 1 MB 级）
> ⇒ littlefs 只剩 ≈ 4 MB 给出厂素材，**可能需要在 SD 卡上也留一份模型作为可选来源**
> （但默认仍从 `/littlefs` 读，保证"没插卡也能用"）。

### 8.3 固件体积

新增代码（sdcard / capture / avi muxer / page_sdcard / 人脸库）预计 **+40–80 KB 代码段**，
而 app 分区 5 MB（`partitions.csv`）当前余量充足 ⇒ **无风险**。

---

## 9. 分阶段实施计划（每阶段可独立烧录验收）

### 阶段 S1：SD 卡挂载（**风险最低，先做**）

**文件**：
- 新增 `middleware/periph/sdcard_service.c/h`（依 §3.2）
- 改 `app/main.c`（插 `sdcard_service_mount()`）、`main/CMakeLists.txt`（SRCS + `fatfs sdmmc`）
- 改 `sdkconfig` + `sdkconfig.defaults`（§3.3 三项，**双写**）

**验收判据**：
- [ ] 启动日志出现 `SD card mounted OK` + `name: SD...`（`sdmmc_card_print_info`）
- [ ] **拔出 SD 卡**重启：只打 `SD mount failed ... (no card?)`，**其余功能全部照常**（不阻塞启动）
- [ ] 内部 RAM `largest` **不低于**改前（32KB 必须在 PSRAM —— 用 `[mem]` 埋点核对）
- [ ] 写一个测试文件再读回，内容一致（验 FATFS 通路）
- [ ] ⚠ **实测项**：日志无 `allocate_dma_buf: not enough mem`

### 阶段 S2-0：双 tab 骨架 + 相机接口升级（**先做，S2a/S2b 都依赖它**）

**文件**：
- `main/middleware/camera/camera_service.h/.c`：
  - 回调签名加 `size`（§2.5.6 变更①），`cam_task` 传 `buf.bytesused`；
  - 新增 `camera_service_start_fmt(cb, user, fmt, w, h)` + `camera_service_owner()`（§2.5.6 变更②、§2.5.5）；
  - 把 `CAM_W`/`CAM_H` 的**编译期硬编码**改为"当前流实际尺寸"（`camera_service.c:22-23, 92`）。
- `main/ui/tab_camera.c`：同步改 `on_frame()` 签名；`refresh()` 读 `camera_service_owner()` 置灰按钮。
- **新增 `main/ui/tab_video.c/h`**（先只放按钮 + 状态文字，拍照/录像留桩）。
- `main/ui/page_main.c`：加第 5 个 tab + `tab_video_build/destroy`；
  `main/CMakeLists.txt`：注册 `ui/tab_video.c`。

**验收判据**：
- [ ] 5 个 tab 不挤压、文字不被裁（F46 教训）；CAM 页一切功能**回归正常**（回调签名改了，必须回归）
- [ ] VIDEO tab 能进能出，不崩
- [ ] ⚠ 侧面验证：`camera_service_owner()` 在 CAM 页显示 `PREVIEW`

### 阶段 S2a：拍照

- 新增 `middleware/camera/capture_service.c/h`（§2.5.4 事务）；
- 改 `sdkconfig(.defaults)`：打开 `CONFIG_CAMERA_OV3660_DVP_JPEG_1280X720_12FPS=y`
  **（与 RGB565 档并存）**；
- 改 `tab_video.c`：接上【拍照】按钮 + 快门反馈 + 存盘耗时显示。

**验收判据**：
- [ ] 拍照后 SD 上出现 `IMG_*.jpg`，**PC 上能正常打开**（这是"真 JPEG"的硬证据）
- [ ] 拍完自动回预览，人脸检测可继续用（**回切后 detect 不失效**）
- [ ] 连拍 10 张无失败、无内存下降
- [ ] ⚠ **实测**：整个"切格式→抓帧→回切"耗时（预期数百 ms，若 >3 s 则需优化/改为预切）

### 阶段 S2b：录像

- 新增 `middleware/media/avi_mjpeg_muxer.c/h`（纯逻辑、无 LVGL、可单测）+ `record_video` 服务
- `tab_video.c` 接【开始录像】/【停止】+ 状态区（时长 / 已写 MB / 实测 fps）
- CAM 页接 `CAM_OWNER_MEDIA` 置灰与提示（§2.5.5）

**验收判据**：
- [ ] 生成 `.avi`，**VLC/电影播放器能播**，时长与录制时间一致
- [ ] 录 30 s 不丢帧（或丢帧数在 UI 上如实显示）
- [ ] **录到一半拔卡/断电** → 文件损坏但不影响系统（重启后仍能正常挂载）
- [ ] ⚠ **实测**：实际平均 fps（写进 `avih`）、SD 写入吞吐、峰值内存

### 阶段 S3：人脸录用

**前置**：解决 §6.1 的组件依赖（**必须先验证**）。

- 新增 `middleware/camera/face_recog_service.cpp/h`（C++，C ABI 对齐现有 `face_service.h` 风格）
- 新增 `middleware/face_db/face_db.c/h`（SD 卡上的库读写 + 双份世代号）
- 新增 `ui/page_face_enroll.c/h` + 改 `tab_camera.c`

**验收判据**：
- [ ] 录入 3 人，重启后仍能认出（**验证落盘**）
- [ ] 陌生人显示"未知"（**验证阈值不是摆设**）
- [ ] 拔掉 SD 卡：人脸**检测**仍可用（模型在 littlefs），仅"识别/录入"不可用并明确提示
- [ ] ⚠ **实测**：detect + recognize 的单帧总耗时（LVGL 线程不得超时）

### 阶段 S4：文件管理页

- 新增 `ui/page_sdcard.c/h` + `page_manager` 注册

**验收判据**：
- [ ] 列表/预览/删除均正确；删除人脸库条目后**重启不复活**
- [ ] 100 个文件的目录滚动不卡（分页生效）
- [ ] SD 卡不存在时页面优雅降级（提示而非崩溃）

---

## 10. 风险登记（★ 含明确的"未实测"）

| # | 风险 | 级别 | 对策 / 当前状态 |
|---|---|---|---|
| R1 | **SD 卡与摄像头同时工作无先例**（main 无 camera） | **高** | 阶段 S1 一次烧录即可验证；重点看 `sdmmc_read_blocks failed` |
| R2 | main 的 mount **漏了 GPIO39 电源使能** | **高** | §3.2 改用 `bsp_sdcard_sdmmc_mount()`（BSP 内部已含） |
| R3 | ~~组件可能拉不到（403）~~ → **已实测可满足**：注册表可达、`0.3.2` 依赖 `human_face_detect ~0.4.1` 与现有 **0.4.2** 兼容 | **低** | 仅剩"该组件在 esp32s31 上用哪个模型变体"待确认（§6.1 末） |
| R4 | 切 JPEG 格式的耗时与回切可靠性 **⚠ 未实测** | 中 | 阶段 S2a 实测；不可接受则退回"软件编码"路线 |
| R5 | 录像实际 fps / SD 写入吞吐 **⚠ 未实测** | 中 | 阶段 S2b 实测；写进 `avih` 而非硬编码 |
| R6 | 掉电/拔卡导致 AVI 或人脸库损坏 | 中 | AVI：接受末段丢失 + UI 提示；人脸库：**双份世代号** |
| R7 | `GPIO20` 与 USB 共用（`BSP_SD_D0` / `BSP_USB_POS`） | 低 | 当前不用 USB；**将来启用 USB 必须重评** |
| R8 | 内部 RAM 余量仅 ≈4 KB | 中 | 所有新增缓冲走 PSRAM；开工前先读 `内存管理规划.md` §3.2 |
| R9 | 本分支 BSP **无 `bsp_camera_stop()`** | 低 | 无法真正给 camera 断电；仅影响功耗，不影响功能 |
| R10 | 人脸录入质量差（光线/角度） | 中 | §6.3 三道质量门槛，拒绝比录错好 |

---

## 11. 明确不做（划界，避免范围膨胀）

| 不做 | 理由 |
|---|---|
| 录像带音频（一期） | 需 AVI 音轨 + 音视频同步，复杂度翻倍；二期再评估 |
| 录像与预览同时进行 | 720p JPEG 软解超帧周期，且抢 CPU |
| SD 卡上的 OTA / 固件存放 | 与本需求无关，且涉及安全边界 |
| 设备端"格式化 SD 卡" | 危险、无必要 |
| `fs_http_service`（PC 端 HTTP 管理） | 属阶段 3.5，当前分支未开工，另立议题 |
| 人脸库加密 | 一期不做（登记为将来项，若涉隐私需重新评估） |

---

## 12. 决策点状态（**三个已全部落定**，2026-09-25 20:37）

1. ~~拍照/录像的分辨率与"预览中断"取舍~~ → **已决（2026-09-25 20:27）**：
   **新建 VIDEO tab** 专做 1280×720@12fps 拍照/录像 → SD；
   **CAM tab** 240×240 专做模型/算法/人脸识别。
   ⇒ 方案已按此重写，**§2.5 就是这条决策的落地设计**（相机拥有权转移 + 接口升级）。
2. ~~录像是否必须带声音~~ → **已决（20:37）**：**先不录音** ⇒ 一期纯视频 MJPEG AVI（§5.4 已同步）。
3. ~~人脸库规模~~ → **已决（20:37）**：**< 10 人** ⇒ 索引全量常驻内部 RAM（≈0.5 KB）、
   UI 不分页、特征按需读 SD（§6.2 已按最简形态定稿）。

> ⇒ **三个决策点全部落定，方案冻结**。下一步就是按 §9 的阶段计划开工（**S1 已开工**）。

---

## 附：与既有文档的关系

- `docs/项目规划/todolist.md` —— 阶段 3.5（文件管理）与阶段三（端侧 AI）的状态以**顶部勘误**为准。
- `docs/开发文档/多模型加载策略_S1方案.md` —— 本文 S3 的 detect 部分沿用其"模型常驻 / 异步加载"结论。
- `docs/开发文档/内存管理规划.md` —— 本文 §8 的判读口径与铁律全部引自此文。
- `docs/工程规范/11_遇到的问题.md` F23（SDMMC DMA bounce）、F35（栈位置）、F47（UI 颜色继承）、F48（写 flash 与 cache）。
- `main` 分支：`main/middleware/system/sdcard_service.c`、`docs/项目规划/SD卡音频视频实施方案-main分支.md`、`改动记录.md` §18。
