# PC 副屏「歌词同步」位置真值方案 —— 讨论与执行计划

> 触发问题（2026-09-24）：网易云 Win32 的 SMTC **没有时间轴**，进度只能靠 PC 本地内插，
> 且从"我们开始计时的时刻"起算。用户指出：**若上位机在歌播到中途才启动，进度从 0 开始，
> 歌词同步就会一直错位，这个方案极难做准**。要求讨论替代方案并给出可执行计划。
>
> 本文分三部分：**① 误差结构（把问题拆对）** → **② 六条候选路线的实测结论** → **③ 执行计划**。
> 所有"实测"均附探针脚本，可复跑；"待验证"一律标明。

---

> ## ✅ 结案（2026-09-24）：本方案已全部落地并**实机验收通过**
>
> | 项 | 结论 |
> |---|---|
> | **R3（CEF CDP 读进度 DOM）** | ✅ **成立**，已实现为 `pcnow/netease_cdp.py`（F43 / §28.51） |
> | **P0（人工校准闭环 + 双通道）** | ✅ 代码完成 + **实机验收通过** | 
> | **P1** | ✅ 成立并落地 | 
> | **P2 / P3** | ⛔ **因 R3 成立而失去必要性，不做** |
> | **歌词对齐** | ✅ 追加 `PCNOW_LYR_LEAD_MS = 1250`（F47 / §28.56），验收后设备端与 PC 端对齐 |
>
> 验收口径：**行级 ±0.8s、不来回跳**（本文 §一 的误差结构推导）；实际以用户实机确认为准。
> 踩坑与规则沉淀见 `docs/工程规范/11_遇到的问题.md` **F43 / F45 / F47**。

---

## 一、先把问题拆对：误差从哪来

歌词同步的误差只有三个来源，量级差异极大：

| 误差源 | 量级 | 会不会累积 | 能否消除 |
|---|---|---|---|
| **漂移**：PC `time.monotonic()` vs 音频硬件的实际推进速率 | 几分钟内 **< 20 ms** | 几乎不累积 | 不需要 |
| **事件延迟**：换曲/暂停事件的检测延迟（SMTC 轮询 1 Hz） | **≤ 1 s**，瞬时 | 不累积 | 可消（提高轮询或事件驱动） |
| **初始锚点**：从"我们开始计时那一刻"起算，但歌可能已播了 x 秒 | **可从 0 到整首歌** | **永久累积** | **必须解决** |

### ⇒ 核心结论

**我们不需要"连续可读的位置"，只需要"能在关键时刻拿到一次绝对位置来校准锚点"。**

这一句把问题的难度降了一个数量级：

- 现状唯一真正会"一直错"的场景只有一个 —— **上位机启动 / 打开推送时，歌已经在播**。
- 只要这次偏差被校准掉，之后**内插是准的**（漂移可忽略、事件延迟不累积）。

### ⇒ 第二个结论：目标应当从"毫秒级同步"降级为"行级同步"

人耳判断歌词错位的能力大约在 **半行（1~2 s）**。所以正确的验收指标不是"±50 ms"，
而是：

> **≥95% 的歌词切换发生在 ±0.8 s 内，且不出现来回跳（提前一句又退回）。**

按这个指标，现状方案在**主用例（PC 常驻、从换曲开始听）下其实已经合格**，
不合格的只有"中途加入"这一个入口。

---

## 二、六条候选路线：实测结论与代价

> 探针都在 `.workbuddy/`，跑法见第 4.5 节。

### 实测事实清单（可直接引用）

