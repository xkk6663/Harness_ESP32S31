"""PTT microphone source; it sends exact 20 ms PCM frames only while held."""
from __future__ import annotations
from collections.abc import Callable
from ..protocol import PCM_BYTES, SAMPLE_RATE

class Capture:
    def __init__(self, send_frame: Callable[[bytes], None]) -> None:
        self.send_frame, self.ptt, self._stream, self._rms = send_frame, False, None, 0
    def start(self) -> None:
        import sounddevice as sd
        import array
        def callback(indata, _frames, _time, _status):
            samples = array.array("h", bytes(indata))
            n = len(samples)
            self._rms = min(100, int(((sum(x * x for x in samples) / n) ** 0.5) / 32768.0 * 1500)) if n else 0
            if self.ptt:
                data = bytes(indata)
                if len(data) == PCM_BYTES: self.send_frame(data)
        self._stream = sd.RawInputStream(samplerate=SAMPLE_RATE, channels=1, dtype="int16", blocksize=320, callback=callback); self._stream.start()
    def close(self) -> None:
        if self._stream: self._stream.stop(); self._stream.close(); self._stream = None
