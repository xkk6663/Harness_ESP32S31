"""NOW_* 推流器 + 曲库缓存（TrackStore）。

职责（对齐 C# agent）：
  - 换曲时后台解析网易云（歌词/封面），结果入 TrackStore 供 HTTP 服务读取；
  - 50ms tick 内插进度时钟；META（换曲+3s 周期）/ PROG（1Hz）/ LRC（v1 行推回退）；
  - hash = SHA1(title|artist)[:8]，与设备端 pcnow_service 一致。
线程模型：SMTC 回调（asyncio 线程）→ update_track()/update_position()（只置字段）；
本线程做 UDP 发送与网易云阻塞请求；UI 只读 state/track 属性。
"""
from __future__ import annotations

import socket
import threading
import time
from collections.abc import Callable

from . import netease

NOW_PORT = 50004


class TrackStore:
    """hash -> TrackData，供 HTTP 线程读取（lock 保护）。"""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._data: dict = {}
        # ★ F44：设备已成功拉走全量歌词的 hash。有了它才能让 NOW_LRC 单行通道
        #   自动闭嘴 —— 全量歌词（v1.1）到位后，每换一行再 UDP 推一次是纯冗余。
        self._pulled: set = set()

    def put(self, td: "netease.TrackData") -> None:
        with self._lock:
            self._data[td.hash] = td

    def get(self, k: str):
        with self._lock:
            return self._data.get(k)

    def mark_lyric_pulled(self, k: str) -> None:
        """HTTP :50005 /lyrics.json 200 命中时由 http_server 调用。"""
        with self._lock:
            self._pulled.add(k)

    def lyric_pulled(self, k: str) -> bool:
        with self._lock:
            return k in self._pulled


