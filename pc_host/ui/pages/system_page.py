"""系统页：设备信息 / 链路 / PC 环境 / OTA 预留。"""
from __future__ import annotations

import importlib.util
import socket
import sys

from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import (QFormLayout, QHBoxLayout, QLabel, QLineEdit,
                               QPushButton, QVBoxLayout, QWidget)

from ..widgets import make_page_header, status_chip, v_card


def _local_ip() -> str:
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return "127.0.0.1"


def _have(mod: str) -> str:
    return "√" if importlib.util.find_spec(mod) else "× 未安装"


class SystemPage(QWidget):
    def __init__(self, ctx) -> None:
        super().__init__()
        self.ctx = ctx
        root = QVBoxLayout(self)
        root.setContentsMargins(24, 20, 24, 20)
        root.setSpacing(14)

        root.addWidget(make_page_header("系统", "设备与上位机运行状态"))

        cols = QHBoxLayout()
        cols.setSpacing(14)

        # 设备
        dev, dev_lay = v_card("设备（ESP32-S31 Korvo-1）")
        form = QFormLayout()
        form.setLabelAlignment(Qt.AlignRight)
        form.setHorizontalSpacing(16)
        form.setVerticalSpacing(8)
        self.fw = QLabel("—")
        self.proto = QLabel("—")
        self.sr = QLabel("—")
        self.dev_ip = QLabel("—")
        for label, w in (("固件版本", self.fw), ("协议版本", self.proto),
                         ("采样率", self.sr), ("设备 IP", self.dev_ip)):
            form.addRow(QLabel(label), w)
        dev_lay.addLayout(form)
        lay, self._dot, self._link_text = status_chip()
        dev_lay.addLayout(lay)
        # 手动指定 IP（自动发现失败时的兜底，立即生效）
        ip_row = QHBoxLayout()
        self.ip_edit = QLineEdit()
        self.ip_edit.setPlaceholderText("自动发现失败？手动填设备 IP，如 192.168.4.1")
        ip_btn = QPushButton("应用")
        ip_btn.clicked.connect(self._apply_ip)
        ip_row.addWidget(self.ip_edit, 1)
        ip_row.addWidget(ip_btn)
        dev_lay.addLayout(ip_row)
        cols.addWidget(dev, 1)

        # 上位机
        pc, pc_lay = v_card("上位机（pc_host）")
        pform = QFormLayout()
        pform.setHorizontalSpacing(16)
        pform.setVerticalSpacing(8)
        self.pc_ip = QLabel(_local_ip())
        for label, w in (("本机 IP", self.pc_ip),
                         ("Python", QLabel(sys.version.split()[0])),
                         ("SMTC(winsdk)", QLabel(_have("winsdk"))),
                         ("音频(sounddevice)", QLabel(_have("sounddevice"))),
                         ("回环采集(pyaudiowpatch)", QLabel(_have("pyaudiowpatch"))),
                         ("ASR(sherpa_onnx)", QLabel(_have("sherpa_onnx")))):
            pform.addRow(QLabel(label), w)
        pc_lay.addLayout(pform)
        cols.addWidget(pc, 1)
        root.addLayout(cols, 1)

        # 端口总表 + OTA 预留
        bottom = QHBoxLayout()
        bottom.setSpacing(14)
        ports, ports_lay = v_card("端口占用")
        ports_lay.addWidget(QLabel(
            "UDP 50000  设备麦克风 → PC（语音）\n"
            "UDP 50001  PC → 设备扬声器（PTT/WAV）\n"
            "TCP 50002  设备录音上传\n"
            "UDP 50003  心跳/信令（双向）\n"
            "UDP 50004  副屏 NOW_* 推送（PC→设备）\n"
            "TCP 50005  副屏封面/歌词 HTTP（设备拉取）"))
        bottom.addWidget(ports, 1)

        ota, ota_lay = v_card("OTA 升级（规划中）")
        ota_lay.addWidget(QLabel(
            "A/B 双区 + 回滚防砖方案已定稿\n"
            "（《WiFi局域网OTA升级实施方案》M1）。\n"
            "落地后此处提供拖拽固件升级。"))
        self.ota_btn = QPushButton("等待实施")
        self.ota_btn.setEnabled(False)
        ota_lay.addWidget(self.ota_btn)
        ota_lay.addStretch(1)
        bottom.addWidget(ota, 1)
        root.addLayout(bottom, 1)

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._poll)
        self._timer.start(1000)

    def _apply_ip(self) -> None:
        ip = self.ip_edit.text().strip()
        link = self.ctx.link
        if link:
            link.set_device_ip(ip or None)
            print(f"[link] manual device ip -> {ip or '(auto-discover)'}", flush=True)

    def _poll(self) -> None:
        link = self.ctx.link
        info = link.info or {}
        if link.online:
            self._dot.setObjectName("StatusDotOn")
            self._link_text.setText("在线")
            self.fw.setText(str(info.get("fw_ver", "—")))
            self.proto.setText(str(info.get("proto_ver", "—")))
            self.sr.setText(f"{info.get('sample_rate', '—')} Hz")
            self.dev_ip.setText(str(link.device_ip or "—"))
        else:
            self._dot.setObjectName("StatusDotOff")
            mode = "（发现中：广播+网段扫描）" if link.device_ip is None else ""
            self._link_text.setText(f"离线{mode}")
            self.fw.setText("—")
            self.proto.setText("—")
            self.sr.setText("—")
            self.dev_ip.setText(str(link.device_ip or "—"))
            self.fw.setText("—")
            self.proto.setText("—")
            self.sr.setText("—")
            self.dev_ip.setText(str(link.device_ip or "—"))
        self._dot.style().unpolish(self._dot)
        self._dot.style().polish(self._dot)
