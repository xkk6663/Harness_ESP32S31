"""Optional sherpa-onnx adapter with a no-op fallback when no model is configured."""
from __future__ import annotations
import queue
import threading
from collections.abc import Callable

class AsrBackend:
    def __init__(self, on_text: Callable[[str], None], recognizer=None) -> None:
        self.on_text, self.recognizer = on_text, recognizer; self.queue: queue.Queue[bytes] = queue.Queue(maxsize=64); self.stop = threading.Event(); self.thread: threading.Thread | None = None
    def push(self, pcm: bytes) -> None:
        if not self.queue.full(): self.queue.put_nowait(pcm)
    def start(self) -> None:
        self.thread = threading.Thread(target=self._run, name="asr", daemon=True); self.thread.start()
    def close(self) -> None:
        self.stop.set()
        if self.thread: self.thread.join(1)
    def _run(self) -> None:
        while not self.stop.is_set():
            try: pcm = self.queue.get(timeout=.2)
            except queue.Empty: continue
            if self.recognizer is None: continue
            self.recognizer.accept_waveform(16000, pcm)
            if self.recognizer.is_ready():
                result = self.recognizer.decode_stream();
                if result.text: self.on_text(result.text)
