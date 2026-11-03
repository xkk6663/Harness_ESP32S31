"""C3 上位机去 Qt：OtaSession 的最小上下文替身 + 固件行为替身。

════════════════════════════════════════════════════════════════════
为什么需要这一层（可测性改造的唯一改动点）
  OtaSession 原本强绑 Qt 侧的 AppContext：它只用到 5 个能力——
      ctx.httpd.attach_ota / detach_ota / port
      ctx.link.online / device_ip / info / send()
      ctx.ota_log(str) / ctx.ota_q(Queue) / ctx.ota_store(可选)
  这里用 40 行替身把这 5 个能力补齐，于是会话可以：
      · 无 PySide6、无显示器、无真机地跑完整个状态机（离线模式）；
      · 也可以在真机上复用同一份代码（真实模式 = NetLink + NowHttpServer）。
  ★ OtaSession 一行未改 —— 测的是"发布形态的那条路径"，不是 mock 出来的假路径。
════════════════════════════════════════════════════════════════════
MockDevice 是"固件行为替身"，语义严格对齐设备端源码：
  · 6 条校验链全部通过才回 OTA_ACCEPT（ota_service.c:683-724）
  · 每 2% 回 OTA_PROGRESS（OTA_PROGRESS_STEP=2，ota_service.c:88/1066）
  · 校验结论回 OTA_RESULT{ok:0|1}（ota_service.c:15）
  · 升级后重启 → 断链 → 以新版本回来（自证窗口 30s，main.c:161-178）
"""
from __future__ import annotations

import json
import queue
import threading
import time


class MockHttpd:
    """httpd 替身：只记录 attach/detach 与端口（真机模式换 NowHttpServer）。"""

    def __init__(self, port: int = 18080) -> None:
        self.port = port
        self.session = None
        self.attach_count = 0
        self.detach_count = 0

    def attach_ota(self, session) -> None:
        self.session = session
        self.attach_count += 1

    def detach_ota(self, session=None) -> None:
        self.session = None
        self.detach_count += 1


class MockLink:
    """链路替身：send() 等价于经 UDP 50003 发给设备。"""

    def __init__(self, device: "MockDevice") -> None:
        self.device = device
        self.device_ip = "127.0.0.1"
        self.info: dict = {}
        self._online = True

    @property
    def online(self) -> bool:
        return self._online

    @online.setter
    def online(self, v: bool) -> None:
        self._online = bool(v)

    def send(self, message: dict) -> None:
        self.device.on_link_message(message)


class NullCtx:
    """OtaSession 的最小运行上下文（无 Qt）。"""

    def __init__(self, httpd: MockHttpd, link: MockLink, *, echo: bool = False) -> None:
        self.httpd = httpd
        self.link = link
        self.ota_q: queue.Queue = queue.Queue()
        self.ota_store = None
        self.logs: list[str] = []
        self._echo = echo

    def ota_log(self, s: str) -> None:
        self.logs.append(s)
        if self._echo:
            print(s, flush=True)

    def drain_events(self) -> list[dict]:
        out = []
        while True:
            try:
                out.append(self.ota_q.get_nowait())
            except queue.Empty:
                return out


class MockDevice:
    """固件行为替身（默认：一切正常，2% 步进，升级成功后回滚确认）。

    失效注入开关（全部对应真实场景）：
        reject           —— 6 条校验链中的某一条把 OFFER 拒掉（值就是 reason 枚举）
        result_ok        —— 设备端 esp_ota_end / SHA256 校验结论
        progress         —— 是否回报进度（False 用于"下载停滞"场景）
        reboot           —— 是否"重启后回来"（False 用于"自证超时/回滚"场景）
        back_ver         —— 重启后上报的版本（不等于目标版本 => 被判回滚）
        online_at_start  —— 起始是否在线（False 用于"设备未上线"场景）
        drop_link_after_accept —— 下载中拔网（用于 peer_lost → OTA_ABORT）
    """

    def __init__(self, *, dev_ver: str = "1.0.0", reject: str | None = None,
                 result_ok: bool = True, progress: bool = True, reboot: bool = True,
                 back_ver: str | None = None, online_at_start: bool = True,
                 drop_link_after_accept: bool = False,
                 progress_step_ms: int = 8, reboot_delay_s: float = 1.2) -> None:
        self.link = MockLink(self)
        self.link._online = online_at_start
        self.link.info = {"fw_ver": dev_ver} if online_at_start else {}

        self.reject = reject
        self.result_ok = result_ok
        self.progress = progress
        self.reboot = reboot
        self.back_ver = back_ver
        self.drop_link_after_accept = drop_link_after_accept
        self.progress_step_ms = progress_step_ms
        self.reboot_delay_s = reboot_delay_s

        self.offers: list[dict] = []
        self.aborts: list[dict] = []
        self.accepts = 0
        self.feed_sink = None          # 由 wire() 注入：设备回包 -> session.feed()
        self._lock = threading.Lock()

    # ---- 设备回包（等价于设备主动往 PC:50003 发 JSON，由 link 分发器喂给会话） ----
    def _reply(self, msg: dict) -> None:
        if self.feed_sink is not None:
            self.feed_sink(dict(msg))

    # ---- 设备收报文（由 MockLink.send 在会话线程里调用） ----
    def on_link_message(self, msg: dict) -> None:
        t = msg.get("type")
        if t == "OTA_OFFER":
            with self._lock:
                self.offers.append(dict(msg))
            threading.Thread(target=self._simulate, args=(dict(msg),), daemon=True).start()
        elif t == "OTA_ABORT":
            self.aborts.append(dict(msg))
            self.link.online = False

    # ---- 一次升级的完整固件侧行为 ----
    def _simulate(self, offer: dict) -> None:
        # 校验链优先（设备端先判失败再宣告接受）
        if self.reject:
            self._reply({"type": "OTA_REJECT", "reason": self.reject})
            return

        size = int(offer.get("size", 0))
        self.accepts += 1
        self._reply({"type": "OTA_ACCEPT", "size": size})

        if self.drop_link_after_accept:
            time.sleep(0.6)
            self.link.online = False
            return

        if self.progress:
            for pct in range(2, 101, 2):
                self._reply({"type": "OTA_PROGRESS", "percent": pct, "speed": 29})
                time.sleep(self.progress_step_ms / 1000.0)

        if not self.progress:
            return  # 既不回进度也不回结论 → 制造"停滞"

        self._reply({"type": "OTA_RESULT", "ok": "1" if self.result_ok else "0",
                     "detail": "" if self.result_ok else "sha256 mismatch"})
        if not self.result_ok:
            return

        if self.reboot:
            self.link.online = False
            time.sleep(self.reboot_delay_s)
            ver = self.back_ver if self.back_ver is not None else offer.get("ver", "")
            self.link.info = {"fw_ver": ver}
            self.link.online = True
        else:
            # 重启后再也没回来（变砖/卡在 bootloader/自动回滚也没起来）
            self.link.online = False


def build_offline(device: MockDevice, *, echo: bool = False) -> tuple[NullCtx, MockDevice]:
    """组装一套离线运行上下文：ctx 与 device（device.link / httpd 已就绪）。"""
    httpd = MockHttpd()
    ctx = NullCtx(httpd, device.link, echo=echo)
    return ctx, device


def wire(session, device: MockDevice) -> None:
    """把设备回包接到会话入口（等价于真机上 link.on_json -> session.feed）。"""
    device.feed_sink = session.feed
