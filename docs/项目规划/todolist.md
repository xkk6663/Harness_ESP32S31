# 传声机工程 TodoList

> 最后更新：2026-09-15（对齐需求文档 V3.3、ICD V1.1）
> 工程：ESP32-S31-Korvo-1 V1.1，ESP-IDF v6.1.0，IDF_TARGET=esp32s31，LCD 800×480，Flash 16MB / PSRAM 16MB。
> 闭环 SOP：每个子任务完成 → `idf.py build` → `idf.py -p COM9 flash`（板子自动复位）→ `docs/_serial_capture.py` 抓日志验证 → 打勾。
> 分层红线：APP(app/) / 中间层(middleware/) / UI(ui/)；中间层**禁止 include lvgl.h**；新源文件注册 `main/CMakeLists.txt`。
> 完成标准：不看代码也能按本条执行；每阶段末尾有"验收"清单，全绿才算过。

---

## 阶段 0：裁剪 + UI 骨架 ✅（已完成）
- [x] 删 `ui_file_browser.c/h` 及 TXT/JPEG 查看、亮度滑块；保留摄像头依赖（esp_video/esp_cam_sensor）；
- [x] SPIFFS 素材只留 xwkkk.jpg + test_16kHz.wav；AI 深蓝 UI；CALL/REC/SYS 三 tab；开屏壁纸 + 上拉手势；
- [x] sdkconfig 清理；docs 工程规范 11 篇 + `skills/esp-idf-closed-loop.md` 闭环 SOP。

## 阶段 1：Wi-Fi 基础（wifi_manager）✅（已完成）
- [x] `middleware/wifi_manager.c/h`：NVS/netif/event 初始化、STA 不自动连、scan/connect/get_ip/is_connected；
- [x] WIFI 子页扫描 + LVGL 键盘（SSID/密码明文）；实测 `got ip: 10.241.126.252`（AP：Power）；
- [x] 双回调槽 sys_cb→LED 状态灯（未连红/连接中蓝/已连绿，BSP_LED_BREATHE_SLOW）；
- [x] led_service 命令队列化修复并发 Cache error；RGB 子页手动常亮控制；改动记录 9.9。

---

## 阶段 1.5：基础设施改造（分区表 + LittleFS）——纯框架，零新增功能 ✅（已完成 2026-09-15）

> 目标：**不增加任何业务功能**，只把地基换成 OTA 布局 + LittleFS，并切到 ota_0 烧录。
> 完成后**现有全部功能照常运行**（WiFi/LED/UI/壁纸/按键/RGB/本地音频 demo），作为阶段二功能开发的前提。
> 权威依据：`docs/项目规划/SPIFFS迁移LittleFS指南.md`，一次性落到 OTA 最终表（不分两步过渡）。

### 1.5.1 分区表改造（partitions.csv）✅
- [x] 改为 OTA 最终布局：`nvs(24K) / phy_init(4K) / otadata(8K) / ota_0(3MB) / ota_1(3MB) / storage(littlefs, 10112KB≈9.87MB，全盘用尽)`；**无 factory、无独立 assets/rec_cache 分区**；
- [x] 构建日志确认分区表：`ota_0,app,ota_0,0x20000,3M`、`ota_1,...0x320000,3M`、`storage,data,littlefs,0x620000,10112K`；
- [x] bin=0x189f70（≈1.63MB），落在 3MB 槽内（51% 使用，49% 空闲）；
- [x] 烧录链路：bootloader@0x2000 + partition-table@0x8000 + ota_data_initial@0x11000 + app@0x20000(ota_0) + storage 镜像@0x620000，Hash verified，从 ota_0 启动。

