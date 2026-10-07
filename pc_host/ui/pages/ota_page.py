"""固件升级页：WiFi 局域网 OTA 面板（收编 C# agent 的 --ota 能力，§6.4）。

选 bin → 解析 esp_app_desc_t → 与设备版本粗比对 → OFFER（UDP 50003）→
设备拉 http://<pc>:50005/firmware.bin（Range 续传）→ 进度/结果 → 自证版本核对。
线程纪律（F44）：OTA 会话线程只投 ctx.ota_q / ctx.ota_log()，本页 Qt 定时器排空。
"""
from __future__ import annotations

import queue
from pathlib import Path

from PySide6.QtCore import Qt, QTimer
from PySide6.QtWidgets import (QCheckBox, QFileDialog, QFormLayout, QHBoxLayout,
                               QLabel, QLineEdit, QPlainTextEdit, QProgressBar,
                               QPushButton, QSpinBox, QVBoxLayout, QWidget)

from ..widgets import make_page_header, v_card
from ...ota import FirmwareInfo, OtaSession, compare_semver

_REPO_ROOT = Path(__file__).resolve().parents[3]
_DEFAULT_FW = _REPO_ROOT / "build" / "display_audio_photo.bin"

_STATE_TEXT = {
    "wait_online": "等待设备上线…",
    "offer": "发送 OFFER / 等待设备接受…",
    "download": "设备下载中…",
    "verify": "设备重启，等待自证核对…",
    "done": "完成",
    "failed": "失败",
    "cancelled": "已中止",
}


