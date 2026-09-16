"""Heartbeat link manager.  Callbacks never run on a GUI thread."""
from __future__ import annotations
import json
import socket
import threading
import time
from collections.abc import Callable

SIGNAL_PORT = 50003

class NetLink:
    def __init__(self, device_ip: str, on_state: Callable[[bool, dict], None] | None = None) -> None:
        self.device_ip, self.on_state = device_ip, on_state
        self._stop = threading.Event(); self._thread: threading.Thread | None = None
        self._sock: socket.socket | None = None; self.online = False; self.info: dict = {}

    def start(self) -> None:
        if self._thread: return
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); self._sock.settimeout(.25)
        self._thread = threading.Thread(target=self._run, name="heartbeat", daemon=True); self._thread.start()

    def close(self) -> None:
        self._stop.set()
        if self._thread: self._thread.join(1); self._thread = None
        if self._sock: self._sock.close(); self._sock = None

    def send(self, message: dict) -> None:
        if self._sock: self._sock.sendto(json.dumps(message, separators=(",", ":")).encode(), (self.device_ip, SIGNAL_PORT))

    def _set_state(self, online: bool, info: dict | None = None) -> None:
        if self.online == online and not info: return
        self.online = online; self.info = info or self.info
        if self.on_state: self.on_state(online, self.info)

    def _run(self) -> None:
        assert self._sock; last_ack = 0.0; last_send = 0.0
        while not self._stop.is_set():
            now = time.monotonic()
            if now - last_send >= 0.2:
                self.send({"type":"HEARTBEAT", "ver":1, "ts":int(now * 1000), "sw_ver":"0.1", "listen_ports":[50000,50002]}); last_send = now
            try:
                data, _ = self._sock.recvfrom(512); message = json.loads(data)
                if message.get("type") == "HEARTBEAT_ACK": last_ack = now; self._set_state(True, message)
            except (TimeoutError, socket.timeout): pass
            except (OSError, json.JSONDecodeError): pass
            if self.online and now - last_ack > 5: self._set_state(False)