| 编号 | 事实 | 证据 |
|---|---|---|
| **V1** | 网易云 PC 与 B 站 PC 的 SMTC 时间轴是**零值死轴**：`position=timedelta(0)`、`end_time=timedelta(0)`、`last_updated=1601-01-01`；但 `playback_status` 正常 | `probe_posknown.py` |
| **V2** | **Chromium 播 HTML5 `<audio>`：SMTC 拿到 `status=PLAYING` + `end_time=90.00s`（真值）+ `last_updated=2026`，但 `position` 停在 0.01s 不推进**（同一时刻页面内 `currentTime=3.21` 在走，已截图确认） | `probe_chromium_smtc.py` + `shots/edge_smoke.png` |
| **V3** | 网易云是 **CEF/Chromium 内核**（主窗口类名 `OrpheusBrowserHost`），并另开三个窗口：`DesktopLyrics`（桌面歌词）、`MiniPlayer`（迷你播放器）、`MediaPlayer SMTC window - {GUID}`（它自己注册的 SMTC） | `probe_ui_tree.py` |
| **V4** | MSAA：`OrpheusBrowserHost` 有 a11y 子树，**能读到 DOM 文本**（实测读到 `'上一行'`、`'位置'`）；而 `DesktopLyrics` / `MiniPlayer` 的 `accChildCount=0` —— **自绘窗口，读不到任何文本** | `probe_msaa.py` |
| **V5** | winsdk 的 UIA 投影**不完整**（只生成了 `AutomationElement`，没有 `Condition` / `TreeScope`），走 UIA 树遍历**必须新增依赖** | `winsdk/.../uiautomation/__init__.pyi` |
| **V6** | 环境已有：`winsdk` / `PIL` / `numpy` / `pyaudiowpatch` / `sounddevice` / `psutil` / `requests`；**没有** `uiautomation` / `pywinauto` / `comtypes` / OCR 库 | `probe_env_and_files.py` |
| **V7** | 主窗口 rect 实测 `(-32000,-32000,199x34)` = **当前最小化**，`PrintWindow` 返回全空 ⇒ **任何像素/OCR 方案都要求窗口可见** | `probe_window_shot.py` |
| **V8** | 网易云本地目录只有加密 `.uc` 音频缓存与广告 DB，**没发现可用于读取播放位置的本地存储** | `probe_env_and_files.py` |

### V2 的推论（很重要）

`position` 不推进**不是投影坏了**（`end_time`/`last_updated` 都是真值），而是
**Chromium 只在页面主动调用 `navigator.mediaSession.setPositionState()` 时才更新 position**。
YouTube 就是这么做的，所以 Chrome 里能看到进度。⇒ 换播放源能不能拿到 position，
**取决于那个页面/客户端有没有实现 Media Session API**。

### 路线对比

| # | 路线 | 能否拿到绝对位置 | 实测状态 | 代价 | 风险 |
|---|---|---|---|---|---|
| **R1** | **事件锚定 + PC 单调内插**（现状） | 换曲/暂停时准；中途加入不准 | ✅ 已实现 | 0 | 中途加入必错 |
| **R2** | **人工/半自动校准**：设备拖滑条 = 打点重锚 | 准（±0.5 s，取决于拖准程度） | ✅ **P0 已完成并实机验收**（2026-09-24）：无时间轴播放器上不再徒劳 SMTC seek，只重锚内插基准 | **低** | 要用户动一次手 |
| **R3** | **CEF 开 `--remote-debugging-port` 读 DOM** | **准（毫秒级）**，且 seek 也能用 | ✅ **成立并已落地**（2026-09-24，F43/§28.51，`netease_cdp.py`） | 低 | 已消解（代价：网易云须**启动时**带 flag，事后开不了） |
| **R4** | **UI a11y 读"位置"节点**（MSAA→UIA） | 准（±0.5 s，受 UI 刷新粒度限制） | ⛔ **不必做**（P2 已取消：R3 已解决真值问题） | — | — |
| **R5** | **换播放源**（网页版网易云 / 本地播放器） | 准（若有 `setPositionState`）或**完全准**（我们自控页面 + CDP） | ⛔ **不必做**（R3 已解决真值问题） | — | — |
| **R6** | 声纹对齐（设备麦克风 vs PC 音乐互相关） | 极准（±10 ms） | ❌ 不推荐 | **极高** | 算力/复杂度爆炸；`.uc` 加密 |

### 被排除的路线（附理由，避免以后重走）

- **`DesktopLyrics` 像素 OCR**：听起来很美（行级真值），但 V4 已证它是**自绘窗口、无 a11y 文本**，
  只能像素读；而它**独立于主窗口状态**（主窗口最小化它也可见）这点确实是优点 —— 但需要
  用户**常开桌面歌词**，且 OCR 要处理字体/缩放/换肤。**列为最后备选**。
- **读网易云本地文件**：V8，无可用位置存储。
- **WASAPI 回环音频**：只能给"是否真的在出声"，**给不了位置**。可作为 R1 的
  "播放态真值"补强（比 SMTC 更真实），但解决不了锚点问题。

---

## 三、推荐方案：分层降级（Anchor + Interpolate + Calibrate）

