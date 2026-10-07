"""§76 壁纸上传（设备拉模型，与 pc_host/ota 同构）。

流程：本机预校验（≤2MB + SOF 解析 = 800x480）→ httpd 挂 /wallpaper/<name> 路由
→ UDP 50003 发 WP_OFFER（×3 次、每 8s，等 WP_RESULT）→ 设备 HTTP GET 拉取
→ 设备回 {"type":"WP_RESULT","ok":..,"name":..,"err":..} → 结果投 ctx.wp_q。

★ 设备回包固定到 PC:50003（link_manager.c 端口覆写），OFFER 必须走 link.send。
★ sha256 为协议保留字段，设备端忽略（TCP 完整性 + validate_file 尺寸/格式闸兜底）。
"""
from __future__ import annotations

import threading
from pathlib import Path

from .ota.session import pick_local_ip_for

WP_FILE_MAX = 2 * 1024 * 1024        # 与设备端 WP_FETCH_MAX_FILE 对齐
WP_W, WP_H = 800, 480                # 与设备端 WP_W/WP_H 对齐（非整屏必拒）
OFFER_TRIES = 3
OFFER_INTERVAL_S = 8.0


def parse_jpeg_size(data: bytes) -> tuple[int, int] | None:
    """最小 SOF 解析：返回 (w, h)。仅找 SOF0~SOF15（跳过 DHT/JPG/DAC）。"""
    n = len(data)
    i = 2                                    # 跳过 SOI (FF D8)
    while i + 4 <= n:
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        if marker == 0xD8 or marker == 0x01 or 0xD0 <= marker <= 0xD7:
            i += 2                           # 无长度段
            continue
        seg_len = int.from_bytes(data[i + 2:i + 4], "big")
        if 0xC0 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
            if i + 9 > n:
                return None
            h = int.from_bytes(data[i + 5:i + 7], "big")
            w = int.from_bytes(data[i + 7:i + 9], "big")
            return w, h
        if seg_len < 2:
            return None
        i += 2 + seg_len
    return None


def precheck(path: Path) -> tuple[int, int, int] | str:
    """本地预校验。@return (w, h, size) 或错误描述字符串。"""
    try:
        data = path.read_bytes()
    except OSError as e:
        return f"读取失败: {e}"
    if len(data) > WP_FILE_MAX:
        return f"文件 {len(data)} B > 上限 {WP_FILE_MAX} B"
    dims = parse_jpeg_size(data)
    if dims is None:
        return "不是合法 JPEG"
    w, h = dims
    if (w, h) != (WP_W, WP_H):
        return f"尺寸 {w}x{h} ≠ {WP_W}x{WP_H}（设备只收整屏图）"
    return w, h, len(data)


class WallpaperSession:
    """一次上传的会话：OFFER×3 → WP_RESULT。状态只在本对象，页面读 ctx.wp_q。"""

    def __init__(self, ctx) -> None:
        self.ctx = ctx
        self.active = False
        self.path: Path | None = None
        self.name = ""
        self.size = 0
        self._done = threading.Event()
        self._result: dict | None = None
        self._thread: threading.Thread | None = None

    # ---- 页面入口 ----
    def start(self, path: Path) -> str | None:
        """@return None=已启动；str=预校验/环境错误。"""
        if self.active:
            return "已有上传在进行"
        link = self.ctx.link
        if link is None or not link.online or not link.device_ip:
            return "设备离线"
        chk = precheck(path)
        if isinstance(chk, str):
            return chk
        self.path = path
        self.name = path.name            # 已校验为 .jpg/.jpeg 由页面过滤器保证
        self.size = chk[2]
        self._done.clear()
        self._result = None
        self.active = True
        self.ctx.httpd.attach_wallpaper(self)
        self._thread = threading.Thread(target=self._run, name="wp-session", daemon=True)
        self._thread.start()
        return None

    def abort(self) -> None:
        self._done.set()                 # 线程看到 result 未到即按取消收场

    # ---- link 分发器回调（link 线程）----
    def feed(self, msg: dict) -> None:
        if str(msg.get("type", "")) == "WP_RESULT":
            self._result = msg
            self._done.set()

    # ---- 工作线程 ----
    def _run(self) -> None:
        try:
            self._run_impl()
        finally:
            self.active = False
            self.ctx.httpd.detach_wallpaper(self)

    def _run_impl(self) -> None:
        dev_ip = self.ctx.link.device_ip
        url = f"http://{pick_local_ip_for(dev_ip)}:{self.ctx.httpd.port}/wallpaper/{self.name}"
        self._log(f"壁纸上传开始: {self.name} ({self.size} B)，通告 URL: {url}")
        offer = {"type": "WP_OFFER", "name": self.name,
                 "size": self.size, "sha256": "", "url": url}

        result = None
        for attempt in range(1, OFFER_TRIES + 1):
            self.ctx.link.send(offer)
            self._log(f"WP_OFFER 已发（第 {attempt}/{OFFER_TRIES} 次）")
            if self._done.wait(OFFER_INTERVAL_S):
                if self._result is None:
                    # ★ abort() 置位 _done 但不填 _result —— 这是用户取消，
                    #   不能误报成"设备无响应（超时）"（§71：取消必须有可信反馈）
                    self._push(False, "已取消上传")
                    return
                result = self._result
                break
        if result is None:
            self._push(False, "设备无响应（WP_RESULT 超时）")
            return
        ok = bool(result.get("ok"))
        err = str(result.get("err", "") or "")
        self._push(ok, None if ok else (err or "设备拒绝"))

    def _push(self, ok: bool, err: str | None) -> None:
        line = f"壁纸上传{'成功' if ok else '失败'}: {self.name}" + (f"（{err}）" if err else "")
        self._log(line)
        try:
            self.ctx.wp_q.put_nowait({"ev": "result", "ok": ok, "name": self.name,
                                      "err": err or "", "line": line})
        except Exception:                     # noqa: BLE001 - 队列满不阻塞线程
            pass

    def _log(self, s: str) -> None:
        try:
            self.ctx.wp_q.put_nowait({"ev": "log", "line": s})
        except Exception:                     # noqa: BLE001
            pass