### 1.5.2 文件系统迁 LittleFS（esp_littlefs）✅
- [x] esp_littlefs 由工程本地 `components/esp_littlefs`（joltwallet v1.22.1，littlefs 核心 submodule SSH 拉取到指定 commit 6cb4e86）提供；`main/idf_component.yml` 移除注册表依赖（registry 403）；
- [x] 单 `storage` 分区读写挂载（format_if_mount_failed=true），挂载点 `/littlefs`；出厂素材（xwkkk.jpg 等）与后续录音/壁纸同处一个 littlefs；
- [x] 根 CMakeLists `spiffs_create_partition_image` → `littlefs_create_partition_image(storage spiffs_content FLASH_IN_PROJECT)`；
- [x] 路径最小对齐：`fs_service.h FS_MNT_PATH="/littlefs"`、`audio_service.h REC_FILENAME` 用 FS_MNT_PATH、`main.c` 调 `fs_service_mount()`、`tab_record.c` 提示文字 `/littlefs/`；未动业务逻辑；
- [x] 未写任何录音/壁纸上传逻辑（属阶段 3 / 3.5）；未引入 ota_service（属阶段 6）。

### 1.5.3 阶段 1.5 验收（纯回归）✅
- [x] 启动日志：`FS_SVC: LittleFS mounted at /littlefs, partition 'storage', total=10112KB used=176KB`；
- [x] 无 panic/abort/Guru；`wallpaper 800x480 decoded` → `PAGE_MGR: load Splash` → `boot done`；
- [x] WiFi STA init done（首次切分区 nvs 已清，需重新连 Power）；LED/UI/按键/RGB/壁纸功能回归正常；
- [x] 烧录备注：S31 ROM 不支持整片 `erase-flash`，直接 `idf.py flash` 覆盖烧录即可；改动记录已追加、文件索引已更新。

---

## 阶段 2：UDP 低延迟通话 + PC 主机软件

> 端口：50000 设备→PC 音频；50001 PC→设备音频；50003 信令/心跳；50002/50004 阶段三/六用。
> 音频契约：16kHz/16bit/mono/小端，20ms=640B 净荷；12B 帧头（magic A5 5A|ver|type|seq|ts_ms|len）。
> 分层：网络任务只跟 audio_service ringbuf 打交道，不直接碰 codec、不 include lvgl。

### 2.0 audio_service 流式改造 + 三级缓冲（前置，先做）
- [ ] 新增 STREAM 模式：mic 采集常开循环写 `mic_tx_rb`，spk 播放常开循环从 `spk_rx_rb` 读；与文件播放/录音命令互斥；
- [ ] L1：I2S DMA（6 描述符×10ms），DMA 回调只搬运 + `xTaskNotify`，不跑协议；
- [ ] L2：独立流式 ringbuf（上行 8 帧≈5KB；下行目标水位 2–3 帧），与 play_rb/rec_rb 分开；用 3rd/ringbuf，≤16383B、4B 对齐、双核临界区；
- [ ] 下行水位：<2 帧补静音防爆音，>6 帧丢最旧防延迟累积；上行满丢最旧并计数；
- [ ] 对外接口：`audio_stream_start/stop`、`audio_stream_read_mic(buf,n)`、`audio_stream_write_spk(buf,n)`；
- [ ] mic/spk 双任务分离，ES8389 会话期同时常开、结束再 close；钉 Core1、prio 7–8、任务通知替代轮询；
- [ ] **本地回环验收**：mic→spk 自回环 ≤8ms、无爆音。

### 2.0.1 接口规范 + 传输抽象（先定后写）
> 分区表 / LittleFS / 烧录切换已在阶段 1.5 完成，此处只做软件抽象。
- [ ] 双端常量对齐 `通信接口规范_ICD.md`：采样契约、12B 帧头、端口表、ts 单端排序语义；
- [ ] 抽 `voice_transport` 统一接口 open/close/send_frame/on_recv_frame；先实现 `udp_transport`，`tcp_transport` 留桩；
- [ ] mic 流**多消费者分发器**：udp 通话与 record 录音可分别订阅同一份 mic；
- [ ] 预留 `session_manager` 状态机 IDLE/UDP_CALL/TCP_RECORD 互斥；信令枚举预留 REC_*；
- [ ] PC 端对称 NetLink(Udp/Tcp) 抽象，播放/ASR/落盘只订阅帧流。

