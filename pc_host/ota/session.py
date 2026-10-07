"""OTA 会话线程：OFFER → ACCEPT → PROGRESS/RESULT → 自证核对。

线程模型（F44 纪律）：
  - 会话跑在独立 daemon 线程，绝不直接碰 Qt widget；
  - 日志走 ctx.ota_log()（只投队列），状态/进度事件投 ctx.ota_q，
    OTA 页在 Qt 定时器里排空 —— 与 call_page._asr_q / pcnow_page 同一套写法；
  - 设备回包（OTA_ACCEPT/PROGRESS/RESULT）由 link 线程的 on_json 分发器
    调 session.feed() 塞进 _inbox，会话线程自行消费 —— 两个线程只经队列握手。

HTTP 侧："就绪才宣告" —— 固件解析成功后才把 session 挂到 httpd 的
/firmware.* 路由（http_server.attach_ota），失败/结束即摘除，设备拿不到半成品。
"""
from __future__ import annotations

import json
import queue
import socket
import threading
import time
from pathlib import Path

from .fw_info import FirmwareInfo

CHUNK = 64 * 1024
WAIT_ONLINE_S = 20
OFFER_TRIES = 3
ACCEPT_WAIT_S = 10
DOWNLOAD_TIMEOUT_S = 180
PEER_LOST_ABORT_S = 8
VERIFY_TIMEOUT_S = 60


def compare_semver(a: str, b: str) -> int:
    """SemVer 粗比对：a>b 正、a<b 负、相等 0。支持 v 前缀 / -dirty / +build。
    只用于"要不要升"的粗判；完整性靠 SHA256。"""
    def parse(s: str) -> list[int]:
        s = s.strip().lstrip("vV")
        core = s.split("-")[0].split("+")[0]
        parts = core.split(".")
        out = [0, 0, 0]
        for i in range(min(3, len(parts))):
            try:
                out[i] = int(parts[i])
            except ValueError:
                pass
        return out
    x, y = parse(a), parse(b)
    for i in range(3):
        if x[i] != y[i]:
            return x[i] - y[i]
    return 0


def pick_local_ip_for(device_ip: str) -> str:
    """选设备能到达的本机 IPv4（connect-trick 让内核选路由出接口）。"""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect((device_ip, 9))            # discard 端口，不会真发包
        ip = s.getsockname()[0]
        s.close()
        return ip
    except OSError:
        return "127.0.0.1"


