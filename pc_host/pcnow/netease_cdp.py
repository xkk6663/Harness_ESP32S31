"""网易云 PC（CEF）位置真值读取 —— P1。

为什么需要它
------------
网易云 PC 的 SMTC 时间轴是**零值死轴**（`position=end_time=timedelta(0)`，
`last_updated=1601`），只有 `playback_status` 正常；实测它的 `<audio>` 元素
`currentTime/duration` 恒为 0、`navigator.mediaSession.playbackState` 恒为 'none'
（页面里 `iframe` 数为 0）⇒ **媒体元素这条路上没有真值**。

唯一的位置真值在**进度 DOM 文本**（`mm:ss / mm:ss`）。真机实测（2026-09-24）：

    Chromium 91 / NeteaseMusicDesktop 3.1.40.205461，目标 orpheus://orpheus/pub/app.html
    t1 = "00:06 / 04:02"  →  t2 = "00:11 / 04:02"（5 秒过去，位置正好 +5 秒）

⇒ 每秒 evaluate 一次即可拿到 (位置, 曲长) **真值**，误差只来自读取时刻（≤1 帧）。

前置条件（重要）
----------------
网易云必须带 `--remote-debugging-port=9222` 启动：双击
`pc_host/restart_cloudmusic_debug.bat`（纯 ASCII，自带 `/json/version` 自检）。
没有这个端口时本模块 `available=False`，调用方必须退回本地内插
（`pusher` 的 `sync_src='event'`），**不能因为拿不到真值就停摆**。

★ 代理陷阱（踩过）
------------------
本机环境常带 `http_proxy/https_proxy=http://127.0.0.1:12635`（Clash 系）。
urllib 默认走它，连 `127.0.0.1` 的 DevTools 端口都会失败，表现为
`upstream connect failed ... 10061`（curl 同理，要 `--noproxy '*'`）。
⇒ 本模块 HTTP 用 `ProxyHandler({})`，WS 用裸 socket（天然不过代理）。
"""

from __future__ import annotations

import base64
import json
import os
import re
import socket
import threading
import time
import urllib.request
from collections.abc import Callable

DEFAULT_PORT = 9222

# "mm:ss / mm:ss"（网易云进度文本；允许 1~2 位分钟）
_TIME_RX = re.compile(r"^\s*(\d{1,2}):(\d{2})\s*/\s*(\d{1,2}):(\d{2})\s*$")

# 进度真值：用**文本形状**定位节点，不用类名 ——
# 实测类名带 hash（`StyledSliderContainer_smigcsb`），随时会被前端构建改掉。
# 选"最深"的匹配节点（祖先的 textContent 会包含子孙，必须取最内层）。
# 性能：用 textContent（不走 layout）；innerText 只在命中的那一个节点上取。
_POS_JS = r"""
(() => {
  const rx = /^\s*(\d{1,2}):(\d{2})\s*\/\s*(\d{1,2}):(\d{2})\s*$/;
  const els = document.querySelectorAll('div,span,p,label,time');
  let best = null;
  for (const el of els) {
    const t = (el.textContent || '').trim();
    if (t.length > 24 || !rx.test(t)) continue;
    if (!best || best.contains(el)) best = el;
  }
  if (!best) return null;
  const a = document.querySelector('audio,video');
  return JSON.stringify({
    text: (best.innerText || best.textContent || '').trim(),
    cls: String(best.className || '').slice(0, 60),
    paused: a ? a.paused : null
  });
})()
""".strip()


# ------------------------------------------------------------------ 最小 WS 客户端

class _WsReader:
    def __init__(self, sock: socket.socket, initial: bytes = b"") -> None:
        self.s = sock
        self.buf = bytearray(initial)

    def _need(self, n: int) -> None:
        while len(self.buf) < n:
            try:
                chunk = self.s.recv(65536)
            except socket.timeout as e:
                raise TimeoutError("ws recv timeout") from e
            if not chunk:
                raise ConnectionError("ws closed by peer")
            self.buf += chunk

    def read_frame(self) -> tuple[int, bytes]:
        self._need(2)
        b0, b1 = self.buf[0], self.buf[1]
        opcode, masked, ln = b0 & 0x0F, bool(b1 & 0x80), b1 & 0x7F
        idx = 2
        if ln == 126:
            self._need(4)
            ln = int.from_bytes(self.buf[2:4], "big")
            idx = 4
        elif ln == 127:
            self._need(10)
            ln = int.from_bytes(self.buf[2:10], "big")
            idx = 10
        mask = b""
        if masked:
            self._need(idx + 4)
            mask = bytes(self.buf[idx:idx + 4])
            idx += 4
        self._need(idx + ln)
        data = bytes(self.buf[idx:idx + ln])
        del self.buf[:idx + ln]
        if masked:
            data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        return opcode, data


def _ws_send(sock: socket.socket, payload: bytes) -> None:
    mask = os.urandom(4)
    n = len(payload)
    hdr = bytearray([0x81])
    if n < 126:
        hdr.append(0x80 | n)
    elif n < 65536:
        hdr.append(0x80 | 126)
        hdr += n.to_bytes(2, "big")
    else:
        hdr.append(0x80 | 127)
        hdr += n.to_bytes(8, "big")
    hdr += mask
    sock.sendall(bytes(hdr) + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))


