"""只读 HTTP :50005（设备→PC 拉封面/歌词/固件）。ThreadingHTTPServer 标准库实现。

路由（ICD V1.3 + §8 OTA，与 C# agent 一致）：
  GET /cover/<hash>.jpg    -> TrackStore 中的 JPEG 字节
  GET /lyrics.json?k=<hash>-> {"k","ver","lines":[{t,text}...]}
  GET /firmware.json       -> {"ver","size","sha256",...}   （★ 仅 OTA 会话活跃时）
  GET|HEAD /firmware.bin   -> 200/206 + Content-Length + 流式写出（支持 Range 续传）
                              （★ 仅 OTA 会话活跃时 —— "就绪才宣告"，永不给半成品）

★ OTA 路由由 OtaSession 生命周期控制：会话 start() 时 attach_ota()、结束 detach_ota()。
"""
from __future__ import annotations

import json
import re
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

HTTP_PORT = 50005

_RANGE_RE = re.compile(r"bytes=(\d+)-", re.IGNORECASE)


class NowHttpServer:
    def __init__(self, store=None, on_log=None) -> None:
        """store 可为 None（--no-pcnow 模式：只为 OTA 提供服务，封面/歌词路由回 404）。"""
        self.store = store
        self.on_log = on_log or (lambda s: None)
        self.port = HTTP_PORT
        self._ota = None                      # OtaSession | None（就绪才宣告）
        self._httpd: ThreadingHTTPServer | None = None
        self._thread: threading.Thread | None = None
        self._handler = _make_handler(self)

    # ---- OTA 路由挂载/摘除（OtaSession 生命周期调用） ----
    def attach_ota(self, session) -> None:
        self._ota = session
        self.on_log(f"OTA 路由上线：/firmware.json /firmware.bin（{session.fw.version}）")

    def detach_ota(self, session) -> None:
        if self._ota is session:
            self._ota = None
            self.on_log("OTA 路由摘除")

    def start(self) -> bool:
        try:
            self._httpd = ThreadingHTTPServer(("", HTTP_PORT), self._handler)
        except OSError as e:
            self.on_log(f"HTTP :{HTTP_PORT} 绑定失败（C# agent 或其它进程占用？）: {e}")
            return False
        self._httpd.daemon_threads = True
        self._thread = threading.Thread(target=self._httpd.serve_forever, name="now-http", daemon=True)
        self._thread.start()
        self.on_log(f"HTTP 服务就绪 :{HTTP_PORT}（/cover /lyrics.json /firmware.*）")
        return True

    def stop(self) -> None:
        if self._httpd:
            threading.Thread(target=self._httpd.shutdown, daemon=True).start()
            self._httpd.server_close()
            self._httpd = None
        if self._thread:
            self._thread.join(1.5)
            self._thread = None


def _make_handler(srv: NowHttpServer):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"         # ★ Range/206 需要 HTTP/1.1 语义

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

        def _send_firmware(self, is_head: bool) -> None:
            ota = srv._ota
            if ota is None or not ota.active:
                self._send(404, b'{"error":"firmware not ready"}', "application/json")
                return
            total = ota.served_size
            m = _RANGE_RE.match(self.headers.get("Range", "") or "")
            start = int(m.group(1)) if m else 0
            start = max(0, min(start, total))
            if is_head:
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(total))
                self.send_header("Accept-Ranges", "bytes")
                self.end_headers()
                ota.serve_firmware(None, start, is_head=True)
                return
            self.send_response(206 if m else 200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(total - start))
            self.send_header("Accept-Ranges", "bytes")
            if m:
                self.send_header("Content-Range", f"bytes {start}-{total - 1}/{total}")
            self.end_headers()
            try:
                ota.serve_firmware(self.wfile, start, is_head=False)
            except (BrokenPipeError, ConnectionResetError):
                pass                          # 设备中途断开（续传场景常态）

        def do_HEAD(self) -> None:  # noqa: N802
            if urlparse(self.path).path == "/firmware.bin":
                self._send_firmware(is_head=True)
            else:
                self.send_response(200)
                self.send_header("Content-Length", "0")
                self.end_headers()

        def do_GET(self) -> None:  # noqa: N802
            parsed = urlparse(self.path)
            path = parsed.path
            if path == "/firmware.bin":
                self._send_firmware(is_head=False)
                return
            if path == "/firmware.json":
                ota = srv._ota
                if ota is None or not ota.active:
                    self._send(404, b'{"error":"firmware not ready"}', "application/json")
                else:
                    self._send(200, ota.firmware_json(), "application/json")
                return
            if path.startswith("/cover/") and path.endswith(".jpg"):
                if srv.store is None:
                    self._send(404, b'{"error":"pcnow disabled"}', "application/json")
                    return
                h = path[len("/cover/"):-len(".jpg")]
                td = srv.store.get(h)
                if td and td.cover:
                    self._send(200, td.cover, "image/jpeg")
                else:
                    self._send(404, b'{"error":"cover not ready"}', "application/json")
                return
            if path == "/lyrics.json":
                if srv.store is None:
                    self._send(404, b'{"error":"pcnow disabled"}', "application/json")
                    return
                qs = parse_qs(parsed.query)
                h = (qs.get("k") or [""])[0]
                td = srv.store.get(h)
                if td and td.lyrics:
                    body = json.dumps({"k": h, "ver": 1,
                                       "lines": [{"t": t, "text": s} for t, s in td.lyrics]},
                                      ensure_ascii=False).encode("utf-8")
                    # ★ F44：告诉 pusher "这条 hash 的全量歌词已经被设备端拉走了"，
                    #   之后 NOW_LRC 单行通道自动停发（每换一行再推一次是纯冗余）。
                    srv.store.mark_lyric_pulled(h)
                    self._send(200, body, "application/json; charset=utf-8")
                else:
                    self._send(404, b'{"error":"lyrics not ready"}', "application/json")
                return
            self._send(200, '{"service":"pc_host pcnow+ota","ver":1}'.encode(), "application/json")

    return Handler
