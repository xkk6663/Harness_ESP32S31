"""壁纸管理页（§76）：左侧大图预览 + 右侧上传控制，替代系统页旧卡片。

数据面复用 pc_host/wallpaper.py（设备拉模型，与 OTA 同构）：
选图 → 本地预校验（≤2MB + 800x480 整屏）→ OFFER×3 → 设备 HTTP GET 拉取
→ WP_RESULT。取消 = sess.abort()（线程收场，设备侧无会话则自然超时忽略）。

★ F44 纪律：wp_q 由工作线程投递，本页 QTimer 在 Qt 线程排空，绝不跨线程碰控件。
"""
from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QPixmap
from PySide6.QtWidgets import (QFileDialog, QFrame, QHBoxLayout, QLabel,
                               QPlainTextEdit, QPushButton, QVBoxLayout,
                               QWidget)

from ..widgets import make_page_header, v_card

_PREVIEW_W, _PREVIEW_H = 520, 312     # 800x480 同比例（×0.65），留卡片内边距

_C_DIM = "color: #94A3B8;"
_C_OK = "color: #34D399;"
_C_ERR = "color: #F87171;"
_C_RUN = "color: #22D3EE;"


class WallpaperPage(QWidget):
    def __init__(self, ctx) -> None:
        super().__init__()
        self.ctx = ctx
        self.wp_path: Path | None = None
        self._running = False
        self._t0 = 0.0                     # 上传开始时刻（算已耗时）

        root = QVBoxLayout(self)
        root.setContentsMargins(24, 20, 24, 20)
        root.setSpacing(14)
        root.addWidget(make_page_header("壁纸管理", "SD 卡壁纸池 · 设备拉取上传"))

        cols = QHBoxLayout()
        cols.setSpacing(14)

        # ---- 左：预览 ----
        prev_card, prev_lay = v_card("预览（800x480，与设备屏幕同比例）")
        frame = QFrame()
        frame.setObjectName("Card")        # 深底托图
        frame.setFixedSize(_PREVIEW_W + 16, _PREVIEW_H + 16)
        fl = QVBoxLayout(frame)
        fl.setContentsMargins(8, 8, 8, 8)
        self.preview = QLabel("选一张 800x480 JPEG\n此处显示效果预览")
        self.preview.setAlignment(Qt.AlignCenter)
        self.preview.setFixedSize(_PREVIEW_W, _PREVIEW_H)
        self.preview.setStyleSheet("background: #10141B; color: #64748B;")
        fl.addWidget(self.preview)
        prev_lay.addWidget(frame, 0, Qt.AlignHCenter)
        self.dim_label = QLabel("—")
        self.dim_label.setStyleSheet(_C_DIM)
        prev_lay.addWidget(self.dim_label)
        prev_lay.addStretch(1)
        cols.addWidget(prev_card, 3)

        # ---- 右：上传控制 ----
        up_card, up_lay = v_card("上传到设备")
        up_lay.addWidget(QLabel(
            "本地预校验（≤2MB + 800x480 整屏）通过后，设备经局域网拉取 →\n"
            "落池 /sdcard/wallpaper → 自动设为当前壁纸并 crossfade。"))
        self.file_label = QLabel("未选择文件")
        self.file_label.setStyleSheet(_C_DIM)
        self.file_label.setWordWrap(True)
        up_lay.addWidget(self.file_label)

        row = QHBoxLayout()
        self.pick_btn = QPushButton("选图…")
        self.pick_btn.clicked.connect(self._pick)
        self.send_btn = QPushButton("上传到设备")
        self.send_btn.setEnabled(False)
        self.send_btn.clicked.connect(self._send)
        self.cancel_btn = QPushButton("取消上传")
        self.cancel_btn.setEnabled(False)
        self.cancel_btn.clicked.connect(self._cancel)
        row.addWidget(self.pick_btn)
        row.addWidget(self.send_btn, 1)
        row.addWidget(self.cancel_btn)
        up_lay.addLayout(row)

        self.status = QLabel("空闲")
        self.status.setWordWrap(True)
        up_lay.addWidget(self.status)

        up_lay.addWidget(QLabel("上传日志"))
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumHeight(150)
        self.log.setPlaceholderText("会话事件（OFFER / 拉取 / 结果）…")
        up_lay.addWidget(self.log, 1)
        cols.addWidget(up_card, 2)

        root.addLayout(cols, 1)

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._drain)
        self._timer.start(500)

    # ---- 选图 + 预校验 + 可视化 ----
    def _pick(self) -> None:
        path, _ = QFileDialog.getOpenFileName(
            self, "选择壁纸（800x480 JPEG）", str(Path.home()),
            "图片 (*.jpg *.jpeg)")
        if not path:
            return
        self.wp_path = Path(path)
        self.file_label.setText(str(self.wp_path))
        self.file_label.setStyleSheet("color: #E2E8F0;")
        self._show_preview(self.wp_path)
        from ...wallpaper import precheck
        chk = precheck(self.wp_path)
        if isinstance(chk, str):
            self.dim_label.setText(f"✗ {chk}")
            self.dim_label.setStyleSheet(_C_ERR)
            self.send_btn.setEnabled(False)
            self.status.setText("预校验未通过")
            self.status.setStyleSheet(_C_ERR)
        else:
            w, h, size = chk
            self.dim_label.setText(
                f"✓ {w}x{h} · {size} B（{size / 1024:.0f} KB）· 可上传")
            self.dim_label.setStyleSheet(_C_OK)
            self.send_btn.setEnabled(not self._running)
            self.status.setText("预校验通过，可上传")
            self.status.setStyleSheet(_C_OK)

    def _show_preview(self, path: Path) -> None:
        pm = QPixmap(str(path))
        if pm.isNull():
            self.preview.setText("图片加载失败")
            self.preview.setStyleSheet("background: #10141B; color: #F87171;")
            return
        self.preview.setPixmap(pm.scaled(
            _PREVIEW_W, _PREVIEW_H, Qt.KeepAspectRatio, Qt.SmoothTransformation))
        self.preview.setStyleSheet("background: #10141B;")

    # ---- 上传 / 取消 ----
    def _send(self) -> None:
        if self.wp_path is None:
            return
        sess = self.ctx.wp_session
        if sess is None:
            self._set_status("✗ 壁纸会话不可用（服务未启动）", _C_ERR)
            return
        import time
        err = sess.start(self.wp_path)
        if err is not None:
            self._set_status(f"✗ {err}", _C_ERR)
            return
        self._running = True
        self._t0 = time.monotonic()
        self._sync_buttons()
        self._set_status("上传中… OFFER×3（每 8s 一轮），等设备拉取并回 WP_RESULT", _C_RUN)

    def _cancel(self) -> None:
        sess = self.ctx.wp_session
        if sess is not None:
            sess.abort()
            self._set_status("已请求取消，等会话线程收场…", _C_RUN)

    # ---- wp_q 排空（Qt 线程） ----
    def _drain(self) -> None:
        if self._running:
            import time
            self.status.setText(
                f"上传中… 已等待 {time.monotonic() - self._t0:.0f}s "
                "（OFFER×3 / 设备 HTTP 拉取 / WP_RESULT）")
        q = self.ctx.wp_q
        while q is not None:
            try:
                ev = q.get_nowait()
            except Exception:                 # queue.Empty
                break
            if ev.get("ev") == "log":
                self.log.appendPlainText(str(ev.get("line", "")))
            elif ev.get("ev") == "result":
                self._running = False
                self._sync_buttons()
                ok = bool(ev.get("ok"))
                self._set_status(("✓ " if ok else "✗ ") + str(ev.get("line", "")),
                                 _C_OK if ok else _C_ERR)

    def _sync_buttons(self) -> None:
        self.send_btn.setEnabled(
            not self._running and self.wp_path is not None
            and self.dim_label.text().startswith("✓"))
        self.cancel_btn.setEnabled(self._running)

    def _set_status(self, text: str, color: str) -> None:
        self.status.setText(text)
        self.status.setStyleSheet(color)
