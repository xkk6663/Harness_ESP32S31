"""Low-latency speaker sink; queue overflow intentionally discards old PCM."""
from __future__ import annotations
import queue
from ..protocol import PCM_BYTES, SAMPLE_RATE

class Player:
    def __init__(self) -> None: self.queue: queue.Queue[bytes] = queue.Queue(maxsize=3); self._stream = None
    def push(self, pcm: bytes) -> None:
        if len(pcm) != PCM_BYTES: return
        while self.queue.full():
            try: self.queue.get_nowait()
            except queue.Empty: break
        self.queue.put_nowait(pcm)
    def start(self) -> None:
        import sounddevice as sd
        def callback(out, frames, _time, _status):
            try: out[:] = self.queue.get_nowait()
            except queue.Empty: out[:] = bytes(PCM_BYTES)
        self._stream = sd.RawOutputStream(samplerate=SAMPLE_RATE, channels=1, dtype="int16", blocksize=320, callback=callback); self._stream.start()
    def close(self) -> None:
        if self._stream: self._stream.stop(); self._stream.close(); self._stream = None
