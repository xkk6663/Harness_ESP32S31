# SD 卡壁纸 + PC 壁纸管理 迁移实施方案

> 版本 **V2.1（决策已锁定，执行版）** · 2026-10-07 · 分支：**main**
> 需求（用户原话）：复用端侧支线 **SD 卡页面 → wallpaper 文件夹 → 打开图片 → 设为浮窗壁纸/开屏壁纸**
> 的逻辑功能；main 再加 **pc_host GUI 局域网上传/管理/设壁纸**。
>
> ⚠ **V1 作废说明**：V1 调研用了过时的分支 SHA（b0b17a8"性能优化"），漏掉了其后 6 个提交。
> 真实现位于**分支头 b590d31**（含 §75 壁纸服务全套），本文按正确源码重写。

---

## 0. 支线真实现源码地图（b590d31 = 端侧人脸识别 分支头）

分支头比 b0b17a8 新 6 个提交：`拍照录像存储sd卡 → SD卡文件管理 → 接入AI小智 → 手势识别模型 → 人脸识别管理系统 → 在线点歌 → wifi连接设备管理终端`。壁纸功能横跨其中三笔：

| 文件 | 规模 | 职责 | 依赖 |
|---|---|---|---|
| `middleware/media/wallpaper_service.{c,h}` | 628+103 行 | **核心服务**：SD 壁纸池 + 双缓冲解码 + 10min 随机轮换 + NVS 持久化 + 手动切换 + 文件校验 | sdcard_service / fs_service / nvs / esp_jpeg_dec |
| `middleware/media/file_mgr_service.{c,h}` | 432+117 行 | SD 文件管理服务：目录浏览/删除/照片预览/AVI 播放（服务任务 + 命令队列 + 事件回调） | sdcard / camera viewfinder |
| `ui/tab_files.{c,h}` | 610+31 行 | **FILES 页**：SD 浏览 UI；预览壁纸池内图片时出现 **"SET WALLPAPER"** 按钮 → `wallpaper_service_request_set()` | file_mgr / wallpaper_service / viewfinder |
| `ui/overlay_wallpaper.c` | 改 191 行 | 下拉浮层：不再自持底图，**借 service 双缓冲**，gen 轮询 + crossfade + commit 归还 | wallpaper_service |
| `ui/overlay_wp_particles.c` | 改 94 行 | 粒子浮层：同上（省掉自持 1.15MB 底图），canvas 固定 **RGB888** | wallpaper_service |
| `ui/page_splash.c` | 改 | 开屏直接 build 粒子浮层 ⇒ **开屏壁纸与浮窗壁纸天然同源** | overlay_wp_particles |
| `middleware/net/web_server.{c}` + `web/index.html` | 1031+577 行 | 设备端 Web 管理终端：浏览器访问，含壁纸**上传**（`wallpaper_service_validate_file` 作上传闸） | wifi_manager（改 104 行）/ page_wifi（改 74 行） |

### 双缓冲契约（迁移的"魂"，§75 设计）

```
service 任务：只往"空闲 buffer"解码 → 解完翻 s_cur、gen++ → 等 commit（UI 还旧图 / 5s 超时兜底）
LVGL 线程  ：只读当前 buffer，tick 里轮询 gen 发现新图（零跨线程 LVGL 调用，F44 同源）
buffer[0]  = 借 fs_service 的 file_buffer 前 768KB（main 该 buffer 为 1.15MB，够放）
buffer[1]  = service 自分配 work buffer（全屏 RGB565 = 800×480×2 = 768KB PSRAM）

恢复链：NVS wp_last（且文件在池中）→ SD 池随机 → littlefs xwkkk.jpg → 深蓝纯色（绝不黑屏）
换图入口：10min 随机轮换 ＋ FILES 页手动 request_set（名字必须在池内，防路径注入）
解码约束：池内一律 800×480（非整屏 JPEG 拒收）
         【V2.1 定稿】输出改 **RGB565**（用户拍板：硬件不支持全屏 RGB888 落屏），
         解码配置借 main 现有 overlay_wp_particles 的 esp_jpeg_dec RGB565 cfg（**无 R/B swap**）；
         两消费端 canvas 统一 LV_COLOR_FORMAT_RGB565
```

---

## 1. main 迁移适配点（逐条比对两分支差异得出）

