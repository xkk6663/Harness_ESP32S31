"""UDP voice receiver/reorderer and PTT packet sender."""
from __future__ import annotations
import socket
import threading
import time
from collections.abc import Callable
from ..protocol import PCM_BYTES, pack_voice_frame, unpack_voice_frame

class UdpVoice:
    def __init__(self, device_ip: str, on_pcm: Callable[[bytes], None]) -> None:
        self.device_ip, self.on_pcm = device_ip, on_pcm; self._stop = threading.Event(); self._thread: threading.Thread | None = None
        self._rx: socket.socket | None = None; self._tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); self._seq = 0

    def start(self) -> None:
        self._rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); self._rx.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); self._rx.bind(("", 50000)); self._rx.settimeout(.02)
        self._thread = threading.Thread(target=self._run, name="udp-rx", daemon=True); self._thread.start()

    def close(self) -> None:
        self._stop.set()
        if self._thread: self._thread.join(1)
        if self._rx: self._rx.close()
        self._tx.close()

    def send_ptt(self, pcm: bytes) -> None:
        packet = pack_voice_frame(self._seq, int(time.monotonic() * 1000), pcm); self._seq = (self._seq + 1) & 0xFFFF
        self._tx.sendto(packet, (self.device_ip, 50001))

    def _run(self) -> None:
        assert self._rx; slots: dict[int, bytes] = {}; expected: int | None = None; deadline = time.monotonic()
        dbg = {"recv": 0, "bad": 0, "last": 0.0}
        while not self._stop.is_set():
            try:
                raw = self._rx.recv(1024)
                dbg["recv"] += 1
                frame = unpack_voice_frame(raw); slots[frame.seq] = frame.pcm
                if expected is None:
                    expected = frame.seq
                    deadline = time.monotonic()
                else:
                    # detect session restart: device seq wraps back to 0
                    diff = (frame.seq - expected) & 0xFFFF
                    if diff > 0x7FFF:  # huge backwards jump = new session
                        print(f"[voice] seq restart {expected}->{frame.seq}, resetting", flush=True)
                        expected = frame.seq
                        deadline = time.monotonic()
                        slots.clear()
            except (TimeoutError, socket.timeout): pass
            except ValueError as e:
                dbg["bad"] += 1
                if time.monotonic() - dbg["last"] > 2:
                    print(f"[voice] bad pkt: len={len(raw) if 'raw' in dir() else 0} err={e}", flush=True)
                    dbg["last"] = time.monotonic()
            now = time.monotonic()
            if time.monotonic() - dbg["last"] > 2 and dbg["recv"]:
                print(f"[voice] recv={dbg['recv']} bad={dbg['bad']}", flush=True)
                dbg["recv"] = 0; dbg["bad"] = 0; dbg["last"] = time.monotonic()
            while expected is not None and now >= deadline:
                got = slots.pop(expected, None)
                if got is None:
                    dbg["miss"] = dbg.get("miss", 0) + 1
                else:
                    self.on_pcm(got)
                expected = (expected + 1) & 0xFFFF; deadline += .02
            if dbg.get("miss", 0) and time.monotonic() - dbg.get("last_miss_log", 0) > 2:
                print(f"[voice] missed {dbg['miss']} frames", flush=True); dbg["miss"] = 0; dbg["last_miss_log"] = time.monotonic()