### 2.1 link_manager 信令与连接状态（新增中间层）
- [ ] `link_manager_init()`：UDP 50003 socket + 常驻任务（栈 4096），按 ICD 解析 JSON；
- [ ] 收 PC HEARTBEAT：`recvfrom` 学习 PC IP、刷新心跳时间，回调 ONLINE，回 HEARTBEAT_ACK（fw/proto/sample_rate）；
- [ ] 500ms 巡检，2s 无心跳 → OFFLINE；预留 SESSION_START/STOP/PTT/MODE/REC_* 分支；
- [ ] `link_manager_is_online()` / `get_peer(addr*)` 供 udp_voice 取上行目标；
- [ ] 事件回调 tab_sys：连接点 ONLINE 绿/OFFLINE 红 + PC IP；断线自动通知 udp_voice STOP。

### 2.2 udp_voice 音频收发（新增中间层）
- [ ] `udp_voice_init()`：50000/50001 socket + tx/rx 常驻任务（栈 8192，Core0）；
- [ ] tx：从 mic ringbuf 取 640B → 打 12B 帧头 → 发 peer:50000；
- [ ] rx：收 50001 → 校验 magic/ver，16 帧环形重排窗口乱序重排；超时 40ms 填静音 → 写 spk ringbuf；
- [ ] 命令队列 VOICE_CMD_START/STOP/SET_PTT；START 前要求 link ONLINE；
- [ ] 丢包/乱序/延迟统计，每 2s 一条日志（高频收发路径不打日志）。

### 2.3 设备端 UI 接线
- [ ] `tab_call.c`：传音按钮 toggle → udp_voice START/STOP + 通话计时；peer 离线禁用并提示；
- [ ] `tab_sys_entry.c`：订阅 link_manager，在线绿/离线红 + PC IP；
- [ ] 物理按键映射（VOL-/VOL+ 已通；可选 BOOT 短按=通话开关）。

### 2.4 PC 主机软件（新建 pc_host/，Python，按需求 7.4 目录 + 7.5 解耦）
- [ ] 骨架 + PySide6 主窗口（Call/Record/OTA/Wallpaper/System 多 Tab）+ EventBus + 各业务独立 QThread；
- [ ] `net/link.py`：1s 心跳 + 收 ACK，维护在线状态；
- [ ] `net/voice_udp.py`：收 50000、去帧头、jitter 重排，同一帧 PCM 广播 play_q + asr_q；
- [ ] `audio/player.py`：sounddevice 播放 play_q（低延迟，满丢旧帧）；`audio/capture.py`：PTT 按住采集组帧发 50001；
- [ ] `asr/backend.py`：AsrBackend 抽象 + sherpa-onnx 离线流式，asr_q 喂 PCM 出文本经信号槽刷新；
- [ ] Call Tab：PTT 键、实时转写文本（清空/复制/导出 txt）、电平、丢包/延迟统计。

### 2.5 阶段二工程质量（源码走查硬约束，随做随验）
- [ ] sdkconfig：WIFI_PS_NONE、锁最高主频、禁 light-sleep、tick 1000Hz、LWIP UDP buffer 调大；
- [ ] 实时缓冲/队列/DMA 放内部 SRAM、静态分配、640B 帧池复用零 malloc；
- [ ] 任务 xTaskCreatePinnedToCore 绑核（音频 Core1 prio7-8，网络/UI Core0），测栈高水位留 30%；
- [ ] 跨任务回调只 post 命令（参照 led_service），不在回调直接操作 socket/codec/LVGL；
- [ ] udp rx 非阻塞 + select 超时，单循环统一收包/命令/心跳；
- [ ] 回声门控：spk 下行期间对 mic 衰减/静音防自激（AEC 留 M5）；
- [ ] 中间层事件改观察者/多订阅者，替单 set_evt_cb 覆盖槽；音量单一数据源；PA 开关 ramp 防爆音；
- [ ] 运行时统计（丢包率/jitter/水位/栈水位/CPU）输出 SYS 页或日志。