### 3.1 架构

```
                   ┌──────────────────────────────────────┐
   校准源（按可用性降级，谁在就用谁）                │
   ① 真时间轴（若播放器上报 position）  ──┐           │
   ② CEF CDP / DOM（若 R3 成立）        ──┤          │
   ③ UI a11y "位置"节点（若 R4 成立）   ──┤→ 锚点校准 ┤→ 单调时钟 ─→ pos_ms ─┐
   ④ 设备拖滑条（人工打点）             ──┤           │                    ├→ 设备
   ⑤ 换曲 / 暂停 / 继续 事件            ──┘           │                    │
                   └──────────────────────────────────────┘            └→ line(行号) ─┘
```

**关键设计：对外输出改成 `pos_ms` + `line` 双通道，设备以 `line` 为准切行。**

理由：把人耳敏感的"行切换"变成**PC 端算出的离散量**，于是

- 可以在一处（PC）做**迟滞 / 单调性约束 / 跳变抑制**（人耳对"来回跳"的容忍度远低于对"整体偏移"）；
- 校准动作的影响被限制在进度条视觉上，不会让歌词"抖回去"；
- 设备端逻辑更简单（`line` 可以直接用，不必自己判抖动）；⚠ 但**最终不是"照单显示"**——
  F47 后设备端仍要拿本机（带补偿）的值与其取 `max`，理由见 §3.2 与 ICD §7.5。

### 3.2 协议增量（ICD V1.3，向后兼容）

| 消息 | 现状 | 新增字段 |
|---|---|---|
| `NOW_PROG` | `{pos_ms, playing, hash}` | **`line`**（PC 算出的当前歌词行号，`-1` 表示无/未知） |
| `NOW_META` | `{..., duration_ms, cover, lrc, hash, playing}` | **`sync_src`**（`"timeline"` / `"dom"` / `"a11y"` / `"manual"` / `"event"`，供 UI 显示同步置信度） |

设备端：`s_prog_line`；行来源由 `lrc_find_locked()` 决定。
⇒ 老固件忽略新字段也能跑（退回现状行为），**协议向后兼容**。

> ⚠ **本节是 2026-09-24 的原始设计，取行规则已被 F47 修订**：原文写的是
> "`s_prog_line >= 0` 时**直接采信**，否则退回按 pos 二分"，落地时改成了
> `idx = max(idx_pc, idx_local)`（见 §4.1 的 P0-3 行与 ICD §7.5）。原因：设备端要对歌词做
> 时间提前量补偿，而 `line` 是**行号**、无法被时间偏移修正 ⇒ 若"line 优先"，补偿会在
> line 有效时**静默失效**。此处保留原文仅为记录决策演进。

---

## 四、执行计划

### 4.0 阶段总览

| 阶段 | 内容 | 产出 | 依赖 | 状态 |
|---|---|---|---|---|
| **P0** | 人工校准闭环（R2）+ 双通道协议 | 中途加入可手动对齐；歌词按行号显示 | 无 | ✅ **已实机验收（2026-09-24）** |
| **P1** | 试 CEF CDP（R3） | 成/败的**确定结论** | 用户同意重启网易云 | ✅ **成立并已落地**（2026-09-24） |
| **P2** | UI a11y 读位置（R4） | 自动校准（若 P1 败） | 用户同意保持窗口非最小化 | ⛔ **P1 已成，不必做** |
| **P3** | 换播放源评估（R5） | 产品决策 | 用户决策 | ⛔ 同上（P1 已解决真值问题） |
| **P4** | 兜底：现状 + 置信度提示 | 诚实告知用户 | 无 | ✅ 已随 P0 一起做完（置信度标签） |

**P0 / P1 均已落地；P2/P3 因 P1 成立而失去必要性。**

---

### P0 —— 人工校准闭环 + 双通道（今天可完成）

**目标**：把"中途加入必错"从"无解"降为"用户拖一下就对"，并让歌词切换不再受位置抖动影响。

