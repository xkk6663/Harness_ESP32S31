"""Windows SMTC（系统媒体总线）采集线程 —— 所有播放器自动注册，无需逐个对接。

winsdk 为可选依赖；缺失时 available=False，UI 提示安装即可，其余功能不受影响。
线程内自跑 asyncio 事件循环，1Hz 轮询当前会话（比事件回调跨线程更简单可靠）。
"""
from __future__ import annotations

import asyncio
import queue
import threading
from collections.abc import Callable


def _is_playing(status) -> bool:
    """★ winsdk 枚举 str() 是纯数字（实测 str='5'），"PLAYING" in str 恒 False！
    必须用 .name 比较（has_name=True）或 int 值（Playing=4）。"""
    return getattr(status, "name", "") == "PLAYING" or int(status) == 4


def _timeline_usable(tl) -> bool:
    """★ 判"这个播放器到底有没有上报时间轴"（2026-09-24，修"进度条 0~1s 徘徊"）。

    winsdk 对"注册了 SMTC 会话但不上报时间轴"的播放器（网易云 Win32 实测）返回的是
    **零值对象而不是 None**：position=timedelta(0) / end_time=timedelta(0) /
    last_updated_time=1601-01-01。若只判 `tl.position is not None`，会把 0 当成
    有效时刻 —— pusher 每秒收到 pos=0 打点，`_pos_at` 被反复刷新，内插值
    `_pos + (now - _pos_at)` 永远落在 0~1s ⇒ 设备端进度卡死、歌词不滚。

    判据改为"时间轴里有没有真实内容"，任一成立即视为可用：
      1) last_updated_time 是真实日期（年份 > 2000）；
      2) 有正曲长（end_time > 0）；
      3) 有正进度（position > 0）。
    """
    if tl is None or getattr(tl, "position", None) is None:
        return False
    lu = getattr(tl, "last_updated_time", None)
    if lu is not None and getattr(lu, "year", 0) > 2000:
        return True
    end = getattr(tl, "end_time", None)
    if end is not None and end.total_seconds() > 0:
        return True
    return tl.position.total_seconds() > 0


