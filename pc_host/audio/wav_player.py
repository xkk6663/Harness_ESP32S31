"""WAV file player: reads 16k/16bit/mono PCM from a wav file and sends frames.
Driven by a timer (not pyaudio loopback) so it works without audio capture.
"""
from __future__ import annotations
import struct
import threading
import time
from collections.abc import Callable
from pathlib import Path
from ..protocol import PCM_BYTES, SAMPLE_RATE

class WavPlayer:
    def __init__(self, send_frame: Callable[[bytes], None]) -> None:
        self.send_frame = send_frame
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        self._file: Path | None = None
        self._pcm = b""
        self._pos = 0
        self._rms = 0

    def load(self, path: str | Path) -> bool:
        p = Path(path)
        if not p.exists():
            return False
        with open(p, "rb") as f:
            data = f.read()
        # parse wav: find 'data' chunk
        if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
            return False
        off = 12
        pcm = b""
        while off + 8 <= len(data):
            cid = data[off:off+4]
            csize = struct.unpack_from("<I", data, off+4)[0]
            if cid == b"data":
                pcm = data[off+8:off+8+csize]
                break
            off += 8 + csize + (csize & 1)
        if not pcm:
            return False
        self._pcm = pcm
        self._pos = 0
        self._file = p
        import array as _a
        _s = _a.array("h", pcm[:min(len(pcm), 64000)])
        _rms = int((sum(x*x for x in _s)/max(len(_s),1))**0.5)
        print(f"[wav] loaded {p.name} {len(pcm)} bytes = {len(pcm)//2//SAMPLE_RATE}s head_rms={_rms} first_nz={next((i for i,b in enumerate(pcm) if b), -1)}", flush=True)
        return True

    def start(self) -> None:
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="wav-player", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(1)
            self._thread = None

    def close(self) -> None:
        self.stop()

    def _run(self) -> None:
        next_tick = time.monotonic()
        sent = 0
        stat_t = time.monotonic()
        while not self._stop.is_set():
            if not self._pcm:
                time.sleep(0.1); continue
            if self._pos >= len(self._pcm):
                self._pos = 0
                print(f"[wav] loop", flush=True)
            chunk = self._pcm[self._pos:self._pos + PCM_BYTES]
            if len(chunk) < PCM_BYTES:
                chunk = chunk + b"\x00" * (PCM_BYTES - len(chunk))
            self._pos += PCM_BYTES
            import array
            s = array.array("h", chunk)
            self._rms = min(100, int(((sum(x*x for x in s)/len(s))**0.5)/32768.0*1500))
            try:
                self.send_frame(chunk)
                sent += 1
            except Exception as e:
                print(f"[wav] send err: {e}", flush=True)
            if time.monotonic() - stat_t > 2:
                print(f"[wav] sent={sent} rms={self._rms} pos={self._pos}/{len(self._pcm)}", flush=True)
                sent = 0; stat_t = time.monotonic()
            next_tick += 0.02
            dt = next_tick - time.monotonic()
            if dt > 0:
                time.sleep(dt)
            else:
                next_tick = time.monotonic()
