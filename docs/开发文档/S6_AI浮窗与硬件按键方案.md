# S6 AI 浮窗 + 硬件按键绑定 · 方案（已落地）

> 2026-09-27 · 方案 4 个拍板点用户确认/采用默认：① SET 三段=收窗 ✅ ② MODE
> IDLE 自动 start ✅ ③ M1+M2 一次性交付 ✅ ④ 记录跨开关保留 ✅。
> **实施记录见 `改动记录.md` §54**（overlay_ai.c/.h 新增、tab_ai.c/.h 删除、
> manager/page_main/CMake 接线；静态校验 3/3 OK）。

---

## 1. 现状盘点（全部有现成机制，无需造轮子）

| 机制 | 现状 | 与本方案的关系 |
|---|---|---|
| `middleware/periph/button_service` | 4 个 ADC 分压键（SET/MODE/VOL-/VOL+），iot_button 组件，回调跑在 **iot_button 任务上下文**（禁止阻塞/LVGL）；VOL± 内部自理，**SET/MODE 目前零消费者** | SET/MODE 干净可用；回调线程纪律是本方案核心约束 |
| `ui/overlay_manager` | lv_layer_top 上的**互斥浮层**调度（同一时刻只允许一个浮层，open 新的自动 hide 旧的） | AI 浮窗直接注册为第三种类型 |
| `ui/overlay_wallpaper` | 浮层先例：`lv_layer_top()` 上常驻构建 + y 轴滑入/滑出动画（`wallpaper_animate_to`，ease_out） | 「从底下弹出」照抄同款动画骨架 |
| `ui/tab_ai.c` | tabview 内一个 tab：标题/状态行/滚动对话/激活码行/按钮行（TALK-END 大按钮 + HOLD TALK PTT + CLEAR） | 内容整体搬迁为浮窗；tab_ai.c 退役 |
| `page_main.c` | tabview 7 tabs：CAM/VIDEO/FILES/REC/**AI**/SYS | 删 AI tab 注册 |
| `xiaozhi_service` | `start()/stop()/ptt_start()/ptt_stop()` 全部只置标志/队列，线程安全，异步状态机 | 按键回调可直接调用，无阻塞 |

---

## 2. 目标交互

### 2.1 按键语义（SET = 会话扳机，MODE = 按住说话）

```
SET 单击（状态机循环）：
  浮窗关闭                    → 浮窗从底部弹出(300ms滑入) + xiaozhi_start()
                                 （弹出与唤醒并行，start 本身异步 1~2s 建链）
  浮窗开 + IDLE(未会话)       → xiaozhi_start()      〔等价屏幕 TALK〕
  浮窗开 + CHATTING           → xiaozhi_stop()       〔等价 END；浮窗保留可看记录〕
  浮窗开 + IDLE(会话已结束)    → 收起浮窗回原 tab
  （即：一下唤起开说 → 再按结束 → 再按收窗；屏幕 X 钮随时可收窗）

MODE 按住 / 松开：
  按下  → 若 CHATTING：ptt_start()（屏幕 HOLD TALK 同步变红）
          若 IDLE：先自动 xiaozhi_start() + 记"待 PTT"，浮窗 timer 在进入
          CHATTING 后自动补 ptt_start()（建链 1~2s 内按住不丢）
  松开  → ptt_stop()
```

- 屏幕按钮**全部保留**（TALK/END、HOLD TALK、CLEAR、新增 X 收窗），硬件键
  只是等价触发；按钮与硬件键驱动**同一组请求函数**，状态由同一个 lv_timer
  刷新，天然不互斥打架。
- VOL-/VOL+ 行为零变化。

### 2.2 线程纪律（F44 同族，硬约束）

iot_button 回调上下文**只置 volatile 请求标志 + 调 xiaozhi_service_*（本来就
只置标志）**，绝不碰 LVGL。浮窗的 lv_timer（20ms）排空标志并刷新按钮/状态。
与 tab_ai 现有 evt_drain 范式一致。

---

## 3. 结构改动

