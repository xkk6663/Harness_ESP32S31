"""Minimal PySide6 desktop shell; network services remain independently testable."""
from __future__ import annotations
import argparse
import queue
from pathlib import Path
from .net.link import NetLink
from .net.voice_udp import UdpVoice
from .record.server import RecordingServer
from .audio.player import Player
from .audio.capture import Capture
from .asr.backend import AsrBackend

def main() -> int:
    parser = argparse.ArgumentParser(); parser.add_argument("--device", required=True); parser.add_argument("--recordings", default="recordings"); args = parser.parse_args()
    try:
        from PySide6.QtCore import QTimer
        from PySide6.QtWidgets import QApplication, QLabel, QPushButton, QPlainTextEdit, QVBoxLayout, QWidget
    except ImportError as exc:
        raise SystemExit("Install desktop extras: pip install -e .[desktop]") from exc
    pcm_q: queue.Queue[bytes] = queue.Queue(maxsize=12)
    server = RecordingServer(Path(args.recordings)); server.start()
    link = NetLink(args.device); link.start()
    player = Player()
    asr = AsrBackend(lambda text: None)
    rx_level = {"rms": 0, "count": 0}
    import time as _t; _dbg = {"last": 0.0}
    def on_pcm(pcm: bytes) -> None:
        player.push(pcm); asr.push(pcm)
        rx_level["count"] += 1
        import array
        samples = array.array("h", pcm)
        n = len(samples)
        if n:
            s = sum(x * x for x in samples) / n
            rx_level["rms"] = min(100, int((s ** 0.5) / 32768.0 * 1500))
        now = _t.monotonic()
        if now - _dbg["last"] > 2:
            print(f"[on_pcm] len={len(pcm)} head={pcm[:8].hex()} rms_raw={int((s**0.5) if n else 0)} level={rx_level['rms']} count={rx_level['count']}", flush=True)
            _dbg["last"] = now
        if not pcm_q.full(): pcm_q.put_nowait(pcm)
    voice = UdpVoice(args.device, on_pcm); voice.start()
    try:
        player.start(); capture = Capture(voice.send_ptt); capture.start(); asr.start()
    except ImportError:
        capture = None
    app = QApplication([]); window = QWidget(); layout = QVBoxLayout(window)
    from PySide6.QtWidgets import QProgressBar
    state = QLabel("Connecting...")
    tx_meter = QProgressBar(); tx_meter.setRange(0, 100); tx_meter.setFormat("UP (dev mic): %v%  rx=%i")
    rx_meter = QProgressBar(); rx_meter.setRange(0, 100); rx_meter.setFormat("DOWN (local mic): %v%")
    transcript = QPlainTextEdit(); transcript.setReadOnly(True); ptt = QPushButton("Hold to talk")
    ptt.pressed.connect(lambda: (setattr(capture, "ptt", True) if capture else None, link.send({"type":"PTT_DOWN","ver":1})))
    ptt.released.connect(lambda: (setattr(capture, "ptt", False) if capture else None, link.send({"type":"PTT_UP","ver":1})))
    layout.addWidget(state); layout.addWidget(tx_meter); layout.addWidget(rx_meter); layout.addWidget(transcript); layout.addWidget(ptt)
    def poll():
        state.setText("Connected" if link.online else "No device heartbeat")
        tx_meter.setValue(rx_level["rms"])
        tx_meter.setFormat(f"UP (dev mic): {rx_level['rms']}%  frames={rx_level['count']}")
        rx_meter.setValue(getattr(capture, "_rms", 0) if capture else 0)
        rx_level["rms"] = 0
    timer = QTimer(); timer.timeout.connect(poll); timer.start(100)
    window.setWindowTitle("ESP32 Voice Host"); window.resize(620, 420); window.show()
    result = app.exec()
    if capture: capture.close()
    player.close(); asr.close(); voice.close(); link.close(); server.close(); return result
if __name__ == "__main__": raise SystemExit(main())