class SmtcWatcher:
    """on_track(dict) 换曲/播放态变化；on_position(float) 1Hz 进度。

    v0.3（2026-09-23）：增加 send_command() —— 设备端"上一首/播放/暂停/下一首"
    回程控制。命令入队（线程安全），_loop 每秒消费并调 winsdk 会话控制方法。
    """

    def __init__(self,
                 on_track: Callable[[dict], None],
                 on_position: Callable[[float, bool], None],
                 on_log: Callable[[str], None] | None = None) -> None:
        self.on_track = on_track
        self.on_position = on_position
        self.on_log = on_log or (lambda s: None)
        self.available = False
        self.current: dict = {}          # 最近一次 track 快照（线程安全：原子赋值）
        self.position: float = 0.0
        self.playing: bool = False
        self._last_app = ""              # 上次选中会话的应用（尽量保持粘性）
        self._cmd_q: "queue.Queue[str]" = queue.Queue()
        self._last_op = None             # 持有 winsdk 异步操作引用（防 GC 取消）
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None

    def send_command(self, cmd: str, pos_ms: int | None = None) -> None:
        """设备端媒体控制入口，任意线程可调。
        cmd: play/pause/next/prev；seek 需带 pos_ms（尽力而为：仅对上报 SMTC
        时间轴的播放器有效，网易云 Win32 不支持）。"""
        if cmd in ("play", "pause", "next", "prev"):
            self._cmd_q.put((cmd, None))
        elif cmd == "seek" and pos_ms is not None:
            self._cmd_q.put((cmd, int(pos_ms)))

    def start(self) -> None:
        self._thread = threading.Thread(target=self._run, name="smtc", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(1.5)
            self._thread = None

    def _run(self) -> None:
        try:
            import winsdk.windows.media.control as wmc  # noqa: F401
        except ImportError:
            self.on_log("winsdk 未安装（pip install winsdk），SMTC 采集停用 —— 副屏将无曲目来源")
            return
        self.available = True
        self.on_log("SMTC init ok（Windows 媒体总线已接管，等待播放器出声）")
        try:
            asyncio.run(self._loop())
        except Exception as e:  # 线程内兜底，避免静默死亡
            self.on_log(f"SMTC loop exited: {type(e).__name__}: {e}")

    async def _loop(self) -> None:
        from winsdk.windows.media.control import (
            GlobalSystemMediaTransportControlsSessionManager as Manager)

        manager = await Manager.request_async()
        last_key = None
        last_apps: frozenset = frozenset()
        while not self._stop.is_set():
            try:
                # ---- 会话选择：不能只跟 manager.get_current_session()（Windows 的
                #      "当前会话"是粘性的，B 站播过一次就一直霸占，网易云抢不到）。
                #      策略：优先"正在播放"的会话（同 app 粘性），全暂停则沿用上次。
                sessions = list(manager.get_sessions())
                apps = frozenset(s.source_app_user_model_id for s in sessions)
                if apps != last_apps:
                    last_apps = apps
                    self.on_log("SMTC 会话: " + (", ".join(sorted(apps)) if apps else "(空)"))

                playing = [s for s in sessions
                           if _is_playing(s.get_playback_info().playback_status)]
                best = None
                if playing:
                    best = next((s for s in playing
                                 if s.source_app_user_model_id == self._last_app), playing[0])
                else:
                    best = next((s for s in sessions
                                 if s.source_app_user_model_id == self._last_app), None)
                    if best is None and sessions:
                        best = sessions[0]
                if best is not None and best.source_app_user_model_id != self._last_app:
                    self._last_app = best.source_app_user_model_id
                    last_key = None       # 换会话强制刷一次 track
                    self.on_log(f"SMTC 选中会话: {self._last_app}"
                                + ("（播放中）" if playing else "（未播放）"))
                session = best

                # ---- 设备端回程媒体控制（NOW_CMD → winsdk 会话方法）----
                await self._drain_commands(sessions, playing)

                if session is None:
                    if last_key is not None:
                        last_key = None
                        self.current = {}
                        self.on_track({})
                    await asyncio.sleep(1.0)
                    continue

                props = await session.try_get_media_properties_async()
                tl = session.get_timeline_properties()
                status = session.get_playback_info().playback_status
                playing = _is_playing(status)

                title = props.title or ""
                artist = props.artist or ""
                album = props.album_title or ""
                duration = (tl.end_time.total_seconds() if tl.end_time else 0.0) or 0.0
                # ★ 不能写 `tl.position is not None`：网易云 Win32 给的是 timedelta(0)
                #   而不是 None，那样会把 0 当有效时刻（详见 _timeline_usable）。
                pos_known = _timeline_usable(tl)
                pos = (tl.position.total_seconds() if pos_known else 0.0) or 0.0

                self.position = pos
                self.playing = playing
                # pos_known=False（网易云等）：pusher 端内插自由推进，勿用 0 打点
                self.on_position(pos, playing, pos_known)

                key = (title, artist, playing)
                track = {"title": title, "artist": artist, "album": album,
                         "duration_s": duration, "playing": playing}
                if key != last_key:
                    last_key = key
                    self.current = track
                    if title or artist:
                        self.on_log(f"track: {artist} - {title}" + (" [暂停]" if not playing else ""))
                    self.on_track(track)
            except Exception as e:
                self.on_log(f"SMTC poll error: {type(e).__name__}: {e}")
            await asyncio.sleep(1.0)

    async def _drain_commands(self, sessions: list, playing: list) -> None:
        """消费命令队列（_loop 内 await 调用）。命令延迟 ≤1s（轮询周期）。"""
        while True:
            try:
                cmd, pos_ms = self._cmd_q.get_nowait()
            except queue.Empty:
                return
            target = None
            if self._last_app:
                target = next((s for s in sessions
                               if s.source_app_user_model_id == self._last_app), None)
            if target is None:
                target = next(iter(playing), sessions[0] if sessions else None)
            if target is None:
                self.on_log(f"SMTC 命令 {cmd} 无可用会话")
                return
            await self._exec_cmd(target, cmd, pos_ms)

    async def _exec_cmd(self, session, cmd: str, pos_ms: int | None = None) -> None:
        """★ await 版（§28.45 补充）：本地实验 await 100% 生效；fire-and-forget 虽然持引用
        实测也通过，但 await + 超时兜底最稳。winsdk IAsyncAction/IAsyncOperation 可直接 await。"""
        try:
            if cmd == "seek":
                import datetime
                await asyncio.wait_for(
                    session.try_change_playback_position_async(
                        datetime.timedelta(milliseconds=pos_ms or 0)), 3)
            else:
                fn = {"play": session.try_play_async,
                      "pause": session.try_pause_async,
                      "next": session.try_skip_next_async,
                      "prev": session.try_skip_previous_async}.get(cmd)
                if fn is None:
                    return
                await asyncio.wait_for(fn(), 3)
            self.on_log(f"SMTC -> {cmd}" + (f" {pos_ms}ms" if pos_ms is not None else "")
                        + f" ({session.source_app_user_model_id})")
        except Exception as e:
            self.on_log(f"SMTC {cmd} failed: {type(e).__name__}: {e}")