### 2.6 阶段 2 验收
- [ ] PC 启动后 SYS tab 红→绿，关软件 2s 回红；
- [ ] 按传音按钮，PC 实时听到声音且 ASR 逐句出字（≤1s）；PC 按住 PTT 设备实时播放，松开停，不中断设备上行；
- [ ] 端到端 ≤100ms，丢包补偿听感连续；本地回环 ≤8ms；连续通话 30min 无宕机/内存增长；
- [ ] 打勾 + `改动记录.md` 追加 + 代码文件索引补 link_manager/udp_voice/pc_host。

---

## 阶段 3：录音记录 + PC 同步（record_service / TCP 50002）

> 需求：REC 录音频，联网边录边传（无限时长），离线本地缓存 ≤5min PCM，PC 本地保存并可停止/播放/删除。
> 详见需求 V3.2 第 5 章、ICD 第 5/5.7 章。

### 3.0 存储与文件系统（前置，FS 阶段 1.5 已迁 LittleFS）
- [ ] 确认分区：单 `storage` littlefs（9.87MB），出厂素材与后续录音/壁纸同盘；离线 PCM 硬顶 300s（9.16MB），到点/空间不足自动停；
- [ ] storage 内分目录：`/recordings/`（录音）与 `/wallpapers/`（壁纸）隔离；清录音只删 `/recordings/`。

### 3.1 设备端 record_service（中间层，双模状态机）
- [ ] `record_service_init()`：常驻任务 + 命令队列（START/STOP/UPLOAD/RETRY/DELETE）；
- [ ] START 查 link：ONLINE→STREAM（mic 帧走 tcp_transport，仅 2–3s 抗抖缓冲，无限时长）；OFFLINE→LOCAL 写 `/recordings/` WAV（44B 占位头，≤300s）；
- [ ] STREAM 中断网→从断点转 LOCAL，恢复后 REC_BEGIN 带 `base_offset` 让 PC 拼接；
- [ ] STOP：STREAM 发 REC_END；LOCAL 回填 WAV 头、入待传队列，link 上线自动补传；
- [ ] UPLOAD：TCP 50002，REC_BEGIN→分块(len+offset+CRC16)→REC_END，错块重发/断点续传；
- [ ] 每 64KB flush 掉电保护；录音期 spk 静音防自录；mic 多消费者复用阶段二接口。

### 3.2 REC 子页 UI（改 tab_record.c）
- [ ] 替换 5s 本地测试：REC/STOP、已录时长、剩余可录时长、上传进度条/状态、当前模式（STREAM/LOCAL）；
- [ ] 本地待传列表（重传/删除）；录音中禁用通话（session 互斥）。

### 3.3 PC 端录音库（pc_host/record + ui/record_tab）
- [ ] TCP server 50002：REC_BEGIN 建文件、分块校验写、REC_END 回填 WAV 入库（sqlite：时间/时长/大小）；
- [ ] 录音库面板：列表；停止（REC_STOP）、本地播放（sounddevice）、删除（二次确认）、导出；
- [ ] STREAM/LOCAL 统一按 session_id+offset 落盘拼接；与通话经 EventBus 共存。

### 3.4 阶段 3 验收
- [ ] 联网录音停止后自动进 PC 库可播、时长一致；离线录满 ≤5min 自动停，联网后自动补传且 PC 端连续；
- [ ] PC 可停止/播放/删除，列表与文件一致；传输拔线重续不损坏，掉电已录部分保留；
- [ ] 录音与通话互斥切换正常；打勾 + 改动记录 + 文件索引。

---

## 阶段 3.5：文件系统管理（远程 HTTP + 设备端文件浏览器 + 壁纸热更新）

