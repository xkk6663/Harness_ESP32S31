"""通话对讲页：设备麦克风上 行（TX）+ PC WAV 推流下 行（RX）+ ASR 转写。

对讲会话本身由设备端按键发起（SESSION_START/STOP 走 50003 信令），
PC 端常驻收听；此页负责状态显示、WAV 推流到设备扬声器与转写文本。
"""
from __future__ import annotations

import queue

from PySide6.QtCore import QTimer
from PySide6.QtWidgets import (QGridLayout, QHBoxLayout, QLabel, QListWidget,
                               QPlainTextEdit, QPushButton, QVBoxLayout, QWidget)

from ..widgets import make_page_header, meter, status_chip, update_meter, v_card


class CallPage(QWidget):
    def __init__(self, ctx) -> None:  # ctx: AppServices
        super().__init__()
        self.ctx = ctx
        root = QVBoxLayout(self)
        root.setContentsMargins(24, 20, 24, 20)
        root.setSpacing(14)

        root.addWidget(make_page_header("通话对讲", "设备端发起会话后自动连通 · PCM 16k/20ms · UDP 50000/50001"))

        # 链路状态
        link_card, link_lay = v_card("链路")
        lay, self._dot, self._link_text = status_chip()
        link_lay.addLayout(lay)
        self._link_detail = QLabel("")
        self._link_detail.setObjectName("Secondary")
        link_lay.addWidget(self._link_detail)
        root.addWidget(link_card)

        # 电平
        meter_card, m_lay = v_card("音频电平")
        grid = QGridLayout()
        grid.setHorizontalSpacing(12)
        grid.setVerticalSpacing(8)
        self.tx_meter = meter("TX 上行（设备麦克风）: {v}%")
        self.rx_meter = meter("RX 下行（WAV → 设备扬声器）: {v}%")
        grid.addWidget(QLabel("TX"), 0, 0)
        grid.addWidget(self.tx_meter, 0, 1)
        grid.addWidget(QLabel("RX"), 1, 0)
        grid.addWidget(self.rx_meter, 1, 1)
        m_lay.addLayout(grid)
        root.addWidget(meter_card, 1)

        # WAV 推流
        wav_card, w_lay = v_card("WAV 推流（16k/16bit/单声道）")
        self.wav_list = QListWidget()
        w_lay.addWidget(self.wav_list, 1)
        row = QHBoxLayout()
        self.play_btn = QPushButton("播放推流")
        self.play_btn.setObjectName("Primary")
        self.stop_btn = QPushButton("停止")
        self.del_btn = QPushButton("删除")
        self.del_btn.setObjectName("Danger")
        for b in (self.play_btn, self.stop_btn, self.del_btn):
            row.addWidget(b)
        row.addStretch(1)
        w_lay.addLayout(row)
        root.addWidget(wav_card, 2)

        # ASR 转写
        asr_card, a_lay = v_card("实时转写（sherpa-onnx）")
        self.transcript = QPlainTextEdit()
        self.transcript.setReadOnly(True)
        a_lay.addWidget(self.transcript, 1)
        root.addWidget(asr_card, 1)

        self._asr_q: queue.Queue = queue.Queue(maxsize=64)
        if ctx.asr is not None:
            ctx.asr.on_text = lambda text: self._asr_q.put(text)  # type: ignore[attr-defined]

        self.play_btn.clicked.connect(self._do_play)
        self.stop_btn.clicked.connect(self._do_stop)
        self.del_btn.clicked.connect(self._do_del)
        self._refresh_list()

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._poll)
        self._timer.start(120)

    # ---- UI 回调（主线程） ----
    def _refresh_list(self) -> None:
        self.wav_list.clear()
        for w in sorted(self.ctx.rec_dir.glob("*.wav")):
            self.wav_list.addItem(w.name)

    def _do_play(self) -> None:
        item = self.wav_list.currentItem()
        if not item or self.ctx.wav is None:
            return
        self.ctx.wav.stop()
        self.ctx.voice.reset_seq()
        if self.ctx.wav.load(self.ctx.rec_dir / item.text()):
            self.ctx.wav.start()

    def _do_stop(self) -> None:
        if self.ctx.wav is not None:
            self.ctx.wav.stop()

    def _do_del(self) -> None:
        item = self.wav_list.currentItem()
        if not item:
            return
        if self.ctx.wav is not None:
            self.ctx.wav.stop()
        try:
            (self.ctx.rec_dir / item.text()).unlink()
            print(f"[del] {item.text()}", flush=True)
        except OSError as e:
            print(f"[del] error: {e}", flush=True)
        self._refresh_list()

    # ---- 100ms 轮询（读线程安全状态） ----
    def _poll(self) -> None:
        link = self.ctx.link
        if link.online:
            self._dot.setObjectName("StatusDotOn")
            fw = (link.info or {}).get("fw_ver", "?")
            self._link_text.setText(f"设备已连接  {link.device_ip}")
            self._link_detail.setText(f"fw_ver={fw}  proto_ver={(link.info or {}).get('proto_ver', '?')}")
        else:
            self._dot.setObjectName("StatusDotOff")
            self._link_text.setText("等待设备心跳（同一路由器 / AP 热点）")
            self._link_detail.setText("")
        self._dot.style().unpolish(self._dot)
        self._dot.style().polish(self._dot)

        update_meter(self.tx_meter, self.ctx.rx_level["rms"],
                     f"frames={self.ctx.rx_level['count']}")
        self.ctx.rx_level["rms"] = 0
        if self.ctx.wav is not None:
            update_meter(self.rx_meter, self.ctx.wav._rms)

        while True:
            try:
                text = self._asr_q.get_nowait()
            except queue.Empty:
                break
            self.transcript.appendPlainText(text)