### 3.1 新浮窗 `ui/overlay_ai.c/.h`（tab_ai.c 改造，文件替换）

- 注册进 overlay_manager：`overlay_type_t` 增 `OVERLAY_AI`；invoke_show/hide
  增分支（互斥：壁纸下拉与 AI 浮窗天然互斥，机制白拿）；
- 构建：`lv_layer_top()` 上全屏容器（800×480，COL_BG 不透明——盖住 tab 栏
  即挡住触摸，无穿透问题），常驻构建一次，show/hide = y 轴
  `+480 → 0 → +480` 滑动动画（wallpaper 同款）；
- 内容 = 现 tab_ai 全量（bin 字体对话记录/激活码行/三按钮行），底部按钮行
  右端加 **X 钮**：CHATTING 时先 stop() 再收窗（防 codec 悬挂）；
- 对外接口：
  - `void overlay_ai_init(void)`（构建 + `button_service_set_evt_cb` 注册——
    全仓 grep 确认当前无其它注册者，覆盖式注册无冲突）；
  - `void overlay_ai_key_feed(btn_id_t, btn_evt_t)`（内部实现直接由回调调用，
    只置标志）；事件路由规则：SET/MODE 进来，VOL± 忽略（保持音量键自理）；
- tab_ai.c/.h 删除（无其它引用者，page_main 之外无人 include）。

### 3.2 接线改动

| 文件 | 改动 |
|---|---|
| `overlay_manager.h/.c` | +OVERLAY_AI 类型与 show/hide 分支（~10 行） |
| `page_main.c` | 删 AI tab 的 add_tab/build/destroy；`main.c` UI 就绪后调 `overlay_ai_init()`（或 page_main 末尾） |
| `main/CMakeLists.txt` | SRCS：`tab_ai.c` → `overlay_ai.c` |
| `button_service` | **零改动** |

### 3.3 状态同步要点

- 会话状态文本/按钮色仍由 xiaozhi_service 事件队列驱动（现 evt_drain 机制
  原样搬）；硬件键触发的状态变化走同一队列，无需额外通路；
- MODE 按下时若 IDLE：`s_hw_ptt_pending=true`，timer 检测到 CHATTING 后补
  `ptt_start()`；若建链失败（ERROR 事件）则清 pending + 状态行提示；
- 浮窗 hide 时 lv_timer 保留（开销可忽略：无会话时事件队列为空）。

---

## 4. 里程碑（每步可验收）

| 步 | 内容 | 验收 |
|---|---|---|
| M1 | overlay_ai 浮窗化（tab_ai 平移 + X 钮 + manager 分支 + page_main 接线），SET/MODE 未绑 | 屏幕照常；AI 只能从浮窗进（暂留屏幕入口？见 §5 拍板点 3） |
| M2 | 按键绑定：SET/MODE 回调 → 标志 → timer 排空 → 上述状态机 | SET 弹窗+唤醒；MODE 按住说话松开收；SET 循环语义 |
| M3 | 收尾：动画手感、激活码/清屏回归、静态校验、烧录验收 | idf.py build + flash，全流程真机走查 |

---

## 5. 拍板点（请圈选）

1. **SET 第三段语义**：浮窗开 + 会话已结束（IDLE）时，SET = 收窗（本方案），
   还是 = 重新 start()（收窗只能靠 X）？本方案选前者，理由：三击循环
   "唤起→结束→收起"最顺手，X 钮兜底。
2. **MODE 在 IDLE 时按住**：自动 start() 等建链后自动开 PTT（本方案，代价
   是建链 1~2s 内白按住），还是忽略并状态行提示 "Tap TALK first"？
3. **过渡期屏幕入口**：M1 完成但 M2 未绑键前，AI 只能靠浮窗——需要临时把
   SYS 页加一个"打开 AI"入口吗？还是 M1+M2 一次性交付（改动量不大，
   倾向一次性）？
4. **浮窗是否保留对话记录跨开关**：浮窗 hide 时记录保留（本方案，X 只收窗
   不清记录；CLEAR 手动清），还是每次弹出清空？
