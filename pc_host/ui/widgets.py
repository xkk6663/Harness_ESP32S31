"""公共部件：卡片、页头、设备状态徽标、电平条。"""
from __future__ import annotations

from PySide6.QtCore import Qt
from PySide6.QtWidgets import QFrame, QHBoxLayout, QLabel, QProgressBar, QVBoxLayout


def card(parent=None) -> QFrame:
    f = QFrame(parent)
    f.setObjectName("Card")
    return f


def v_card(title: str | None = None, parent=None) -> tuple[QFrame, QVBoxLayout]:
    """带内边距的竖排卡片，可选标题。"""
    f = card(parent)
    lay = QVBoxLayout(f)
    lay.setContentsMargins(16, 14, 16, 14)
    lay.setSpacing(10)
    if title:
        t = QLabel(title)
        t.setObjectName("H2")
        lay.addWidget(t)
    return f, lay


def make_page_header(title: str, subtitle: str = "") -> QFrame:
    """页面统一页头：大标题 + 副标题。"""
    f = QFrame()
    lay = QVBoxLayout(f)
    lay.setContentsMargins(0, 0, 0, 4)
    lay.setSpacing(2)
    t = QLabel(title)
    t.setObjectName("PageTitle")
    lay.addWidget(t)
    if subtitle:
        s = QLabel(subtitle)
        s.setObjectName("Secondary")
        lay.addWidget(s)
    return f


def status_chip() -> tuple[QHBoxLayout, QLabel, QLabel]:
    """设备状态行：圆点 + 文本。返回 (layout, dot_label, text_label)。"""
    dot = QLabel("●")
    dot.setObjectName("StatusDotOff")
    text = QLabel("未连接")
    text.setObjectName("Secondary")
    lay = QHBoxLayout()
    lay.setContentsMargins(0, 0, 0, 0)
    lay.setSpacing(6)
    lay.addWidget(dot)
    lay.addWidget(text)
    lay.addStretch(1)
    return lay, dot, text


def meter(format_tpl: str) -> QProgressBar:
    bar = QProgressBar()
    bar.setRange(0, 100)
    bar.setValue(0)
    bar._fmt = format_tpl  # type: ignore[attr-defined]
    return bar


def update_meter(bar: QProgressBar, value: int, extra: str = "") -> None:
    bar.setValue(max(0, min(100, value)))
    fmt = getattr(bar, "_fmt", "")
    bar.setFormat(fmt.format(v=max(0, min(100, value))) + (f"   {extra}" if extra else ""))


def hline() -> QFrame:
    line = QFrame()
    line.setFrameShape(QFrame.HLine)
    line.setStyleSheet("color:#273041; background:#273041; max-height:1px; border:none;")
    return line