> 目标：PC 端可远程管理 /littlefs 里的图片/音频（上传/下载/删除/设为壁纸），
> 设备端 System 子页可浏览文件、点图片预览、点音频试听。
> 关键结论：LittleFS 本身只是 Flash 文件系统，"远程写入"由上层 HTTP 做；
> LittleFS 提供 wear-leveling + 掉电保护 + 原子 rename，适合远程写。

### 3.5.1 设备端 fs_http_service（新增中间层，禁止 include lvgl）
- [ ] `fs_http_service_init()`：基于 `esp_http_server`，wifi_manager 上线后起服务，端口 80；常驻在 HTTP 自带任务，不另建大任务；
- [ ] 接口：
  - `GET /fs/list` → JSON `[{name,size,type(image/audio)}]`（遍历 /littlefs，按扩展名分类）；
  - `PUT /fs/upload?name=xxx` → 流式分块写 `xxx.tmp`，写完 `rename()` 原子替换（掉电不损旧文件）；
  - `GET /fs/file?name=xxx` → 返回文件内容（PC 下载/预览）；
  - `POST /fs/delete?name=xxx` → unlink；
  - `POST /fs/wallpaper?name=xxx` → 设为开屏壁纸（发事件，不直接操作 LVGL）；
- [ ] 约束：上传前查剩余空间（`esp_littlefs_info`）；单文件上限（图片<512KB、音频<2MB，超出拒绝）；
  文件名仅允许 `[A-Za-z0-9._-]`、≤63 字符，拒绝 `../` 路径穿越；简单 token 头校验；
- [ ] 上传/删除完成经事件队列通知 UI 刷新文件列表（网络任务上下文禁止碰 LVGL）。

### 3.5.2 设备端壁纸热更新（wallpaper_service，3.5.1 的壁纸子能力）
- [ ] 中间层 wallpaper_service（命令队列）：设壁纸→校验 JPEG/大小→写 `/wallpapers/custom.jpg`→发事件；
- [ ] UI 订阅事件，LVGL 图片资源失效重载（PPA 硬解）；默认壁纸从 /littlefs 读；
- [ ] 校验失败/掉电回退旧壁纸；换图不卡 UI、不影响通话。

### 3.5.3 设备端文件浏览器 UI（System 子页新增入口）
- [ ] System 子页加"Files"入口 → 进入文件浏览页（list/grid）：实时列出 /littlefs 下图片(缩略)+音频(时长/大小)；
- [ ] 点图片 → 全屏预览（复用 JPEG/PPA 硬解，滑动/返回退出）；点音频 → 调 `audio_service` PLAY_FILE 播放，带播放/停止/返回；
- [ ] 文件列表随 fs_http 增删事件自动刷新（走队列/timer，不在网络回调刷 LVGL）；
- [ ] 长列表用虚拟滚动/分页，避免一次性加载全部缩略图占 PSRAM。

### 3.5.4 PC 端文件管理 Tab（pc_host，PySide6）
- [ ] 统一"文件管理"面板：列文件（图标/大小/类型），支持拖拽上传、下载到本地、删除、"设为壁纸"按钮；
- [ ] 图片上传前按 800×480 缩放 + 压 JPEG；上传进度/失败重试；EventBus 与通话/录音/OTA 互不阻塞。

### 3.5.5 阶段 3.5 验收
- [ ] PC 上传新壁纸即时生效、重启保留；PC 上传音频后设备端文件浏览器立即可见、可播放；
- [ ] 设备端点图片全屏预览清晰、点音频播放正常；删除后列表/PC 同步刷新；
- [ ] 上传中途掉电不损坏旧文件（临时文件+rename）；空间不足/非法文件名被拒；操作不卡 UI、不影响通话；打勾 + 改动记录 + 文件索引。

---

## 阶段 4：设备发现 + 完整信令 + 双模式切换（50003）

> 阶段二已做心跳/在线；本阶段补设备发现、完整信令、模式互斥切换。