| # | 差异 | 影响 | 迁移动作 |
|---|---|---|---|
| 1 | **像素格式定稿（V2.1）：全屏 RGB565**（用户拍板：硬件不支持全屏 RGB888 落屏；分支 16 位 / main LVGL depth 24 仅是内部处理精度，落屏一律 565） | 双缓冲从支线 1.15MB×2 RGB888 缩为 **768KB×2 RGB565**；buf0 仍借 `fs_service.file_buffer` 前 768KB（main 该 buffer 1.15MB ≥ 768KB） | service 解码输出改 RGB565，配置照抄 main `overlay_wp_particles` 现有 esp_jpeg_dec cfg（无 swap）；两消费端 canvas 统一 `LV_COLOR_FORMAT_RGB565` |
| 2 | 净内存账（V2.1 重算） | buf1 自分配 768KB；main `overlay_wp_particles` 自持的 768KB RGB565 底图**释放**；wp_particles 原本还有约 0.77KB 的自解码开销删除 | **净增 ≈ +0（768KB − 768KB，基本持平）**，比 V2 的 +0.38MB 更省 |
| 3 | main `overlay_wallpaper` 本来就是 RGB888 + R/B swap | 与新输出格式（RGB565 无 swap）不一致 | 用分支版改造（gen 轮询 + crossfade + commit），**去掉 swap**，canvas 改 RGB565 |
| 4 | main `sdcard_service.h` **已有 `SDCARD_MNT_PATH "/sdcard"`** 宏 | 无 | 直接 include |
| 5 | main `page_splash` 有自己的开屏壁纸逻辑（自解码） | 双份内存 | 改为 build 粒子浮层（同分支），开屏吃 service buffer，删自解码路径 |
| 6 | main 无 `file_mgr_service` / `tab_files` | 见 §2 决策 ① | **已拍板 ①A：整页搬** |
| 7 | main 分区 littlefs 3MB | **无影响**：壁纸池在 SD 卡，littlefs 只留 xwkkk 兜底图 | 无 |
| 8 | main 页序已定 7 tab（CALL/REC/AUDIO/SINK/VIDEO/PC/SYS） | FILES 页挂载方式 | **已拍板：第 8 tab "FILES"（SYS 前插入）** |
| 9 | 分支 `web_server` 含浏览器上传 | 与 pc_host 功能重叠 | **已拍板 ②路线 1：不搬 web_server** |
| 10 | 跨分支搬运 | build 串味 | **切完源码 `idf.py fullclean`**（老规矩） |

---

## 2. 三个决策（V2.1 已全部拍板 ✅）

### ① SD 浏览入口 → **拍板 A：整页搬 `tab_files` + `file_mgr_service`（约 +1,190 行）**

- **A. 整页搬 `tab_files` + `file_mgr_service`（约 +1,190 行，推荐）**：
  原样复用支线 FILES 页——浏览/预览/SET WALLPAPER/删除/AVI 播放入口一次到位，
  且 main 的 todolist 本来就规划了文件管理；代码在支线已实机跑通，搬运风险低。
  挂载为**第 8 个 tab "FILES"**（SYS 前插入）。
- **B. 轻量入口（约 +150 行）**：SYS 页加"壁纸"卡片，只列 `/sdcard/wallpaper/*.jpg`
  （内置简版扫描，不搬 file_mgr），点击 → 全屏预览 → SET WALLPAPER。
  快，但没有文件管理能力，后续做文件管理还得再搬一次 file_mgr。

### ② PC 上传路线 → **拍板 路线 1：pc_host「设备拉」**（与 pc_host v0.3 的 OTA 完全同构）

- **路线 1（推荐）：pc_host「设备拉」**——与 pc_host v0.3 的 OTA 完全同构：
  pc_host httpd 挂 `/wallpaper/<name>.jpg` 路由（就绪才宣告）→ UDP 50003 发
  `WP_OFFER{name,size,sha256,url}` → 设备 `wp_fetch` 任务（**内部 RAM 栈**，F35）GET 下载
  → `validate_file` 校验 → 落池 → `request_set()` → `WP_RESULT` 回报。
  工程量小（ICD §10 + 设备 fetch ~200 行 + pc_host 卡片 ~150 行），
  与"一个应用管全部"一致，防火墙零配置。
- **路线 2：连 `web_server`（1031+577 行）一起搬**：白得一个浏览器设备管理终端
  （上传壁纸 + 更多管理功能），但要动 wifi_manager/page_wifi、打静态资源包，
  工程量大且与 pc_host 重叠。可作为后续独立立项。

### ③ 轮换策略 → **拍板：保留 10 分钟随机轮换 + NVS 记忆**（现成代码不改）

---

## 3. 实施计划（拍板后执行）