class OtaSession:
    """一次 OTA 升级会话。属性 active 供 httpd 路由判断（就绪才宣告）。"""

    def __init__(self, ctx, fw: FirmwareInfo, *, limit_percent: int = 0,
                 corrupt: bool = False, force: bool = False) -> None:
        self.ctx = ctx
        self.fw = fw
        self.limit_percent = max(0, min(99, int(limit_percent)))
        self.corrupt = corrupt
        self.force = force

        self.active = False                  # httpd 路由据此注册 /firmware.*
        self.state = "idle"
        self.served_bytes = 0
        self.serve_count = 0
        self.result_ok: bool | None = None

        self._inbox: queue.Queue = queue.Queue(maxsize=200)
        self._stop = threading.Event()
        self._abort_sent = False
        self._lock = threading.Lock()

    # ---- 对外（UI / 分发器 / httpd） ----
    def start(self) -> None:
        self.active = True
        self.ctx.httpd.attach_ota(self)
        self._thread = threading.Thread(target=self._run, name="ota-session", daemon=True)
        self._thread.start()

    def stop(self, reason: str = "user_cancel") -> None:
        """中止：置停机位 + 给设备发 OTA_ABORT（幂等）。"""
        if not self._abort_sent:
            self._abort_sent = True
            self.ctx.link.send({"type": "OTA_ABORT", "reason": reason})
            self._log("已发送 OTA_ABORT（设备侧块边界立即停）")
        self._stop.set()

    def feed(self, msg: dict) -> None:
        """link 线程调用：设备 OTA_* 回包入口（非阻塞）。"""
        try:
            self._inbox.put_nowait(msg)
        except queue.Full:
            pass

    def firmware_json(self) -> bytes:
        """GET /firmware.json。★ --ota-limit 时上报截断后 size（与 C# 同语义）。"""
        return json.dumps({
            "ver": self.fw.version,
            "size": self.served_size,
            "sha256": self.fw.sha256,
            "full_size": self.fw.size,
            "limit_percent": self.limit_percent,
        }).encode("utf-8")

    @property
    def served_size(self) -> int:
        return self.fw.size * self.limit_percent // 100 if self.limit_percent else self.fw.size

    def serve_firmware(self, out, start_offset: int, is_head: bool) -> None:
        """GET /firmware.bin 响应体：流式 64KB 分块写出（支持 Range 续传偏移）。"""
        limit = self.served_size
        start_offset = max(0, min(start_offset, limit))
        remain = limit - start_offset
        if is_head:
            self._log(f"HEAD firmware.bin: total={limit} B, range={start_offset}-")
            return
        corrupt_at = (remain // 2) if self.corrupt else -1
        corrupted = False
        total = 0
        with open(self.fw.path, "rb") as f:
            f.seek(start_offset)
            while total < remain:
                want = min(CHUNK, remain - total)
                buf = f.read(want)
                if not buf:
                    break
                if 0 <= corrupt_at < total + len(buf) and not corrupted:
                    idx = corrupt_at - total
                    buf = bytearray(buf)
                    buf[idx] ^= 0x01
                    buf = bytes(buf)
                    corrupted = True
                    self._log(f"★ CORRUPT 注入 @ 流偏移 {start_offset + corrupt_at}"
                              "（预期设备端 esp_ota_end / 段哈希报错）")
                out.write(buf)
                total += len(buf)
        with self._lock:
            self.served_bytes += total
            self.serve_count += 1
            cnt = self.serve_count
        self._log(f"served firmware.bin: {total} B (offset={start_offset}, "
                  f"{'limit=' + str(self.limit_percent) + '%' if self.limit_percent else 'full'}"
                  f"{', CORRUPTED' if corrupted else ''}) 请求 #{cnt}")

    # ---- 会话主流程（会话线程） ----
    def _log(self, s: str) -> None:
        self.ctx.ota_log(f"[ota] {s}")

    def _emit(self, ev: str, **kw) -> None:
        try:
            self.ctx.ota_q.put_nowait({"ev": ev, **kw})
        except queue.Full:
            pass

    def _set_state(self, s: str) -> None:
        self.state = s
        self._emit("state", state=s)

    def _run(self) -> None:
        try:
            self._run_impl()
        except _Rejected as e:
            self._finish(False, f"设备拒绝 OTA_OFFER: {e}")
        except Exception as e:  # noqa: BLE001 —— 会话线程绝不静默死
            self._log(f"会话异常终止: {type(e).__name__}: {e}")
            self._finish(False, f"session error: {e}")
        finally:
            self.active = False
            self.ctx.httpd.detach_ota(self)

    def _run_impl(self) -> None:
        fw = self.fw
        self._set_state("wait_online")
        self._log("=" * 46)
        self._log(f"固件    : {fw.path}")
        self._log(f"版本    : {fw.version}  ({fw.project_name}, build {fw.build_time})")
        self._log(f"大小    : {fw.size} B ({fw.size / 1048576:.2f} MB)")
        self._log(f"SHA256  : {fw.sha256}")
        if self.limit_percent:
            self._log(f"★ 测试开关: limit {self.limit_percent}% → 只发 {self.served_size} B")
        if self.corrupt:
            self._log("★ 测试开关: corrupt → 流中点翻 1 bit")
        if self.force:
            self._log("★ 测试开关: force → 忽略版本比对")

        # 1) 等设备心跳（复用 NetLink 的在线判定）
        t0 = time.monotonic()
        while not self.ctx.link.online and time.monotonic() - t0 < WAIT_ONLINE_S:
            if self._stop.is_set():
                return self._finish(None, "cancelled")
            time.sleep(0.3)
        if not self.ctx.link.online:
            return self._finish(False, f"设备未上线（{WAIT_ONLINE_S}s 超时）。"
                                "检查：同一 WiFi？防火墙放行 UDP 50003？")
        dev_ip = self.ctx.link.device_ip or ""
        dev_ver = str((self.ctx.link.info or {}).get("fw_ver", "") or "")
        self._log(f"设备在线: {dev_ip}，当前版本: {dev_ver or '(未上报)'}")

        # 2) 版本比对（粗判）
        if not self.force and dev_ver:
            if compare_semver(dev_ver, fw.version) >= 0:
                self._log(f"★ 版本未提升（设备 {dev_ver} >= 固件 {fw.version}），"
                          "默认不发 OFFER。要强行刷入请勾选「忽略版本比对」。")
                return self._finish(False, f"版本未提升（{dev_ver} >= {fw.version}）")
        elif not self.force:
            self._log("设备未上报 fw_ver，跳过版本比对（继续）")

        # 3) OFFER ×3（设备回包固定到 PC:50003，由 link 线程 feed 进来）
        url = f"http://{pick_local_ip_for(dev_ip)}:{self.ctx.httpd.port}/firmware.bin"
        self._log(f"通告 URL: {url}")
        offer = json.dumps({"type": "OTA_OFFER", "ver": fw.version,
                            "size": self.served_size, "sha256": fw.sha256, "url": url},
                           separators=(",", ":"))
        accepted = False
        self._set_state("offer")
        for attempt in range(1, OFFER_TRIES + 1):
            if self._stop.is_set():
                return self._finish(None, "cancelled")
            self._log(f"发送 OTA_OFFER（第 {attempt}/{OFFER_TRIES} 次）")
            self.ctx.link.send(json.loads(offer))
            if self._drain_until_accept(ACCEPT_WAIT_S):
                accepted = True
                break
        if not accepted:
            return self._finish(False, f"未收到 OTA_ACCEPT（{OFFER_TRIES} 次 OFFER 已用尽）")

        # 4) 下载/校验：收 PROGRESS / RESULT；心跳丢失 >8s 发 ABORT
        self._set_state("download")
        dl0 = time.monotonic()
        last_seen = time.monotonic()
        while time.monotonic() - dl0 < DOWNLOAD_TIMEOUT_S:
            if self._stop.is_set():
                self.stop("user_cancel")
                return self._finish(None, "cancelled")
            if self.ctx.link.online:
                last_seen = time.monotonic()
            elif not self._abort_sent and time.monotonic() - last_seen > PEER_LOST_ABORT_S:
                self.stop("peer_lost")
            try:
                msg = self._inbox.get(timeout=0.5)
            except queue.Empty:
                continue
            t = str(msg.get("type", ""))
            if t == "OTA_PROGRESS":
                self._emit("progress", percent=int(msg.get("percent", -1)),
                           speed=int(msg.get("speed", 0)))
            elif t == "OTA_RESULT":
                ok = str(msg.get("ok", "0")) == "1"
                detail = str(msg.get("detail", ""))
                self._log(f"设备上报 RESULT: ok={ok} detail={detail}")
                if not ok:
                    return self._finish(False, f"设备上报升级失败: {detail}")
                break
            elif t == "OTA_REJECT":
                return self._finish(False, f"设备拒绝: {msg.get('reason', '?')}")
        else:
            return self._finish(False, f"下载/校验阶段 {DOWNLOAD_TIMEOUT_S}s 超时（设备可能已断开）")

        # 5) 自证核对：等设备以新版本回来（≤60s）
        self._set_state("verify")
        self._log("升级成功，设备重启中… 等待以新版本回来（自证 30s 内）")
        v0 = time.monotonic()
        while time.monotonic() - v0 < VERIFY_TIMEOUT_S:
            time.sleep(1.0)
            if self.ctx.link.online:
                back_ver = str((self.ctx.link.info or {}).get("fw_ver", "") or "")
                self._log(f"设备已回来: {self.ctx.link.device_ip}，fw_ver={back_ver or '(未上报)'}")
                ver_ok = back_ver == fw.version
                if ver_ok:
                    self._log(f"★ 版本核对通过（{fw.version}）→ 升级成功 ✅")
                else:
                    self._log(f"★ 版本核对不一致: 期望 {fw.version} / 实际 {back_ver}"
                              " → 可能已自动回滚 ❌（查串口 rollback 关键字）")
                return self._finish(True if ver_ok else False, "updated" if ver_ok else "version-mismatch")
        self._finish(False, f"设备重启后 {VERIFY_TIMEOUT_S}s 内未恢复心跳 —— 可能已自动回滚，"
                      "请查串口日志（rollback 关键字）")

    def _drain_until_accept(self, wait_s: float) -> bool:
        deadline = time.monotonic() + wait_s
        while time.monotonic() < deadline:
            if self._stop.is_set():
                return False
            try:
                msg = self._inbox.get(timeout=0.4)
            except queue.Empty:
                continue
            t = str(msg.get("type", ""))
            if t == "OTA_ACCEPT":
                self._log(f"设备已接受（size={msg.get('size', '?')}），开始下载…")
                return True
            if t == "OTA_REJECT":
                reason = str(msg.get("reason", "?"))
                hint = {"busy": "录音/通话中", "pending": "待自证", "space": "槽不够",
                        "network": "离线", "badargs": "参数问题", "badjson": "参数问题"}
                self._log(f"设备拒绝: {reason}（{hint.get(reason, '见 ICD §8')}）")
                raise _Rejected(reason)
        return False

    def _finish(self, ok: bool | None, detail: str) -> None:
        self.result_ok = ok
        self._set_state("done" if ok else ("cancelled" if ok is None else "failed"))
        self._emit("result", ok=ok, detail=detail)
        self._log(f"会话结束: {detail}")


class _Rejected(Exception):
    """_drain_until_accept 收到 REJECT 时上抛，由 _run 统一落 _finish（防双重 result）。"""