| 任务 | 文件 | 具体做法 | 验收 |
|---|---|---|---|
| P0-1 | `pc_host/main.py` `on_link_json` | 网易云类**无时间轴**播放器收到 `NOW_CMD seek` 时：**不调** `try_change_playback_position_async`（徒劳），只做 `pusher.update_position(target, playing, pos_known=True)` 重锚，并打日志 `校准(meng): 内插 x.x s → 实际 y.y s（偏差 ±z.z s）` | 设备拖滑条后，PC 侧进度立刻跳到目标值，且不再尝试 SMTC seek |
| P0-2 | `pc_host/pcnow/pusher.py` | `_interpolated_pos()` 增加返回 `line`（`bisect_right(lrc_t, pos)-1`）；`_run()` 在 `NOW_PROG` 里带上 `"line"`；加**迟滞**：行号只允许"保持 / +1"，回退需连续 3 拍确认 | 连续推包中 `line` 单调不回跳 |
| P0-3 | `main/middleware/network/pcnow_service.c` | `handle_prog()` 解析 `line` → `s_prog_line`；`get_info()` 取 `idx = MAX(lrc_find_locked(lyr_pos_locked()), s_prog_line)` | 设备端按行号切行。**⚠ 已修订（F47）**：原设计是"`line` **优先采信**"，但那会让对齐提前量在"PC line 生效"时**静默失效** ⇒ 改为"取更靠后者"：正常播放由本机（带 1250ms 补偿）定行，位置突退时才由 PC 那个做过迟滞的值兜底 |
| P0-4 | `main/ui/tabs/tab_pcnow.c` | 歌词区加"同步源/置信度"小字（读 `sync_src`）；拖滑条时提示"已校准" | UI 能看出当前是真时间轴还是内插 |
| P0-5 | `docs/项目规划/通信接口规范_ICD.md` | 升到 V1.3，记录 `line` / `sync_src` | 文档与代码一致 |

**回退**：设备端忽略 `line` 即退回现状（P0-2 单独在 PC 侧加迟滞也无害）。

---

### P1 —— 试 CEF Chrome DevTools Protocol（R3，最高性价比）【2026-09-24 已实测：**成立并已落地**】

**假设 H2**：网易云 CEF 接受 `--remote-debugging-port`，于是能直接读 DOM 里的播放位置。
**⇒ 结论：接受，而且比预期好。**

#### P1-1 探测结果（全部真机实测）

```bat
"C:\Program Files\Netease\CloudMusic\cloudmusic.exe" --remote-debugging-port=9222
```

`/json/version` 返回：

```json
{"Browser":"Chrome/91.0.4472.169","Protocol-Version":"1.3",
 "User-Agent":"... NeteaseMusicDesktop/3.1.40.205461"}
```

`/json/list` 只有**一个** target：`type=page, title=网易云音乐, url=orpheus://orpheus/pub/app.html`。
（桌面歌词/迷你播放器窗口是原生窗口，不在 CEF 里 —— 与 MSAA 侧的结论一致。）

#### P1-2 关键修正：**`<audio>.currentTime` 这条路上没有真值**

探测推翻了原计划的假设，实测：

| 来源 | 实测值 | 可用性 |
|---|---|---|
| `<audio>` 元素 | `currentTime=0, duration=0, paused=true, src=""`（**恒为 0**） | ❌ |
| `navigator.mediaSession.playbackState` | `"none"`（恒为 none） | ❌ |
| `document.querySelectorAll('iframe')` | `0` | — |
| **进度 DOM 文本** | `"00:06 / 04:02"` → 5 秒后 `"00:11 / 04:02"` | ✅ **唯一真值** |

⇒ **这同时解释了 SMTC 为什么是零值死轴**：网易云既不走 `mediaSession`，也不更新
`<audio>` 的时间轴，SMTC 自然拿不到东西。**不是 winsdk 投影的问题。**

决定性实验（连采三拍，`probe_cef_cdp.py --play-check`）：

```
t1 = "00:06 / 04:02"  (sec=6,  dur=242)
t2 = "00:11 / 04:02"  (sec=11, dur=242)
ADVANCING: true        ← 5 秒过去，位置正好 +5 秒
```

#### P1-3 落地实现（已完成）