### M1：设备端壁纸复用（对应决策 ① 的选择）

| 步骤 | 内容 | 量 |
|---|---|---|
| 1 | `wallpaper_service.{c,h}` 从 b590d31 提取 → `main/middleware/media/`；include 路径适配（periph→system）、`WALLPAPER_POOL_DIR` 用 main 的 `SDCARD_MNT_PATH`；**解码输出改 RGB565**（照抄 main `overlay_wp_particles` 的 esp_jpeg_dec cfg，无 swap）；MEM_STAGE 等分支私有宏替换；CMakeLists SRCS 登记 | ~80 行适配 |
| 2 | 消费端改造：`overlay_wallpaper` / `overlay_wp_particles` / `page_splash` 换成分支版逻辑（gen 轮询 + crossfade + commit）；两 overlay canvas 统一 RGB565、**去 swap**；删各处自解码与自持底图（wp_particles 释放 768KB） | ~250 行 diff |
| 3 | 入口：**拍板 ①A** = 搬 `tab_files`+`file_mgr_service`（file_mgr 去 camera viewfinder 依赖、tab_files 剥离 avi 播放）挂第 8 tab | ~1,190 行原样 + ~100 行适配 |
| 4 | `idf.py fullclean` 重编 + 静态探针（`.workbuddy/probe_idf_syntax.py`） | — |

### M2：pc_host 壁纸管理（对应决策 ② 路线 1）

| 步骤 | 内容 | 量 |
|---|---|---|
| 1 | ICD §10：`WP_OFFER / WP_ACK / WP_RESULT` 三条信令（字段与 OTA_OFFER 同构） | 文档 |
| 2 | 设备：`wallpaper_service_fetch()`（GET → 边收边写池 → validate → request_set；300KB 上限硬校验）；`main.py` 分发器把 `WP_*` 喂给处理（OTA 同款模式） | ~200 行 |
| 3 | pc_host：系统页"壁纸管理"卡片（选图 → 本地预校验 → 上传 → 结果；F44 队列排空） | ~150 行 |

### 红线与纪律（迁移中必须保持）

- `wallpaper_service` 属 middleware：**不 include lvgl.h**；
- UI 侧只读 buffer + gen 轮询 + commit 归还，绝不跨线程调 LVGL（F44）；
- fetch/导入任务栈**内部 RAM**（F35：碰 flash/SD IO）；解码缓冲全 PSRAM；
- **RGB565 定稿（V2.1）**：解码输出与两 canvas 全链 RGB565、**无 R/B swap**——
  合入前逐字节核对 main `overlay_wp_particles` 现行 cfg 与支线 `decode_file_into`，
  警惕"分支换过一次 + main 再换一次"的双重交换（V2 原红线保留，语义改为"确认 swap 为 0 层"）。

---

## 4. 验收步骤（预览）

| # | 组 | 步骤 | 判据（可 grep） |
|---|---|---|---|
| A | 池扫描 | SD 卡建 `wallpaper/` 放 2 张 800×480 jpg → FILES 页浏览 | 列表出现 2 项 |
| B | 手动切换 | 预览池内图 → SET WALLPAPER | crossfade 到新图；NVS `wp_last` 更新 |
| C | 开屏同源 | 重启 | splash 显示当前壁纸（非 xwkkk） |
| D | 下拉浮层 | 下拉手势 | 浮窗壁纸 = 同一张（gen 轮询生效） |
| E | 轮换 | 等 10min（或临时改短） | 自动随机换图 + crossfade，无撕裂 |
| F | 回退链 | 拔 SD 卡重启 | 依次降级：NVS 失效→池随机→xwkkk→深蓝，绝不黑屏（`fallback wallpaper (littlefs)`） |
| G | 非法图 | 池内放 1600×960 / 500KB | `validate_file` 拒收，当前图不动 |
| H | PC 上传 | pc_host 选图上传 | 30s 内设备换图，`WP_RESULT ok:1`；中途杀 pc_host → 失败且旧图不动 |
| I | 内存 | 开机 BOOT_DIAG + 换图前后三元组 | 净增 ≈ **+0**（buf1 新增 768KB ≈ wp_particles 释放 768KB），无常驻泄漏 |

---

## 5. 工作量汇总（已拍板 ①A + 路线 1 + 保留轮换）

| 组合 | 量 |
|---|---|
| M1（service + 三消费端 + ①A 整页搬） + M2（路线 1） | 1.5~2 天（含真机验收） |
| 路线 2（再搬 web_server） | 另加 1~2 天，建议独立立项（本次不做） |