### 4.1 设备发现
- [ ] 设备启动后周期性 UDP 广播自身（型号/版本/proto 版本）；PC `net/discovery.py` 扫描局域网列出设备；
- [ ] PC 主界面设备列表选择后作为心跳/通话/录音目标。

### 4.2 完整信令（ICD §4/§5.5）
- [ ] 50003 JSON 消息补齐：SESSION_START/STOP、PTT、MODE_SWITCH、REC_START/STOP、OTA_* 占位；
- [ ] 双向确认：每条控制消息回 ACK，超时重发。

### 4.3 模式切换状态机（session_manager）
- [ ] IDLE ↔ UDP_CALL ↔ TCP_RECORD 互斥：任一模式进行中禁止切入另一模式；切换走 STOP 当前→START 新，不重启；
- [ ] 设备 UI 与 PC 端经信令同步模式，状态 UI 一致。

### 4.4 多任务调度与优先级管理（FreeRTOS，跨全系统统一）
> 原则：硬实时音频 > 网络收包 > 业务/传输 > 后台管理/UI；后台写 Flash 必须分段让出 CPU。

- [ ] 统一任务优先级表（数值越大越高，FreeRTOS 0=IDLE）：

| 优先级 | 任务 | 核 | 说明 |
|---|---|---|---|
| 8 | mic 采集 / spk 播放（audio_stream） | Core1 | 硬实时，最高；I2S DMA 任务通知唤醒 |
| 7 | udp_voice tx/rx | Core0 | 次实时，紧跟音频，不可被后台阻塞 |
| 5 | tcp_record 收块 / ota_service 写块 | Core0 | 可靠传输，允许偶发抖动 |
| 4 | link_manager 心跳/信令 | Core0 | 500ms 节拍，非实时 |
| 3 | fs_http_service（esp_http_server 自带任务） | Core0 | 后台管理，最低业务级 |
| 3 | LVGL lvgl_port task | Core0 | 稳态 30FPS，不抢实时 |
| 2 | led/button/wifi 事件回调 | Core0 | 事件驱动，非实时 |
| 0 | IDLE | 任一 | idle hook 喂狗/统计 |

- [ ] **核绑定**：音频任务钉 Core1（xTaskCreatePinnedToCore），其余网络/HTTP/UI 在 Core0，
  避免 SMP 迁移抖动；音频 ringbuf/jitter/DMA 缓冲放内部 SRAM；
- [ ] **优先级反转防护**：共享 codec/FS/socket 的互斥锁一律用 `xSemaphoreCreateMutex()`（带优先级继承），
  禁止用 `spinlock`/关中断做长时间临界区；后台任务持锁时间 ≤1ms；
- [ ] **后台写 Flash 分段**：HTTP 上传/OTA 写块每写一块 `vTaskDelay(pdMS_TO_TICKS(1))` 让出 CPU，
  并在通话中按 5.6.1 避让；禁止后台任务连续占满 Core0 导致 udp_voice 调度延迟；
- [ ] **通知替代轮询**：DMA/网络/按键用任务通知或队列事件唤醒，不用 `vTaskDelay` 忙等；
- [ ] **栈水位巡检**：每个任务 `uxTaskGetStackHighWaterMark` ≥30% 余量，日志/System 页输出；
- [ ] **可观测**：CPU 占比（`esp_timer_get_time` 采样）、各任务栈水位、队列积压，输出 SYS 页。

### 4.5 验收
- [ ] PC 自动发现并选中设备；一键切通话/录音不重启；切换过程状态两端同步、音频链路干净切换；打勾 + 改动记录。

---

## 阶段 5：稳定与优化

- [ ] 回声消除 AEC（基于双 mic 参考，效果不好回退到 2.5 门控）；
- [ ] G.722 编码可选（64kbps，4× 省带宽；裸 PCM 为默认）；
- [ ] Wi-Fi 自适应：RSSI 低时降帧/提 jitter；
- [ ] 看门狗喂狗策略 + 72h 老化测试；内存碎片/堆水位/栈水位巡检；
- [ ] 性能验收：本地回环 ≤8ms、端到端 ≤100ms、UI 30FPS、72h 无宕机、稳定性 ≥99.9%；
- [ ] 全部指标写回需求文档第 9 章实测值。

