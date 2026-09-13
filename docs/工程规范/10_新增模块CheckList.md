# 10 新增模块 CheckList

新增一个 middleware 模块（例如 `udp_voice`）时，按此清单逐项打勾。

## 代码

- [ ] 在 `middleware/` 下新建 `udp_voice.c` / `udp_voice.h`；
- [ ] 文件头按 02 规范加 SPDX + `@brief`；
- [ ] 命令枚举/结构体/事件枚举按 07 模板；
- [ ] `xxx_init()` 创建队列 + 互斥锁 + 常驻任务（栈 8192）；
- [ ] `xxx_post_cmd()` 非阻塞入队；
- [ ] `xxx_set_evt_cb()` 注册事件回调；
- [ ] 任务主循环 `xQueueReceive(portMAX_DELAY)`；
- [ ] 长循环里用带超时的 `xQueueReceive(pdMS_TO_TICKS(20))` 响应停止命令；
- [ ] 高频链路（每 20ms）禁止打 ESP_LOGI。

## 构建

- [ ] `main/CMakeLists.txt` SRCS 加 `"middleware/udp_voice.c"`；
- [ ] 新依赖加 `main/idf_component.yml`（如有）；
- [ ] `idf.py build` 0 error。

## 接线

- [ ] UI 层在需要的 tab 里 `#include "middleware/udp_voice.h"`；
- [ ] UI 事件回调只调 `xxx_post_cmd()`；
- [ ] 在 `ui_windows_register_audio_events()` 或新注册函数里 `xxx_set_evt_cb()`；
- [ ] 事件回调内操作 LVGL 前 `bsp_display_lock`。

## 验证

- [ ] `idf.py -p COM9 flash`；
- [ ] 跑 `docs/_serial_capture.py` 看启动日志；
- [ ] 业务命令触发后日志符合预期；
- [ ] 无 Guru Meditation、无栈溢出 `0xa5a5a5a5`；
- [ ] 改动追加到 `docs/项目规划/改动记录.md`。

## 文档

- [ ] 新模块在 `附录_代码文件索引.md` 登记；
- [ ] 接口签名同步到 `07_模块实现规范.md`（如有新模式）。
