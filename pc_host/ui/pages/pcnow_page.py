"""音乐副屏页：SMTC 采集开关 + 当前曲目卡（封面/进度）+ 推送状态 + 日志。"""
from __future__ import annotations

import queue

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QPixmap
from PySide6.QtWidgets import (QHBoxLayout, QLabel, QPlainTextEdit, QPushButton,
                               QVBoxLayout, QWidget)

from ..widgets import make_page_header, v_card

_LOG_BURST = 40          # 每个定时器拍最多搬这么多行，防一次卡住 UI


def _fmt_time(s: float) -> str:
    s = int(s)
    return f"{s // 60}:{s % 60:02d}"


# 同步来源的中文显示（ICD V1.3 的 sync_src）
_SYNC_TXT = {
    "timeline": "真时间轴（准）",
    "dom": "CDP 读 DOM（准）",
    "a11y": "界面读取（准）",
    "manual": "已人工校准",
    "event": "本地内插（可能偏移）",
}


class PcNowPage(QWidget):
    def __init__(self, ctx) -> None:
        super().__init__()
        self.ctx = ctx
        root = QVBoxLayout(self)
        root.setContentsMargins(24, 20, 24, 20)
        root.setSpacing(14)

        root.addWidget(make_page_header(
            "音乐副屏", "把电脑正在播放的音乐实时推到设备屏幕 · SMTC + 网易云歌词/封面 · UDP 50004 / HTTP 50005"))

        body = QHBoxLayout()
        body.setSpacing(14)

        # 左：封面
        left, left_lay = v_card()
        self.cover = QLabel("♪")
        self.cover.setFixedSize(200, 200)
        self.cover.setAlignment(Qt.AlignCenter)
        self.cover.setStyleSheet(
            "background:#171c26; border:1px solid #273041; border-radius:12px;"
            "font-size:56px; color:#3b4960;")
        left_lay.addWidget(self.cover, 0, Qt.AlignHCenter)
        body.addWidget(left)

        # 右：曲目 + 控制
        right, right_lay = v_card("正在播放")
        self.title = QLabel("—")
        self.title.setObjectName("TrackTitle")
        self.artist = QLabel("等待播放器出声…")
        self.artist.setObjectName("TrackArtist")
        right_lay.addWidget(self.title)
        right_lay.addWidget(self.artist)

        from PySide6.QtWidgets import QProgressBar
        self.progress = QProgressBar()
        self.progress.setRange(0, 1000)
        self.progress.setFormat("0:00 / 0:00")
        right_lay.addWidget(self.progress)

        row = QHBoxLayout()
        self.toggle_btn = QPushButton("开启推送")
        self.toggle_btn.setObjectName("Primary")
        self.toggle_btn.setCheckable(True)
        self.smtc_badge = QLabel("SMTC")
        self.smtc_badge.setObjectName("Badge")
        row.addWidget(self.toggle_btn)
        row.addSpacing(8)
        row.addWidget(self.smtc_badge)
        row.addStretch(1)
        right_lay.addLayout(row)

        self.info = QLabel("")
        self.info.setObjectName("Secondary")
        self.info.setWordWrap(True)
        right_lay.addWidget(self.info)
        right_lay.addStretch(1)
        body.addWidget(right, 1)
        root.addLayout(body, 2)

        # 日志
        log_card, log_lay = v_card("日志")
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(400)
        log_lay.addWidget(self.log, 1)
        root.addWidget(log_card, 1)

        self.toggle_btn.toggled.connect(self._on_toggle)
        self._last_cover_key = None
        self._cover_painted = False

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._poll)
        self._timer.start(500)

    # ---- 回调 ----
    def _on_toggle(self, on: bool) -> None:
        self.ctx.pusher.set_enabled(on)
        self.toggle_btn.setText("关闭推送" if on else "开启推送")

    def _poll(self) -> None:
        # ★ F44：先在 Qt 线程里排空工作线程投来的日志（这一步必须在最前，
        #   否则日志积压会让下面本次刷新看到的状态显得"落后"）。
        self._drain_log()

        watcher = self.ctx.smtc
        if watcher is not None and watcher.available:
            if not self.smtc_badge.property("ok"):
                self.smtc_badge.setProperty("ok", True)
                self.smtc_badge.setText("SMTC 可用")
                self.smtc_badge.style().unpolish(self.smtc_badge)
                self.smtc_badge.style().polish(self.smtc_badge)
        elif not self.smtc_badge.property("warn"):
            self.smtc_badge.setProperty("warn", True)
            self.smtc_badge.setObjectName("BadgeWarn")
            self.smtc_badge.setText("SMTC 不可用（pip install winsdk）")
            self.smtc_badge.style().unpolish(self.smtc_badge)
            self.smtc_badge.style().polish(self.smtc_badge)

        track = self.ctx.pusher.snapshot()
        title = track.get("title", "")
        self.title.setText(title or "—")
        dur = track.get("duration_s") or 0.0
        pos = track.get("pos_s", 0.0)
        self.artist.setText(
            (track.get("artist", "") or "—")
            + ("　·　已暂停" if track and not track.get("playing") else ""))
        if dur > 0:
            self.progress.setValue(int(pos / dur * 1000))
            self.progress.setFormat(f"{_fmt_time(pos)} / {_fmt_time(dur)}")

        # 封面：每拍查缓存 —— 解析是异步的（换曲后几秒才就绪），hash 变化那一拍
        # 多半还没好；必须持续重试，就绪后上图（一曲只画一次）。
        h = track.get("hash") or ((track.get("title") or "") + "|" + (track.get("artist") or ""))
        if h != self._last_cover_key:
            self._last_cover_key = h
            self._cover_painted = False
            self.cover.setText("♪")
        if not self._cover_painted and track.get("hash"):
            td = self.ctx.store.get(track["hash"])
            if td is not None and td.cover:
                pm = QPixmap()
                pm.loadFromData(td.cover)
                if not pm.isNull():
                    self.cover.setPixmap(pm.scaled(200, 200, Qt.KeepAspectRatioByExpanding,
                                                   Qt.SmoothTransformation))
                    self._cover_painted = True

        if title:
            td2 = self.ctx.store.get(track.get("hash") or "")
            lyric_n = len(td2.lyrics) if td2 else 0
            # ★ lyrics 的时间戳是**毫秒**（ICD §7.3，F45），pos 是秒 ⇒ 必须换算
            cur = (td2.lyrics and next((x for x in td2.lyrics if x[0] <= pos * 1000),
                                       (0, "")) if lyric_n else None)
            self.info.setText(
                f"hash {track.get('hash', '-')}　·　歌词 {lyric_n} 行　·　封面 {'就绪' if td2 and td2.cover else '无'}"
                + f"　·　同步 {_SYNC_TXT.get(track.get('sync_src', ''), track.get('sync_src', '-'))}"
                + (f"　·　行 {(track.get('line', -1) + 1)}/{lyric_n}"
                   if lyric_n and track.get("line", -1) >= 0 else "")
                + (f"　·　当前行「{cur[1]}」" if cur else ""))

    def _drain_log(self) -> None:
        """把 ctx.pcnow_q 里的日志搬进日志框 —— **只在 Qt 线程调用**。

        ★ F44：`Ctx.pcnow_log()` 会被 SMTC / pusher / CDP / 网易云解析四个线程同时
        调用。QPlainTextEdit 不是线程安全的，从非 Qt 线程 appendPlainText 属于未定义
        行为（典型症状：设备端按"下一首"，一次换曲炸出多条日志 ⇒ 随机闪退）。
        因此工作线程只入队，落笔统一在这里做 —— 与 call_page 的 _asr_q 同一套写法。
        """
        q = getattr(self.ctx, "pcnow_q", None)
        if q is None:
            return
        for _ in range(_LOG_BURST):
            try:
                line = q.get_nowait()
            except queue.Empty:
                return
            self.log.appendPlainText(line)

    def append_log(self, s: str) -> None:
        """直接写日志框 —— 仅限 Qt 线程（MainWindow 构造期补历史日志用）。
        工作线程请走 ctx.pcnow_log()。"""
        self.log.appendPlainText(s)