---

## 阶段 6：OTA 远程升级（A/B 双区 + 回滚防砖，迁移 STM32 IAP 思想）

> 设计见需求第 11 章。不自写 bootloader，用 esp_ota/app_update + IDF rollback。

### 6.1 分区与配置（阶段二已占坑，此处开功能）
- [ ] 确认烧录走 bootloader+otadata+ota_0；menuconfig 开 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`；
- [ ] esp_app_desc 版本号纳入构建（git describe），心跳上报；可选尾部划 64K coredump。

### 6.2 设备端 ota_service（中间层状态机）
- [ ] IDLE/RECEIVING/VERIFY/PENDING/VALID/ROLLBACK，独立任务+命令队列；
- [ ] esp_ota_begin/write/end 写备用槽，4KB 块、队列解耦、进度事件给 UI；
- [ ] OTA_BEGIN/数据块(len+offset+可选crc32)/OTA_END/OTA_ABORT（TCP 50004）；整包 SHA256 校验、set_boot、延时重启；nvs 存 offset 续传（可选）；
- [ ] 升级期暂停通话/录音（Flash 写互斥），UI 升级页，规避写 Flash 关 cache 的并发坑。

### 6.3 回滚自证（防砖核心）
- [ ] 新固件 PENDING_VERIFY，自证条件满足（关键任务就绪 + 30s 无崩溃 + PC 心跳在线）后 mark_app_valid；
- [ ] 未自证 → 下次重启自动回滚旧槽；用 CRC 错误注入验证确实回退。

### 6.4 PC 端 OTA 面板（pc_host/ota + ui/ota_tab）
- [ ] 拖拽区收 bin，解析内嵌 esp_app_desc + 现算 SHA256，显示版本/时间/大小；
- [ ] 与设备版本 semver 比对：升级绿/重复灰/降级黄；TCP 分块推送 + 进度/速率/ETA + ACK；
- [ ] 固件仓库按版本归档 + sqlite 升级历史，可选旧版重推；构建后脚本导出 bin+manifest；
- [ ] 迁移 STM32 调试工具：CRC 错误注入、百分比限制器、收发包日志、中止/续传；
- [ ] 升级后心跳读版本确认，回滚红字提示。

### 6.5 阶段 6 验收
- [ ] 正常升级切槽上线、旧槽保留；坏固件/无法自证自动回滚；中途断电不变砖可续传；
- [ ] OTA 与录音/通话互斥正确，assets 不被擦，版本正确上报；打勾 + 改动记录 + 文件索引。

---

## 当前下一步

**先做阶段 2。** 阶段 1.5（分区表 + LittleFS）已于 2026-09-15 完成并回归通过。
开工前：
1. 重读 `skills/esp-idf-closed-loop.md` SOP；遵守 `docs/工程规范/`（中间层常驻任务+命令队列、不 include lvgl）；
2. 阶段 2：2.0 audio_service 流式改造（本地回环 ≤8ms）→ 2.0.1 软件抽象 → 2.1 link 心跳（SYS 绿/红）→ 2.2 udp_voice → 2.3 UI → 2.4 PC；
3. 新源文件注册 `main/CMakeLists.txt` SRCS/REQUIRES；每个单元 build→flash(COM9)→抓日志闭环；
4. PC 端 `pc_host/` 独立工程，按需求 7.4 目录、7.5 EventBus 解耦、7.3 sherpa-onnx；
5. 完成后打勾 + `改动记录.md` 追加 + 代码文件索引更新。

> 默认选型（需求已定，可改）：ASR=sherpa-onnx 离线流式；GUI=PySide6；设备传音=toggle；PC 回讲=按住 PTT；FS=LittleFS；OTA 双槽各 3MB、storage littlefs 9.87MB（阶段 1.5 已落地）。
