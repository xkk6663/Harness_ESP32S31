"""只读 HTTP :50005（设备→PC 拉封面/歌词）。ThreadingHTTPServer 标准库实现。

路由（ICD V1.3，与 C# agent 一致）：
  GET /cover/<hash>.jpg    -> TrackStore 中的 JPEG 字节
  GET /lyrics.json?k=<hash>-> {"k","ver","lines":[{t,text}...]}
"""
from __future__ import annotations

import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

from .pusher import TrackStore

HTTP_PORT = 50005


class NowHttpServer:
    def __init__(self, store: TrackStore, on_log=None) -> None:
        self.store = store
        self.on_log = on_log or (lambda s: None)
        self._httpd: ThreadingHTTPServer | None = None
        self._thread: threading.Thread | None = None
        self._handler = _make_handler(store, self.on_log)

    def start(self) -> bool:
        try:
            self._httpd = ThreadingHTTPServer(("", HTTP_PORT), self._handler)
        except OSError as e:
            self.on_log(f"HTTP :{HTTP_PORT} 绑定失败（C# agent 或其它进程占用？）: {e}")
            return False
        self._httpd.daemon_threads = True
        self._thread = threading.Thread(target=self._httpd.serve_forever, name="now-http", daemon=True)
        self._thread.start()
        self.on_log(f"HTTP 服务就绪 :{HTTP_PORT}（/cover /lyrics.json）")
        return True

    def stop(self) -> None:
        if self._httpd:
            threading.Thread(target=self._httpd.shutdown, daemon=True).start()
            self._httpd.server_close()
            self._httpd = None
        if self._thread:
            self._thread.join(1.5)
            self._thread = None


def _make_handler(store: TrackStore, on_log):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):  # 静默默认访问日志
            pass

        def _send(self, code: int, body: bytes, ctype: str) -> None:
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def do_GET(self) -> None:  # noqa: N802
            parsed = urlparse(self.path)
            path = parsed.path
            if path.startswith("/cover/") and path.endswith(".jpg"):
                h = path[len("/cover/"):-len(".jpg")]
                td = store.get(h)
                if td and td.cover:
                    self._send(200, td.cover, "image/jpeg")
                else:
                    self._send(404, b'{"error":"cover not ready"}', "application/json")
                return
            if path == "/lyrics.json":
                qs = parse_qs(parsed.query)
                h = (qs.get("k") or [""])[0]
                td = store.get(h)
                if td and td.lyrics:
                    body = json.dumps({"k": h, "ver": 1,
                                       "lines": [{"t": t, "text": s} for t, s in td.lyrics]},
                                      ensure_ascii=False).encode("utf-8")
                    # ★ F44：告诉 pusher "这条 hash 的全量歌词已经被设备端拉走了"，
                    #   之后 NOW_LRC 单行通道自动停发（每换一行再推一次是纯冗余）。
                    store.mark_lyric_pulled(h)
                    self._send(200, body, "application/json; charset=utf-8")
                else:
                    self._send(404, b'{"error":"lyrics not ready"}', "application/json")
                return
            self._send(200, '{"service":"pc_host pcnow","ver":1}'.encode(), "application/json")

    return Handler