def _ws_connect(url: str, default_port: int = DEFAULT_PORT,
                timeout: float = 3.0) -> tuple[socket.socket, _WsReader]:
    # ★ Chromium 91（网易云 CEF）返回的 webSocketDebuggerUrl **不带端口**：
    #   ws://127.0.0.1/devtools/page/<ID> ⇒ 必须补默认端口。
    m = re.match(r"ws://([^:/]+)(?::(\d+))?(/.*)?$", url)
    if not m:
        raise ValueError(f"bad ws url: {url}")
    host = m.group(1)
    port = int(m.group(2)) if m.group(2) else default_port
    path = m.group(3) or "/"
    sock = socket.create_connection((host, port), timeout=timeout)
    sock.settimeout(timeout)
    key = base64.b64encode(os.urandom(16)).decode()
    # 不送 Origin 头：Chromium 的 DevTools 端点对"无 Origin"的请求放行
    # （Origin 校验是 Chromium 111+ 才强制，这台是 91）。
    sock.sendall((
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"
    ).encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("handshake: peer closed")
        buf += chunk
    head, rest = buf.split(b"\r\n\r\n", 1)
    status = head.split(b"\r\n")[0].decode("latin-1", "replace")
    if "101" not in status:
        raise ConnectionError(f"handshake failed: {status}")
    return sock, _WsReader(sock, rest)


def _http_json(url: str, timeout: float = 2.0):
    # ★ 必须显式禁用代理，否则会被 Clash 截走（见文件头"代理陷阱"）。
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    req = urllib.request.Request(url, headers={"Host": "127.0.0.1"})
    with opener.open(req, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


# ------------------------------------------------------------------ 对外接口

class NeteaseCdp:
    """网易云进度真值读取器（线程安全；所有失败都静默降级，绝不抛给主循环）。"""

    def __init__(self, port: int = DEFAULT_PORT,
                 on_log: Callable[[str], None] | None = None) -> None:
        self.port = port
        self.on_log = on_log or (lambda s: None)
        self.available = False          # UI 可读：最近一次探测结果
        self.last_text = ""             # 最近一次读到的原始文本，便于排障
        self._sock: socket.socket | None = None
        self._reader: _WsReader | None = None
        self._msg_id = 0
        self._lock = threading.Lock()
        self._retry_after = 0.0
        self._fails = 0

    # ---------------- 探测 / 取数 ----------------

    def probe(self) -> bool:
        """启动时探测一次（端口在不在 + 有没有 page target），并打一条日志。"""
        with self._lock:
            ok = self._ensure()
            if not ok:
                self._retry_after = time.monotonic() + 5.0
        self._set_available(ok)
        self.on_log(
            f"网易云 CDP 已接通 :{self.port} —— 进度走**真值**（sync=dom）"
            if ok else
            f"网易云 CDP 未接通 :{self.port} —— 需带 --remote-debugging-port={self.port} "
            "启动（双击 pc_host\\restart_cloudmusic_debug.bat）；进度退回本地内插（sync=event）"
        )
        return ok

    def poll(self) -> tuple[float, float] | None:
        """→ (位置秒, 曲长秒)；不可用/没读到 → None（调用方原地退回内插）。"""
        now = time.monotonic()
        if now < self._retry_after:
            return None
        with self._lock:
            if not self._ensure():
                self._fail(now)
                return None
            try:
                raw = self._eval(_POS_JS)
            except Exception:  # noqa: BLE001 —— 对端进程重启/页面跳转都可能断
                self._drop()
                self._fail(now)
                return None
        if not raw:
            return None
        try:
            obj = json.loads(raw)
        except ValueError:
            return None
        text = str(obj.get("text") or "")
        m = _TIME_RX.match(text)
        if not m:
            return None
        self.last_text = text
        self._fails = 0
        self._set_available(True)
        pos = int(m.group(1)) * 60 + int(m.group(2))
        dur = int(m.group(3)) * 60 + int(m.group(4))
        return float(pos), float(dur)

    def close(self) -> None:
        with self._lock:
            self._drop()

    # ---------------- 内部 ----------------

    def _set_available(self, ok: bool) -> None:
        if ok != self.available:
            self.available = ok
            if not ok:
                self.on_log(f"网易云 CDP 断开 :{self.port} —— 退回本地内插")

    def _fail(self, now: float) -> None:
        self._fails += 1
        self._retry_after = now + min(30.0, 2.0 * self._fails)   # 2s→4s→…→30s 退避
        self._set_available(False)

    def _drop(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
        self._sock = None
        self._reader = None

    def _ensure(self) -> bool:
        if self._sock is not None:
            return True
        try:
            url = self._page_ws_url()
            self._sock, self._reader = _ws_connect(url, default_port=self.port)
            self._fails = 0
            return True
        except Exception:  # noqa: BLE001
            self._drop()
            return False

    def _page_ws_url(self) -> str:
        targets = _http_json(f"http://127.0.0.1:{self.port}/json/list")
        for t in targets:
            if t.get("type") == "page" and t.get("webSocketDebuggerUrl"):
                return str(t["webSocketDebuggerUrl"])
        raise RuntimeError("no page target on DevTools endpoint")

    def _eval(self, expr: str) -> str | None:
        assert self._sock is not None and self._reader is not None
        self._msg_id += 1
        mid = self._msg_id
        _ws_send(self._sock, json.dumps({
            "id": mid,
            "method": "Runtime.evaluate",
            "params": {"expression": expr, "returnByValue": True,
                       "awaitPromise": False},
        }).encode())
        deadline = time.time() + 3.0
        while time.time() < deadline:
            opcode, data = self._reader.read_frame()
            if opcode != 0x1:
                continue
            msg = json.loads(data.decode("utf-8", "replace"))
            if msg.get("id") != mid:
                continue
            res = msg.get("result", {})
            if "exceptionDetails" in res:
                raise RuntimeError("cdp js exception")
            return res.get("result", {}).get("value")
        raise TimeoutError("cdp eval timeout")
