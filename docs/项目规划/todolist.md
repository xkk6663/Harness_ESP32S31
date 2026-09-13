# 传声机工程 TodoList

> 最后更新：2026-09-13
> 推进顺序：阶段 1 → 阶段 5，每阶段完成后在本文件打勾并追加到 `改动记录.md`。

---

## 阶段 0：裁剪 + UI 骨架 ✅（已完成）

- [x] 删 `ui_file_browser.c/h`（文件浏览 tab）；
- [x] 删 TXT/JPEG 查看、亮度滑块；
- [x] SPIFFS 素材只留 xwkkk.jpg + test_16kHz.wav；
- [x] 保留 esp_video/esp_cam_sensor 摄像头依赖；
- [x] AI 风深色 UI（#0B1120 / #22D3EE）；
- [x] CALL / REC / SYS 三 tab；
- [x] 开屏壁纸 800×480 + 上拉手势；
- [x] sdkconfig 清理（保留 3 个）；
- [x] docs 工程规范 11 篇 + skill 闭环 SOP。

---

## 阶段 1：Wi-Fi 基础（wifi_manager）

> 新建 `middleware/wifi_manager.c/h`，按 07 章"常驻任务+命令队列"模板。

- [ ] `wifi_manager_init()`：NVS 初始化、创建事件组；
- [ ] STA 模式，连接 SSID/password（先写死，后续从 SYS tab 输入）；
- [ ] 事件：WIFI_EVT_CONNECTED / WIFI_EVT_DISCONNECTED / WIFI_EVT_GOT_IP；
- [ ] 重连策略：失败等 3s 重试，最多 5 次后上报 UI；
- [ ] UI 接线：SYS tab 显示 IP/连接状态；
- [ ] 验证：`idf.py -p COM9 flash`，日志看到 `got ip: 192.168.x.x`；
- [ ] 改动追加到 `改动记录.md`。

---

## 阶段 2：UDP 低延迟通话（udp_voice）

> 端口 50000（设备→PC）/ 50001（PC→设备）。

- [ ] `udp_voice_init()`：建 socket、绑定端口、起常驻任务（栈 8192）；
- [ ] 音频链路：audio_task 每 20ms 产出 640B PCM → ringbuf → udp_task 打包 UDP 发；
- [ ] 帧头：`magic(0xA5) + seq(2B) + timestamp(4B) + length(2B) + flags(1B)`；
- [ ] 接收侧：16 帧环形重排窗口，乱序重排；超时 40ms 填静音；
- [ ] 命令：UDP_CMD_START / UDP_CMD_STOP / UDP_CMD_SET_PTT；
- [ ] CALL tab：连接状态、通话计时、PTT 按钮、音量；
- [ ] PC 端：Python `udp_voice_client.py`（socket + pyaudio + jitter buffer）；
- [ ] 验收：端到端 ≤100ms，丢包补偿听感连续；
- [ ] 验收：本地回环 ≤8ms（用 loopback 测试）。

---

## 阶段 3：TCP 整段录音（tcp_record）

> 端口 50002，TCP Client → PC Server。

- [ ] `tcp_record_init()`：建 socket、起常驻任务；
- [ ] 流格式：先 44B WAV 头（占位），随后原始 PCM；
- [ ] 每 2s flush 一次到 PC；
- [ ] 每 30 分钟切片，文件名 `rec_YYYYMMDD_HHMMSS.wav`；
- [ ] 断线重连：包头附 offset，PC 从偏移处续写；
- [ ] 录音期间设备本地喇叭静音；
- [ ] REC tab：录音计时、文件大小、停止按钮；
- [ ] PC 端：Python `tcp_record_server.py`（socket + wave，落盘 + 索引 index.csv）；
- [ ] 验收：录音文件完整可播，拔线不损坏。

---

## 阶段 4：双模式切换 + 信令（50003）

- [ ] 信令通道 UDP 50003：模式切换、开始/停止录音、音量；
- [ ] 设备发现：UDP 广播，PC 扫描局域网内设备；
- [ ] UI：SYS tab 选 UDP/TCP 模式，一键切换不重启；
- [ ] PC 端：`device_discovery.py` + CLI `--mode udp|tcp`；
- [ ] 验收：切换后状态 UI 同步，PC 端收到模式通知。

---

## 阶段 5：稳定与优化

- [ ] AEC 回声消除（如需要）；
- [ ] G.722 编码可选（64kbps，比裸 PCM 省 4 倍带宽）；
- [ ] Wi-Fi 自适应（信号弱时自动降帧）；
- [ ] 看门狗 + 72h 老化测试；
- [ ] 内存碎片检查；
- [ ] 性能指标全达标：≤8ms 本地 / ≤100ms 端到端 / 30FPS / 72h 无宕机。

---

## 当前下一步

**阶段 1：wifi_manager.c/h**。开始前：
1. 读 `docs/工程规范/01~10`；
2. 按 07 章模板写 `middleware/wifi_manager.c/h`；
3. 注册 `main/CMakeLists.txt` SRCS；
4. `idf.py build` → `flash` → `_serial_capture.py` 抓日志；
5. 完成后更新本文件打勾 + `改动记录.md` 追加。
