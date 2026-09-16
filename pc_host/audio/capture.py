"""PC speaker loopback via WASAPI (pyaudiowpatch) -> device speaker."""
from __future__ import annotations
from collections.abc import Callable
from ..protocol import PCM_BYTES, SAMPLE_RATE

class Capture:
    def __init__(self, send_frame: Callable[[bytes], None]) -> None:
        self.send_frame, self._stream, self._rms, self._audio = send_frame, None, 0, None
        self._src = []

    def start(self) -> None:
        import pyaudiowpatch as pyaudio
        import array
        self._audio = pyaudio.PyAudio()
        default_out = self._audio.get_default_output_device_info()
        loopback = None
        for i in range(self._audio.get_device_count()):
            d = self._audio.get_device_info_by_index(i)
            if d.get("isLoopbackDevice") and (default_out["name"] in d["name"] or d["name"] in default_out["name"]):
                loopback = d; break
        if loopback is None:
            for i in range(self._audio.get_device_count()):
                d = self._audio.get_device_info_by_index(i)
                if d.get("isLoopbackDevice") and d["maxInputChannels"] > 0:
                    loopback = d; break
        if loopback is None:
            raise RuntimeError("no WASAPI loopback")
        src_rate = int(loopback["defaultSampleRate"])
        ch = loopback["maxInputChannels"]
        self._ratio = src_rate / SAMPLE_RATE
        need = int(320 * self._ratio) + 2
        print(f"[capture] {loopback['name']} {src_rate}Hz ch={ch} need={need}", flush=True)

        def callback(in_data, frame_count, time_info, status):
            st = array.array("h", bytes(in_data))
            if ch == 2:
                for i in range(0, len(st) - 1, 2):
                    self._src.append((st[i] + st[i + 1]) // 2)
            else:
                self._src.extend(st)
            while len(self._src) >= need:
                out = array.array("h", b"\x00" * 640)
                for k in range(320):
                    fp = k * self._ratio
                    i0 = int(fp); frac = fp - i0
                    out[k] = int(self._src[i0] + (self._src[i0+1] - self._src[i0]) * frac)
                del self._src[:int(320 * self._ratio)]
                self._rms = min(100, int(((sum(x*x for x in out)/320)**0.5)/32768.0*1500))
                self.send_frame(bytes(out))
            return (None, pyaudio.paContinue)

        self._stream = self._audio.open(
            format=pyaudio.paInt16, channels=ch, rate=src_rate,
            input=True, input_device_index=loopback["index"],
            stream_callback=callback)
        self._stream.start_stream()

    def close(self) -> None:
        if self._stream: self._stream.stop_stream(); self._stream.close(); self._stream = None
        if self._audio: self._audio.terminate(); self._audio = None