class OtaPage(QWidget):
    def __init__(self, ctx) -> None:
        super().__init__()
        self.ctx = ctx
        self.session: OtaSession | None = None
        root = QVBoxLayout(self)
        root.setContentsMargins(24, 20, 24, 20)
        root.setSpacing(14)

        root.addWidget(make_page_header("固件升级", "WiFi 局域网 OTA · A/B 双槽轮替 + 30s 自证回滚防砖"))

        top = QHBoxLayout()
        top.setSpacing(14)

        # ---- 卡 1：固件文件 ----
        fw_card, fw_lay = v_card("固件文件（build/display_audio_photo.bin）")
        row = QHBoxLayout()
        self.path_edit = QLineEdit()
        if _DEFAULT_FW.exists():
            self.path_edit.setText(str(_DEFAULT_FW))
        self.path_edit.setPlaceholderText("app bin 路径（含 esp_app_desc_t 的固件，不是 bootloader）")
        self.path_edit.textChanged.connect(self._reparse)
        browse = QPushButton("浏览…")
        browse.clicked.connect(self._browse)
        row.addWidget(self.path_edit, 1)
        row.addWidget(browse)
        fw_lay.addLayout(row)

        form = QFormLayout()
        form.setHorizontalSpacing(16)
        form.setVerticalSpacing(6)
        self.lbl_ver = QLabel("—")
        self.lbl_proj = QLabel("—")
        self.lbl_size = QLabel("—")
        self.lbl_sha = QLabel("—")
        self.lbl_sha.setStyleSheet("font-family: monospace;")
        for label, w in (("固件版本", self.lbl_ver), ("工程 / 构建", self.lbl_proj),
                         ("大小", self.lbl_size), ("SHA256", self.lbl_sha)):
            form.addRow(QLabel(label), w)
        fw_lay.addLayout(form)
        top.addWidget(fw_card, 1)

        # ---- 卡 2：升级控制 ----
        ctl_card, ctl_lay = v_card("升级控制")
        dev_form = QFormLayout()
        dev_form.setHorizontalSpacing(16)
        dev_form.setVerticalSpacing(6)
        self.lbl_dev_ver = QLabel("—")
        self.lbl_cmp = QLabel("—")
        for label, w in (("设备当前版本", self.lbl_dev_ver), ("比对结论", self.lbl_cmp)):
            dev_form.addRow(QLabel(label), w)
        ctl_lay.addLayout(dev_form)

        adv = QHBoxLayout()
        adv.setSpacing(14)
        self.chk_force = QCheckBox("忽略版本比对（--force）")
        self.chk_force.setToolTip("同版本/降级也照发（联调用）")
        self.chk_corrupt = QCheckBox("注入 1bit 损坏（验收 F）")
        self.chk_corrupt.setToolTip("固件流中点翻一个 bit，验证设备端 esp_ota_end/段哈希拒绝")
        spin_lbl = QLabel("只发前")
        self.spin_limit = QSpinBox()
        self.spin_limit.setRange(0, 99)
        self.spin_limit.setValue(0)
        self.spin_limit.setSuffix(" %")
        self.spin_limit.setToolTip("0=全量；N=只发前 N%（触发 size mismatch，验证续传，验收 E）")
        adv.addWidget(self.chk_force)
        adv.addWidget(self.chk_corrupt)
        adv.addWidget(spin_lbl)
        adv.addWidget(self.spin_limit)
        adv.addStretch(1)
        ctl_lay.addLayout(adv)

        btn_row = QHBoxLayout()
        self.btn_start = QPushButton("开始升级")
        self.btn_start.setObjectName("Primary")
        self.btn_start.clicked.connect(self._start)
        self.btn_abort = QPushButton("中止")
        self.btn_abort.setEnabled(False)
        self.btn_abort.clicked.connect(self._abort)
        btn_row.addWidget(self.btn_start)
        btn_row.addWidget(self.btn_abort)
        btn_row.addStretch(1)
        ctl_lay.addLayout(btn_row)

        self.bar = QProgressBar()
        self.bar.setRange(0, 100)
        self.bar.setValue(0)
        self.bar.setFormat("待机")
        ctl_lay.addWidget(self.bar)
        self.lbl_state = QLabel("待机")
        self.lbl_state.setObjectName("Secondary")
        ctl_lay.addWidget(self.lbl_state)
        top.addWidget(ctl_card, 1)

        root.addLayout(top, 1)

        # ---- 卡 3：日志 ----
        log_card, log_lay = v_card("OTA 日志")
        self.log_view = QPlainTextEdit()
        self.log_view.setReadOnly(True)
        self.log_view.setMaximumBlockCount(2000)
        log_lay.addWidget(self.log_view)
        root.addWidget(log_card, 1)

        self._timer = QTimer(self)
        self._timer.timeout.connect(self._poll)
        self._timer.start(150)

    # ---- 文件选择与解析（Qt 线程内同步做，几 MB 读取足够快） ----
    def _browse(self) -> None:
        start = str(_DEFAULT_FW.parent) if _DEFAULT_FW.parent.exists() else str(Path.home())
        path, _ = QFileDialog.getOpenFileName(self, "选择固件 bin", start, "固件 (*.bin);;所有文件 (*)")
        if path:
            self.path_edit.setText(path)
            # ★ 同路径重选（重编译后 build/display_audio_photo.bin 版本已变）
            #   不会触发 textChanged → 显式重解析，否则版本/SHA256 一直显示旧值
            self._reparse()

    def _reparse(self) -> None:
        fw = FirmwareInfo.parse(self.path_edit.text().strip()) if self.path_edit.text().strip() else None
        self._fw = fw
        if fw is None:
            self.lbl_ver.setText("—")
            self.lbl_proj.setText("—")
            self.lbl_size.setText("—")
            self.lbl_sha.setText("—")
        else:
            self.lbl_ver.setText(fw.version)
            self.lbl_proj.setText(f"{fw.project_name} · {fw.build_time}")
            self.lbl_size.setText(f"{fw.size} B ({fw.size / 1048576:.2f} MB)")
            self.lbl_sha.setText(fw.sha256[:16] + "…")
        self._update_cmp()

    def _update_cmp(self) -> None:
        fw = getattr(self, "_fw", None)
        link = self.ctx.link
        dev_ver = str((link.info or {}).get("fw_ver", "") or "") if link else ""
        self.lbl_dev_ver.setText(dev_ver or "（离线 / 未上报）")
        if fw is None:
            self.lbl_cmp.setText("先选择有效固件")
        elif not dev_ver:
            self.lbl_cmp.setText("设备未上报版本，OFFER 时跳过比对")
        else:
            c = compare_semver(dev_ver, fw.version)
            if c < 0:
                self.lbl_cmp.setText(f"可升级  {dev_ver} → {fw.version}")
            elif self.chk_force.isChecked():
                self.lbl_cmp.setText(f"版本未提升（{dev_ver} ≥ {fw.version}），--force 已勾选可强刷")
            else:
                self.lbl_cmp.setText(f"版本未提升（{dev_ver} ≥ {fw.version}），需勾选「忽略版本比对」")
        self.btn_start.setEnabled(fw is not None and self.session is None
                                  and bool(self.ctx.link and self.ctx.link.online))

    # ---- 会话控制 ----
    def _start(self) -> None:
        fw = getattr(self, "_fw", None)
        if fw is None or self.session is not None or not self.ctx.link.online:
            return
        session = OtaSession(self.ctx, fw,
                             limit_percent=self.spin_limit.value(),
                             corrupt=self.chk_corrupt.isChecked(),
                             force=self.chk_force.isChecked())
        self.session = session
        self.ctx.ota_session = session          # link 分发器据此喂 OTA_* 回包
        session.start()
        self._set_running(True)
        self.bar.setValue(0)
        self.bar.setFormat("0%")
        self.lbl_state.setText(_STATE_TEXT["wait_online"])

    def _abort(self) -> None:
        if self.session is not None:
            self.session.stop("user_cancel")

    def _set_running(self, running: bool) -> None:
        self.btn_start.setEnabled(not running)
        self.btn_abort.setEnabled(running)
        self.path_edit.setEnabled(not running)
        for w in (self.chk_force, self.chk_corrupt, self.spin_limit):
            w.setEnabled(not running)

    # ---- 150ms 轮询：排空事件队列 + 设备状态 ----
    def _poll(self) -> None:
        try:
            while True:
                ev = self.ctx.ota_q.get_nowait()
                kind = ev.get("ev")
                if kind == "log":
                    self.log_view.appendPlainText(str(ev.get("line", "")))
                elif kind == "state":
                    self.lbl_state.setText(_STATE_TEXT.get(str(ev.get("state")), str(ev.get("state"))))
                elif kind == "progress":
                    pct = int(ev.get("percent", -1))
                    if pct >= 0:
                        self.bar.setValue(pct)
                        self.bar.setFormat(f"{pct}%   {ev.get('speed', 0)} KB/s")
                elif kind == "result":
                    ok = ev.get("ok")
                    detail = str(ev.get("detail", ""))
                    self.log_view.appendPlainText(f"===== 结果: {detail} =====")
                    self.bar.setFormat(detail)
                    if ok is True:
                        self.bar.setValue(100)
                    self._teardown_session()
        except queue.Empty:
            pass
        # 设备版本/比对结论低频刷新
        self._cmp_tick = getattr(self, "_cmp_tick", 0) + 1
        if self._cmp_tick >= 7:                 # ~1s
            self._cmp_tick = 0
            self._update_cmp()

    def _teardown_session(self) -> None:
        self.session = None
        self.ctx.ota_session = None
        self._set_running(False)
        self._update_cmp()
