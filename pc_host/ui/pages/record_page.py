"""录音库页：设备端录音经 TCP 50002 上传后的本地库（sqlite 索引）。"""
from __future__ import annotations

import os
from datetime import datetime

from PySide6.QtCore import QTimer
from PySide6.QtWidgets import (QHBoxLayout, QHeaderView, QLabel, QPushButton,
                               QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget)

from ..widgets import make_page_header, v_card


def _fmt_size(n: int) -> str:
    return f"{n / 1024:.1f} KB" if n < 1024 * 1024 else f"{n / 1024 / 1024:.2f} MB"


class RecordPage(QWidget):
    def __init__(self, ctx) -> None:
        super().__init__()
        self.ctx = ctx
        root = QVBoxLayout(self)
        root.setContentsMargins(24, 20, 24, 20)
        root.setSpacing(14)

        root.addWidget(make_page_header("录音库", "设备端录音自动上传 · TCP 50002 分块+CRC16 · WAV 16k/16bit/mono"))

        card_, lay = v_card()
        self.table = QTableWidget(0, 4)
        self.table.setHorizontalHeaderLabels(["文件", "时长", "大小", "录制时间"])
        self.table.horizontalHeader().setSectionResizeMode(0, QHeaderView.Stretch)
        self.table.setColumnWidth(1, 90)
        self.table.setColumnWidth(2, 110)
        self.table.setColumnWidth(3, 170)
        self.table.verticalHeader().setVisible(False)
        self.table.setEditTriggers(QTableWidget.NoEditTriggers)
        self.table.setSelectionBehavior(QTableWidget.SelectRows)
        lay.addWidget(self.table, 1)

        row = QHBoxLayout()
        self.refresh_btn = QPushButton("刷新")
        self.open_btn = QPushButton("打开目录")
        self.del_btn = QPushButton("删除选中")
        self.del_btn.setObjectName("Danger")
        self.hint = QLabel("")
        self.hint.setObjectName("Secondary")
        row.addWidget(self.refresh_btn)
        row.addWidget(self.open_btn)
        row.addWidget(self.del_btn)
        row.addStretch(1)
        row.addWidget(self.hint)
        lay.addLayout(row)
        root.addWidget(card_, 1)

        self.refresh_btn.clicked.connect(self.refresh)
        self.open_btn.clicked.connect(self._open_dir)
        self.del_btn.clicked.connect(self._delete)

        self._timer = QTimer(self)
        self._timer.timeout.connect(self.refresh)
        self._timer.start(5000)
        self.refresh()

    def refresh(self) -> None:
        if self.ctx.record_server is None:
            self.hint.setText("录音服务未启动")
            return
        rows = self.ctx.record_server.list()
        self.table.setRowCount(len(rows))
        for i, (sid, path, size, created) in enumerate(rows):
            self.table.setItem(i, 0, QTableWidgetItem(f"{sid}.wav"))
            self.table.setItem(i, 1, QTableWidgetItem(f"{size / 32000:.1f} s"))
            self.table.setItem(i, 2, QTableWidgetItem(_fmt_size(size)))
            self.table.setItem(i, 3, QTableWidgetItem(
                datetime.fromtimestamp(created).strftime("%m-%d %H:%M:%S")))
        self.hint.setText(f"共 {len(rows)} 条 · 目录 {self.ctx.record_server.root}")

    def _open_dir(self) -> None:
        if self.ctx.record_server is not None:
            os.startfile(str(self.ctx.record_server.root))  # type: ignore[attr-defined]

    def _delete(self) -> None:
        if self.ctx.record_server is None:
            return
        row = self.table.currentRow()
        if row < 0:
            return
        sid = self.table.item(row, 0).text()[:-len(".wav")]
        self.ctx.record_server.delete(sid)
        print(f"[rec] deleted {sid}", flush=True)
        self.refresh()