class NowPusher:
    def __init__(self, device_ip_provider: Callable[[], str | None],
                 store: TrackStore | None = None,
                 on_log: Callable[[str], None] | None = None) -> None:
        self.get_device_ip = device_ip_provider
        self.store = store or TrackStore()
        self.on_log = on_log or (lambda s: None)

        self.enabled = False                 # UI 开关（关=不推任何包）
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

        self._lock = threading.Lock()
        self._track: dict = {}               # SMTC 最近 track（含 playing/duration）
        self._pos = 0.0                      # SMTC 最近进度
        self._pos_at = 0.0                   # monotonic 基准
        self._sent_meta_key = None
        self._last_meta = 0.0
        self._last_prog = 0.0
        self._last_lrc_line = -1
        self._resolving: str | None = None   # 正在解析的 hash（防重复）
        self._seq = 0

        # ---- P0-2：行号通道 + 迟滞状态（只在 pusher 线程写） ----
        self._last_line = -1                 # 已对外发布的行号（迟滞后）
        self._back_cnt = 0                   # 连续"回退"计数
        self._lyr_obj = None                 # 行时间戳缓存所属 TrackData（对象身份，见 _lyr_times）
        self._lyr_ts: list[int] = []         # LRC 各行起始**毫秒**（升序，同设备端）
        # 同步来源：timeline(真时间轴) / dom(CDP) / a11y(界面) / manual(人工校准) / event(仅事件锚定)
        self._sync_src = "event"
        # ★ "这一首到底有没有 SMTC 时间轴"（P0-1，P1 里精化为**按曲**判定）：
        #   由 update_position(pos_known=True) 置位，并记住"是哪一首测出来的"。
        #   `pos_known` 是每秒来的 ⇒ 换了播放器/换了曲，最迟 1 拍就会重判，
        #   不会像"全局一次置位"那样被上一个播放器的能力粘住。
        #   决定设备端拖 seek 时**要不要真去调 SMTC seek** —— 对网易云那是徒劳。
        self.has_timeline = False
        self._tl_key: tuple[str, str] = ("", "")   # has_timeline 属于哪一首

        # ---- P1：网易云/CEF 的位置真值（见 pcnow/netease_cdp.py） ----
        #   网易云 SMTC 是零值死轴、<audio>.currentTime 恒 0，唯一真值在进度 DOM
        #   文本 "mm:ss / mm:ss"。main.py 注入 NeteaseCdp；**不注入 = 完全维持 P0 行为**。
        self._cdp = None
        self._cdp_dur = 0.0                        # CDP 读到的曲长（真值）
        self._cdp_dur_key: tuple[str, str] = ("", "")   # 该曲长属于哪一首
        self._cdp_next = 0.0                       # 下次允许 poll 的时刻（1Hz 限频）
        self._cdp_last_pos = -1.0                  # 上一次 CDP 位置（判"在不在动"）
        self._cdp_stale = 0                        # 连续"没动"拍数
        self._cdp_live = False                     # 当前是否在采信 CDP 真值
        self._cdp_announced = False                # 只播报一次"已切到真值"

    _BACK_N = 3                              # 行号回退需连续 N 拍确认

    @property
    def sync_src(self) -> str:
        return self._sync_src

    # ---------- 供 SMTC 线程调用（只置字段，不做 IO） ----------
    def update_track(self, track: dict) -> None:
        with self._lock:
            prev = self._track
            same_song = ((prev.get("title"), prev.get("artist"))
                         == (track.get("title"), track.get("artist")))
            self._track = dict(track)
            if not same_song:
                # 换曲：SMTC 无时间轴（网易云）时从 0 重新内插
                self._pos = 0.0
                # 换曲后行号/同步来源全部重置（新歌从此刻开始锚定）
                self._last_line = -1
                self._back_cnt = 0
                self._sync_src = "event"
            self._pos_at = time.monotonic()
            self._sent_meta_key = None       # 换曲立即发 META
            if track and track.get("playing"):
                self._seq = 0

    def update_position(self, pos: float, playing: bool, pos_known: bool = True) -> None:
        """SMTC 每秒打点。pos_known=False 表示该播放器不上报时间轴（网易云 Win32
        实测 pos/end 全 None）—— 此时绝不能用 0 打点（会每秒把内插时钟清零），
        内插自由推进，仅在播放态切换时固化/重置基准。"""
        with self._lock:
            if pos_known:
                self._pos = pos
                self._pos_at = time.monotonic()
                self.has_timeline = True
                # 记住"这一首"有时间轴（见 __init__ 注释：按曲判定，不粘上一位播放器）
                self._tl_key = (self._track.get("title", ""), self._track.get("artist", ""))
                if self._sync_src != "manual":      # 有真时间轴就以它为准
                    self._sync_src = "timeline"
            else:
                was = bool(self._track.get("playing"))
                if playing != was:
                    if was:                  # 播放→暂停：固化已播时长
                        self._pos += time.monotonic() - self._pos_at
                    self._pos_at = time.monotonic()
            if self._track:
                self._track["playing"] = playing

    def anchor(self, pos: float, source: str = "manual") -> float:
        """★ 把内插时钟重锚到 pos（人工/界面校准的唯一入口，P0-1）。

        返回**重锚前**的内插值，供调用方打"内插 x → 实际 y（偏差 z）"的日志。
        注意：本方法**不改** playing，也不碰 _track；SMTC 的后续
        update_position(..., pos_known=False) 不会覆盖它（那条分支只在播放态
        切换时改 _pos）。
        """
        with self._lock:
            playing = bool(self._track.get("playing"))
            before = self._pos + (time.monotonic() - self._pos_at if playing else 0.0)
            self._pos = max(0.0, float(pos))
            self._pos_at = time.monotonic()
            self._sync_src = source
            self._back_cnt = 0          # 校准后允许行号自由重定位
        return before

    def _lyr_times(self, h: str) -> list[int]:
        """当前 hash 的 LRC 行起始**毫秒**（带缓存，避免 50ms tick 里反复重建列表）。

        ★★ 缓存键必须是 TrackData 的**对象身份**，绝不能只记 hash。
        这是个真实踩过的坑（F45）：换曲后**第一个 tick** 时网易云还没解析完，
        `store.get(h)` 是 None；若此刻把 h 记进缓存，等 2~5s 后歌词到位（hash 并未
        改变）就会永远命中那张空表 ⇒ `_line_for()` 恒 -1 ⇒ **line 通道全曲失效**，
        同时 NOW_LRC 回退通道也被 `not self._lyr_ts` 短路 ⇒ 设备端收不到任何行号，
        只能退回"本机按 pos_ms 二分"，而那条路又踩了 t 单位（秒 vs 毫秒）的坑
        ⇒ 三行歌词定在最后一句不动。
        """
        td = self.store.get(h) if h else None
        if td is not self._lyr_obj:
            self._lyr_obj = td
            self._lyr_ts = [x[0] for x in td.lyrics] if (td and td.lyrics) else []
        return self._lyr_ts

    def _line_for(self, pos_ms: int, h: str) -> int:
        """当前歌词行号（-1 = 无歌词/未知）。pos 单位**毫秒**（与设备端一致）。"""
        if not h:
            return -1
        import bisect
        ts = self._lyr_times(h)
        if not ts:
            return -1
        return bisect.bisect_right(ts, pos_ms) - 1

    def _stable_line(self, raw: int) -> int:
        """★ 行号迟滞（P0-2）：允许「保持」或「前进」（含手动校准后的大跳），
        回退必须连续 _BACK_N 拍确认才生效。

        为什么需要：人耳对"歌词来回跳"的容忍度远低于对"整体偏移"的容忍度。
        位置抖动 1~2s 时，二分结果会在相邻两行之间反复横跳；在这里一次性
        抑制掉，设备端就只收到单调、干净的行号序列。
        只在 pusher 线程调用。
        """
        cur = self._last_line
        if raw < 0 or cur < 0 or raw >= cur:
            self._last_line = raw
            self._back_cnt = 0
            return raw
        self._back_cnt += 1
        if self._back_cnt >= self._BACK_N:
            self._last_line = raw
            self._back_cnt = 0
            return raw
        return cur

    def _resolve_dur(self, track: dict) -> float:
        """曲长：CDP 真值（网易云）> SMTC 时间轴（B 站等上报的播放器）> 网易云曲库 duration。"""
        # ★ P1：只认"这一首"的 CDP 曲长 —— 不按曲判会把上一首的曲长带过来
        if self._cdp_dur > 0 and self._cdp_dur_key == (track.get("title", ""),
                                                      track.get("artist", "")):
            return self._cdp_dur
        dur = track.get("duration_s") or 0.0
        if dur <= 0 and (track.get("title") or track.get("artist")):
            td = self.store.get(netease.track_hash(track.get("title", ""),
                                                   track.get("artist", "")))
            if td and td.duration_ms:
                dur = td.duration_ms / 1000.0
        return dur

    # ---------- 开关 ----------
    def attach_cdp(self, cdp) -> None:
        """注入网易云 CDP 真值读取器（P1，`pcnow/netease_cdp.py`）。

        不注入（或注入的对象 probe() 失败）= 完全维持 P0 的本地内插行为，
        所以这是**可选增强**，不是新依赖。
        """
        self._cdp = cdp

    def set_enabled(self, on: bool) -> None:
        self.enabled = on
        self.on_log(f"副屏推送 {'开启' if on else '关闭'}")

    def start(self) -> None:
        if self._thread:
            return
        self._thread = threading.Thread(target=self._run, name="now-push", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(1.5)
            self._thread = None
        self._sock.close()

    # ---------- UI 只读快照 ----------
    def snapshot(self) -> dict:
        with self._lock:
            t = dict(self._track)
            dur = self._resolve_dur(t) if t else 0.0
            if t:
                if t.get("playing"):
                    t["pos_s"] = min(self._pos + (time.monotonic() - self._pos_at),
                                     dur or 1e9)
                else:
                    t["pos_s"] = self._pos
            t["sync_src"] = self._sync_src
            t["line"] = self._last_line
            t["cdp_live"] = self._cdp_live
        t["duration_s"] = dur if t else 0.0
        # hash 放锁外算（快照带 hash，页面才能查 TrackStore；§28.43 补充）
        if t and (t.get("title") or t.get("artist")):
            t["hash"] = netease.track_hash(t.get("title", ""), t.get("artist", ""))
        return t

    # ---------- 内部 ----------
    def _send(self, obj: dict) -> None:
        ip = self.get_device_ip()
        if not ip:
            return
        import json
        try:
            self._sock.sendto(json.dumps(obj, ensure_ascii=False).encode("utf-8"),
                              (ip, NOW_PORT))
        except OSError as e:
            if self._seq % 200 == 0:
                self.on_log(f"UDP 发送失败: {e}")
            self._seq += 1

    def _resolve_async(self, title: str, artist: str, h: str) -> None:
        """换曲解析放**独立线程**（§28.43 补充）：解析与推送原本同线程，实测解析
        偶发挂住（DNS getaddrinfo 不受 requests timeout 管）会连 META/PROG 一起
        拖死 —— 设备端全无。分离后解析再慢也只影响封面/歌词，曲名/进度照推。
        """
        if self._resolving == h:
            return
        self._resolving = h
        self.on_log(f"解析: {artist} - {title} (hash {h})")
        threading.Thread(target=self._resolve_worker, args=(title, artist, h),
                         name="now-resolve", daemon=True).start()

    def _resolve_worker(self, title: str, artist: str, h: str) -> None:
        try:
            td = netease.resolve_track(title, artist)
        except Exception as e:
            self.on_log(f"解析失败: {type(e).__name__}: {e}")
            return
        self.store.put(td)
        self.on_log(f"lyrics: {len(td.lyrics)} lines"
                    + (f" ({td.lyric_err})" if td.lyric_err and not td.lyrics else "")
                    + f" | cover: {len(td.cover)}B" + (f" ({td.cover_err})" if td.cover_err else ""))

    def _interpolated_pos(self) -> tuple[float, bool, float, int]:
        """→ (秒, playing, 曲长秒, 行号)。行号经迟滞处理，可直接下达设备。"""
        with self._lock:
            t = dict(self._track)
            playing = bool(t.get("playing"))
            pos = self._pos + (time.monotonic() - self._pos_at if playing else 0.0)
            dur = self._resolve_dur(t)
            if dur and pos > dur:
                pos = dur
        h = (netease.track_hash(t.get("title", ""), t.get("artist", ""))
             if (t.get("title") or t.get("artist")) else "")
        # ★ 行号比较用**毫秒**（F45）：LRC 时间戳与设备端 s_lrc_t[] 同为毫秒，
        #   设备端在 line 越界/缺失时也会退回同一个单位做二分。
        line = self._stable_line(self._line_for(int(round(pos * 1000)), h))
        return pos, playing, dur, line

    def _run(self) -> None:
        last_resolve_key = None
        while not self._stop.is_set():
            t0 = time.monotonic()
            with self._lock:
                track = dict(self._track)
            title = track.get("title", "")
            artist = track.get("artist", "")

            # 换曲解析（一次）—— ★ 与推送开关解耦（2026-09-23）：SMTC 抓到新曲就
            # 立刻解析封面/歌词入缓存，无论开关状态；这样打开推送（或换曲后开）
            # 都是秒出画面，不用"先开推送→干等解析"。
            if (title or artist) and last_resolve_key != (title, artist):
                last_resolve_key = (title, artist)
                self._last_lrc_line = -1
                try:
                    self._resolve_async(title, artist, netease.track_hash(title, artist))
                except Exception as e:
                    self.on_log(f"解析失败: {type(e).__name__}: {e}")

            # ★ P1（§28.51）：网易云/CEF 的**位置真值** —— 进度 DOM 文本。
            #   三重门控，任一不满足就原地退回本地内插（行为与 P0 完全一致）：
            #   ① CDP 已接通；② 这一首**没有** SMTC 时间轴（网易云/B 站恒如此）；
            #   ③ SMTC 说在播 且 CDP 位置**在动**（不动 ⇒ 网易云不是当前播放器，
            #      读到的是残留文本 ⇒ 连续 4 拍停止采信）。
            if self._cdp is not None and self.enabled:
                has_tl = self.has_timeline and self._tl_key == (title, artist)
                if not has_tl:
                    now_cdp = time.monotonic()
                    if now_cdp >= self._cdp_next:
                        self._cdp_next = now_cdp + 1.0        # 1Hz 足够（真值无漂移）
                        got = None
                        try:
                            got = self._cdp.poll()
                        except Exception as e:                # noqa: BLE001
                            self.on_log(f"CDP poll 异常: {type(e).__name__}: {e}")
                        if got is not None:
                            cpos, cdur = got
                            if cdur > 0:
                                self._cdp_dur = cdur
                                self._cdp_dur_key = (title, artist)
                            moved = (self._cdp_last_pos < 0 or cpos != self._cdp_last_pos)
                            self._cdp_last_pos = cpos
                            if moved:
                                self._cdp_stale = 0
                                if bool(track.get("playing")):
                                    before = self.anchor(cpos, source="dom")
                                    if not self._cdp_announced:
                                        self._cdp_announced = True
                                        self.on_log(
                                            f"进度走 CDP 真值: {cpos:.0f}s / {cdur:.0f}s"
                                            f"（内插 {before:.0f}s → 修正 {cpos - before:+.0f}s）")
                            else:
                                self._cdp_stale += 1
                                if self._cdp_stale == 4 and self._cdp_live:
                                    self.on_log("CDP 进度文本静止 4 拍（网易云非当前播放器？）"
                                                "—— 退回本地内插")
                            self._cdp_live = moved
                        elif self._cdp_live:
                            self._cdp_live = False
                            self.on_log("读不到网易云进度文本 —— 退回本地内插")

            # 推送（开关只挡这里）：META 换曲立即/3s 周期、PROG 1Hz、LRC 行推
            if self.enabled and (title or artist):
                h = netease.track_hash(title, artist)
                pos, playing, dur, line = self._interpolated_pos()

                # ★ F44：**就绪才宣告** —— cover/lrc 字段只在 TrackStore 里真的有
                #   这一份数据时才填 hash，否则填空串。
                #   为什么必须这样：换曲那一刻 META 先到、网易云解析（2~5s）还没回来，
                #   设备端凭 hash 去 HTTP 拉必然 404；旧实现失败后清 want、3s 后再试 ⇒
                #   换曲后的头十几秒里，一遍遍 404 + 一遍遍 96KB 封面重试，把 UDP
                #   收包任务堵死（丢包）。填空串时设备端**保持现状不动**，等解析完成
                #   元数据一变、META 补一拍，设备端才拉 —— 每首歌恰好拉一次。
                td = self.store.get(h) if h else None
                have_cover = bool(td and td.cover)
                have_lrc = bool(td and td.lyrics)

                # ⚠ 字段名与设备端 pcnow_service.c handle_meta 严格对齐：
                #   duration_ms（毫秒）/ cover+lrc（hash，设备凭 hash 才走 HTTP 拉取）
                # ★ 判重键带曲长/就绪位：解析完成后 key 变化立即补发，不必干等 3s。
                # ★ sync_src（ICD V1.3）：供设备端显示"同步置信度"。
                meta_key = (title, artist, int(dur), self._sync_src, have_cover, have_lrc)
                now = time.monotonic()
                # ★ 1s 下限：sync_src 会在 dom/event 之间抖（CDP 偶发读到静止文本），
                #   若只判 "key 变了就发"，抖动会给设备端打出一串无意义的 META。
                changed = meta_key != self._sent_meta_key
                if (changed and now - self._last_meta >= 1.0) or now - self._last_meta >= 3.0:
                    self._send({"type": "NOW_META", "title": title, "artist": artist,
                                "album": track.get("album", ""), "player": "PC",
                                "duration_ms": int(dur * 1000),
                                "cover": h if have_cover else "",
                                "lrc": h if have_lrc else "",
                                "hash": h, "playing": playing,
                                "sync_src": self._sync_src})
                    self._sent_meta_key = meta_key
                    self._last_meta = now

                # PROG：1Hz（设备端 handle_prog 读 pos_ms；line 为 V1.3 新增行号通道）
                if now - self._last_prog >= 1.0:
                    self._send({"type": "NOW_PROG", "pos_ms": int(pos * 1000),
                                "playing": playing, "hash": h, "line": line})
                    self._last_prog = now

                # LRC（v1 回退通道）：行变化时推 —— 但设备端一旦成功拉走**全量**歌词
                # （v1.1），它自己按 pos/PC line 定位即可，这条通道纯冗余，自动关掉。
                # 判定来自 HTTP :50005 /lyrics.json 是否 200 命中过该 hash（F44）。
                # ★ t 为**毫秒**（ICD §7.3，与 /lyrics.json 同源；F45）。
                if (line >= 0 and line != self._last_lrc_line and self._lyr_ts
                        and not self.store.lyric_pulled(h)):
                    self._last_lrc_line = line
                    td2 = self.store.get(h)
                    if td2 and td2.lyrics and line < len(td2.lyrics):
                        self._send({"type": "NOW_LRC", "hash": h,
                                    "t": td2.lyrics[line][0],
                                    "text": td2.lyrics[line][1]})
            time.sleep(max(0.02, 0.05 - (time.monotonic() - t0)))
