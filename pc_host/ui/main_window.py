"""主窗口：左侧导航 + 右侧页面栈，统管全部设备功能。"""
from __future__ import annotations

from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import (QFrame, QHBoxLayout, QLabel, QMainWindow,
                               QPushButton, QStackedWidget, QVBoxLayout, QWidget)

from .pages.call_page import CallPage
from .pages.ota_page import OtaPage
from .pages.pcnow_page import PcNowPage
from .pages.record_page import RecordPage
from .pages.system_page import SystemPage
from .pages.wallpaper_page import WallpaperPage
from .widgets import status_chip


class MainWindow(QMainWindow):
    def __init__(self, ctx) -> None:
        super().__init__()
        self.ctx = ctx
        self.setWindowTitle("Korvo 上位机 · ESP32-S31")
        self.resize(1020, 660)

        root = QWidget()
        root.setObjectName("Root")
        self.setCentralWidget(root)
        lay = QHBoxLayout(root)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(0)

        # ---- 侧边栏 ----
        side = QFrame()
        side.setObjectName("Sidebar")
        side.setFixedWidth(216)
        side_lay = QVBoxLayout(side)
        side_lay.setContentsMargins(16, 22, 16, 16)
        side_lay.setSpacing(6)

        title = QLabel("Korvo 上位机")
        title.setObjectName("AppTitle")
        sub = QLabel("ESP32-S31 · Korvo-1")
        sub.setObjectName("AppSub")
        side_lay.addWidget(title)
        side_lay.addWidget(sub)
        side_lay.addSpacing(18)

        lay2, self._dot, self._link_text = status_chip()
        side_lay.addLayout(lay2)
        side_lay.addSpacing(14)

        self._pages = [
            ("通话对讲", "◉", CallPage),
            ("录音库", "▶", RecordPage),
            ("音乐副屏", "♫", PcNowPage),
            ("固件升级", "⇪", OtaPage),
            ("壁纸管理", "▣", WallpaperPage),
            ("系统", "◆", SystemPage),
        ]
        self._navs: list[QPushButton] = []
        self.stack = QStackedWidget()
        for name, icon, cls in self._pages:
            btn = QPushButton(f" {icon}  {name}")
            btn.setObjectName("Nav")
            btn.setCheckable(True)
            btn.setCursor(Qt.PointingHandCursor)
            btn.clicked.connect(lambda _=False, i=len(self._navs): self.switch(i))
            side_lay.addWidget(btn)
            self._navs.append(btn)
            self.stack.addWidget(cls(ctx))

        side_lay.addStretch(1)
        ver = QLabel("pc_host 0.4.3 · ICD v1.5")
        ver.setObjectName("AppSub")
        side_lay.addWidget(ver)

        lay.addWidget(side)
        lay.addWidget(self.stack, 1)

        # 页面跳转钩子（系统页"去固件升级页"按钮用）
        ctx.request_page = self.switch

        self._navs[0].setChecked(True)
        self.stack.setCurrentIndex(0)

        # 侧边栏状态 500ms 轮询 + 副屏日志接入（页面建立前的日志先缓存在 ctx）
        # ★ F44：日志走 ctx.pcnow_q 队列，由 PcNowPage 的定时器在 Qt 线程排空。
        #   绝不能再把页面方法当 sink 交给工作线程直接调 —— 从 SMTC/pusher/CDP/解析
        #   四个线程碰 QPlainTextEdit 是未定义行为，典型表现是"换曲那一刻闪退"。
        #   此处只是补上"页面建立前"的积压日志；构造期本就在 Qt 线程，直接写是安全的。
        pcnow_page: PcNowPage = self.stack.widget(2)
        for line in list(ctx.pcnow_log_lines):
            pcnow_page.append_log(line)
        ctx.pcnow_log_lines.clear()

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._poll_sidebar)
        self._timer.start(500)

    def switch(self, i: int) -> None:
        self.stack.setCurrentIndex(i)
        for j, b in enumerate(self._navs):
            b.setChecked(j == i)

    def _poll_sidebar(self) -> None:
        if self.ctx.link.online:
            self._dot.setObjectName("StatusDotOn")
            self._link_text.setText(f"设备在线 · {self.ctx.link.device_ip}")
        else:
            self._dot.setObjectName("StatusDotOff")
            self._link_text.setText("设备离线 · 发现中")
        self._dot.style().unpolish(self._dot)
        self._dot.style().polish(self._dot)