| 文件 | 内容 |
|---|---|
| **新增** `pc_host/pcnow/netease_cdp.py` | `NeteaseCdp`：**零第三方依赖**（stdlib + 内置最小 RFC6455 WS 客户端）；`probe()` / `poll() -> (pos_s, dur_s)`；失败按 2s→4s→…→30s 退避自动降级，绝不抛给主循环 |
| `pc_host/pcnow/pusher.py` | `attach_cdp()` 注入；`_run()` 里 1Hz 采样，三重门控后 `anchor(pos, "dom")`；`_resolve_dur()` 优先用 CDP 真值曲长；`snapshot()` 增加 `cdp_live` |
| `pc_host/main.py` | 装配 `NeteaseCdp` 并 `probe()` 打一条能力日志 |
| `pc_host/restart_cloudmusic_debug.bat` | 一键"关掉 → 带端口重启 → `/json/version` 自检"（纯 ASCII 避免码页乱码） |
| `.workbuddy/probe_cef_cdp.py` | 只读探针：`--play-check` 连采三拍判递增、`--lyric` 探查歌词 DOM |
| `.workbuddy/probe_p1_wire.py` | 闭环联调：pusher + CDP（`device_ip=None` ⇒ 不发任何 UDP 包） |

**三重门控**（任一不满足 ⇒ 原地退回 P0 的本地内插，行为完全不变）：

1. CDP 已接通（`:9222` 在且能连上 page target）；
2. **这一首没有 SMTC 时间轴**（`has_timeline` 现按曲判定：`_tl_key == (title, artist)`）——
   有真时间轴的播放器（Spotify 等）优先用 SMTC，不与 CDP 打架；
3. SMTC 说在播 **且 CDP 位置在动**（不动 ⇒ 网易云不是当前播放器，读到的是残留文本
   ⇒ 连续 4 拍停止采信并打日志）。

#### 联调实测结果

```
poll[0] -> (11.0, 242.0)  (raw='00:11 / 04:02')
poll[1] -> (13.0, 242.0)  (raw='00:13 / 04:02')
poll[2] -> (15.0, 242.0)  (raw='00:15 / 04:02')
[push] 进度走 CDP 真值: 15s / 242s（内插 0s → 修正 +15s）
t=  16.00s dur=  242.0s sync=dom cdp_live=True
t=  24.88s dur=  242.0s sync=dom cdp_live=True      ← 10/10 拍 dom，推进 9/9
```

⇒ **曲长从"曲库估算"变成播放器真值（242s）**；位置每秒被真值重锚 + 内插平滑补间。
原来的"初始锚点永久错"问题**从根上消失**（不再需要人工校准来救）。

> ⚠ 唯一新增约束：网易云要用**带调试端口**的方式启动（双击
> `pc_host\restart_cloudmusic_debug.bat`）。不做也能用，只是退回本地内插。

**验收**：`sync_src == "dom"` ✅；曲长 = 播放器真值 ✅；位置单调推进 ✅。

#### P1-4 新机会（**未做**，留给下一步）

CDP 既然能 evaluate，就能**派发事件**：对进度条 DOM 派发点击/拖动（或 `Input.dispatchMouseEvent`）
⇒ **设备端拖 seek 对网易云也可能可用**（原来 SMTC seek 对网易云无效、`main.py` 里是直接
return 跳过的）。这是 P1 成立后白捡的一条路，需要先确认网易云进度条的 CDP 点按是否生效
（风险：自绘 canvas + 需要精确坐标 ⇒ 可能要用 `getBoundingClientRect` 算宽度比例）。

---

### P2 —— UI a11y 读"位置"节点（R4，P1 失败时）

**假设 H3**：窗口**还原（非最小化）**后，CEF 的 a11y 树会完整暴露进度时间文本。

**P2-1（探测，10 分钟）**：请把网易云窗口**还原并置于前台**，然后跑

```bat
C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe .workbuddy\probe_msaa.py
```

看是否出现带时间样式的节点（`\d{1,2}:\d{2}`）。

**P2-2（若 MSAA 够用）**：把 `.workbuddy/probe_msaa.py` 的 MSAA 读取逻辑收进
`pc_host/pcnow/pos_probe.py`，每 5~10 s 读一次"位置"节点，与内插值比对，
偏差 > 2 s 就重锚（这就是**自动校准**）。**零新增依赖。**
**P2-3（若 MSAA 不够）**：给 `pc_host` 加可选依赖 `uiautomation`（V5：winsdk 的 UIA 投影不全），
用 UIA 的 `ValuePattern` / `TextPattern` 读数值；缺失时功能自动降级（`sync_src` 退回 `"event"`）。

**风险与对策**：UI 改版 ⇒ 节点名变化 ⇒ 加"选择器可配置 + 找不到就静默降级"，
永不因为读不到而让主功能不可用。

---

