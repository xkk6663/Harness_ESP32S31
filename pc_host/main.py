"""PySide6 desktop shell: TX=dev mic->PC, RX=PC wav->dev speaker."""
from __future__ import annotations
import argparse
import queue
from pathlib import Path
from .net.link import NetLink
from .net.voice_udp import UdpVoice
from .record.server import RecordingServer
from .audio.player import Player
from .audio.wav_player import WavPlayer
from .asr.backend import AsrBackend

def main() -> int:
    parser = argparse.ArgumentParser(); parser.add_argument("--device", required=True); parser.add_argument("--recordings", default="recordings"); args = parser.parse_args()
    try:
        from PySide6.QtCore import QTimer
        from PySide6.QtWidgets import (QApplication, QLabel, QPushButton, QPlainTextEdit,
            QVBoxLayout, QWidget, QListWidget, QHBoxLayout)
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
        s = sum(x*x for x in samples)/n if n else 0
        rx_level["rms"] = min(100, int((s**0.5)/32768.0*1500))
        now = _t.monotonic()
        if now - _dbg["last"] > 2:
            print(f"[on_pcm] len={len(pcm)} rms={int(s**0.5)} count={rx_level['count']}", flush=True)
            _dbg["last"] = now
        if not pcm_q.full(): pcm_q.put_nowait(pcm)
    voice = UdpVoice(args.device, on_pcm); voice.start()
    wav = WavPlayer(voice.send_ptt)
    player.start(); asr.start()

    app = QApplication([]); window = QWidget(); layout = QVBoxLayout(window)
    state = QLabel("Connecting...")
    tx_meter = QProgressBar() if False else None
    from PySide6.QtWidgets import QProgressBar
    tx_meter = QProgressBar(); tx_meter.setRange(0, 100)
    rx_meter = QProgressBar(); rx_meter.setRange(0, 100)
    transcript = QPlainTextEdit(); transcript.setReadOnly(True)

    # wav file list
    wav_label = QLabel("WAV files (select one for RX playback):")
    wav_list = QListWidget(); wav_list.setMaximumHeight(120)
    rec_dir = Path(args.recordings)
    for w in sorted(rec_dir.glob("*.wav")):
        wav_list.addItem(w.name)
    play_btn = QPushButton("Play selected wav to device (RX)")
    stop_btn = QPushButton("Stop playback")
    del_btn = QPushButton("Delete selected")

    layout.addWidget(state)
    layout.addWidget(QLabel("UP (dev mic):"))
    layout.addWidget(tx_meter)
    layout.addWidget(QLabel("DOWN (wav->dev spk):"))
    layout.addWidget(rx_meter)
    layout.addWidget(wav_label); layout.addWidget(wav_list)
    row = QHBoxLayout(); row.addWidget(play_btn); row.addWidget(stop_btn); row.addWidget(del_btn)
    layout.addLayout(row)
    layout.addWidget(transcript)

    def refresh_list():
        wav_list.clear()
        for w in sorted(rec_dir.glob("*.wav")):
            wav_list.addItem(w.name)
    def do_play():
        item = wav_list.currentItem()
        if not item: return
        wav.stop()
        voice.reset_seq()
        if wav.load(rec_dir / item.text()):
            wav.start()
    def do_stop():
        wav.stop()
    def do_del():
        item = wav_list.currentItem()
        if not item: return
        wav.stop()
        try:
            (rec_dir / item.text()).unlink()
            print(f"[del] {item.text()}", flush=True)
        except Exception as e:
            print(f"[del] error: {e}", flush=True)
        refresh_list()
    play_btn.clicked.connect(do_play)
    stop_btn.clicked.connect(do_stop)
    del_btn.clicked.connect(do_del)

    def poll():
        state.setText("Connected" if link.online else "No device heartbeat")
        tx_meter.setValue(rx_level["rms"])
        tx_meter.setFormat(f"UP (dev mic): {rx_level['rms']}%  frames={rx_level['count']}")
        rx_meter.setValue(wav._rms)
        rx_meter.setFormat(f"DOWN (wav): {wav._rms}%")
        rx_level["rms"] = 0
    timer = QTimer(); timer.timeout.connect(poll); timer.start(100)
    window.setWindowTitle("ESP32 Voice Host"); window.resize(620, 480); window.show()
    result = app.exec()
    wav.close(); player.close(); asr.close(); voice.close(); link.close(); server.close(); return result

if __name__ == "__main__": raise SystemExit(main())