### P3 —— 换播放源（R5，需你拍板）

| 子方案 | 拿位置 | 难度 | 提示 |
|---|---|---|---|
| **R5a 网页版网易云 + 浏览器** | 若页面实现了 `mediaSession.setPositionState` ⇒ **完全准 + seek 可用**；若没实现 ⇒ 只有时长+状态（见 V2） | 低 | **先验证**：浏览器登录 music.163.com 播一首，跑 `probe_posknown.py` 看 `position` 是否推进 |
| **R5b 我们自控页面 + CDP** | **毫秒级最准** | 中 | 需音频 URL（VIP cookie），且有 ToS 顾虑 |
| **R5c 本地播放器**（MusicBee / PotPlayer 等自带 SMTC 的） | 准 | 中 | 需本地曲库；失去在线推荐 |

**取舍一句话**：**R5 是唯一能让"seek 也真正可用"的路线**（网易云 Win32 曲内跳转本来就无解）。
如果"设备当遥控器"是核心体验，值得认真考虑 R5a。

---

### P4 —— 兜底（无论 P1~P3 结果如何都要做）

1. 设备/PC UI 明确显示 **同步置信度**（`sync_src`）：真时间轴 / 内插 / 已人工校准；
2. 当 `sync_src == "event"` 且检测到"锚点失配迹象"（如多次人工校准）时，UI 提示
   "歌词可能不同步，可拖动进度条对齐"；
3. 在 `docs/工程规范/11_遇到的问题.md` 记 **F42**：网易云 SMTC 无时间轴的完整实测结论
   + V2（Chromium 只推状态和时长）—— 这两条以后一定还会被问到。

---

### 4.5 探针脚本索引（可复跑）

| 脚本 | 用途 | 跑法（项目根目录） |
|---|---|---|
| `.workbuddy/probe_posknown.py` | 看各 SMTC 会话的时间轴真值 | `cd pc_host && <venv>/python ../.workbuddy/probe_posknown.py` |
| `.workbuddy/probe_pusher_clock.py` | 验证内插时钟推进/暂停固化 | 同上 |
| `.workbuddy/probe_env_and_files.py` | 依赖清单 + 网易云本地目录 | `<venv>/python .workbuddy/probe_env_and_files.py` |
| `.workbuddy/probe_ui_tree.py` | 枚举网易云全部窗口（含隐藏） | 同上 |
| `.workbuddy/probe_window_shot.py` | 截图各窗口 → `.workbuddy/shots/` | 同上 |
| `.workbuddy/probe_msaa.py` | MSAA 读各窗口文本节点 | 同上 |
| `.workbuddy/probe_chromium_smtc.py` | Chromium 时间轴冒烟测试（隔离实例，跑完自动关闭，临时目录留在 `%TEMP%`） | 同上 |

> `<venv>` = `C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe`

---

## 五、需要你拍板的三个决策点

| 问题 | 影响 |
|---|---|
| **Q1** 能否接受听歌时**网易云窗口保持还原（不最小化）**？ | 决定 R4（UI 自动校准）是否可行。若坚持最小化，R4 基本作废（V7） |
| **Q2** 能否接受**改用浏览器/网页版**听歌？ | 决定 R5a —— 唯一能让 **seek 真正可用** 的路线 |
| **Q3** 能否接受**偶尔在设备上拖一下进度条做校准**？ | 若接受，**P0 做完就够用**，P1~P3 都可延后甚至不做 |

**我的建议**：先做 **P0**（低成本、立刻改善、且是后面所有方案的公共底座），
然后花 5 分钟试 **P1**（性价比极高，失败也无损失）。P2/P3 等 Q1/Q2 的答复。

---

## 附：为什么"行级同步"是对的抽象

顺带说明一个容易走偏的点：现状协议是"**PC 下发全量 LRC，设备自己按位置算行**"。
这在位置准的时候没问题，但它把"位置精度"和"显示正确性"**强耦合**了 ——
位置抖一下就整行跳，而且设备端没有任何"平滑/迟滞"的空间。

改成"PC 算行号、设备只显示"之后：

- PC 端可以同时掌握 **LRC 全量 + 位置 + 行号**，做单点决策（迟滞、单调、异常抑制）；
- 设备端变成纯展示，代码更简单、更省 RAM；
- **换位置来源（R1→R5）时，设备端一行都不用改**，这是这一层解耦最大的价值。
