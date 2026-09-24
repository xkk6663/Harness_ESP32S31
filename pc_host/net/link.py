"""Heartbeat link manager.  Callbacks never run on a GUI thread.

v2.1 (2026-09-23): 发现阶段叠加“/24 网段单播扫描”，支持手动指定 IP + 成功 IP 记忆。

实测（2026-09-23，10.129.105.0/24 WiFi）：对设备单播心跳 → ACK 秒回；全局广播
(255.255.255.255) 与子网定向广播发得出去、设备端（lwIP INADDR_ANY:50003）也确实
会回，但 Windows 防火墙把 ACK 拦了 —— 出站*单播* UDP 会建立软状态放行“回复”，
*广播*流没有这个状态，ACK 被当作不请自来的入站包丢弃（v1/C# 版都用固定 IP 单播，
从未暴露此坑）。单播扫描 = 逐个向网段 254 个地址发一个小心跳包，每个 ACK 都走
单播软状态 ⇒ 免防火墙配置、免管理员权限；AP 隔离也只挡广播/多播，不挡单播。

发现优先级：
  1. --device / 系统页手动指定（trusted，永不自动放弃）
  2. 上次成功 IP 记忆 pc_host/.device_ip（15s 无 ACK 自动转全量发现）
  3. 广播 + /24 单播扫描（SCAN_INTERVAL 一轮）
State is polled by the UI (no cross-thread Qt signals).
"""
from __future__ import annotations

import json
import socket
import threading
import time
from collections.abc import Callable
from pathlib import Path

SIGNAL_PORT = 50003
HEARTBEAT_INTERVAL = 0.2
OFFLINE_AFTER = 5.0
SCAN_INTERVAL = 3.0                  # 发现阶段：/24 单播扫描周期
MEMO_GRACE = 15.0                    # 记忆 IP 无 ACK 的容忍期，过后转全量发现
MEMO_FILE = Path(__file__).resolve().parents[1] / ".device_ip"


def _local_ipv4s() -> list[str]:
    """本机 IPv4 列表（排除环回）。首个 = 默认路由出口 IP（connect-trick），其余为多网卡补充。"""
    ordered: list[str] = []
    try:  # 默认路由出口 IP（最可靠，置顶）
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 9))
        ordered.append(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    try:  # 多网卡补充
        for ip in socket.gethostbyname_ex(socket.gethostname())[2]:
            if not ip.startswith("127.") and ip not in ordered:
                ordered.append(ip)
    except OSError:
        pass
    return ordered


def _scan_addrs() -> list[str]:
    """发现目标：默认路由出口网段的 x.y.z.1-254（排除本机自身）。

    设备与本机必然在同一个 WiFi/LAN 网段（默认路由出口所在网段），
    没必要扫 VMware 等虚拟网卡网段（实测 3 网段 759 地址，白扫还扰民）。
    """
    mine = set(_local_ipv4s())
    if not mine:
        return []
    primary = _local_ipv4s()[0]          # _local_ipv4s 首个 = connect-trick 默认路由 IP
    base = ".".join(primary.split(".")[:3])
    addrs = [f"{base}.{i}" for i in range(1, 255) if f"{base}.{i}" not in mine]
    return addrs


def _bcast_addrs() -> list[str]:
    """广播目标：全网广播 + 各本机网段定向广播。"""
    targets = ["255.255.255.255"]
    targets += [".".join(ip.split(".")[:3]) + ".255" for ip in _local_ipv4s()]
    return sorted(set(targets))


class NetLink:
    def __init__(self, device_ip: str | None, on_state: Callable[[bool, dict], None] | None = None,
                 on_json: Callable[[dict], None] | None = None) -> None:
        # --device 显式指定 → trusted；否则尝试读上次成功 IP 记忆
        self.device_ip = device_ip
        self.device_ip_manual = device_ip is not None
        if device_ip is None and MEMO_FILE.exists():
            try:
                memo = MEMO_FILE.read_text(encoding="ascii").strip()
                if memo:
                    self.device_ip = memo          # 非 trusted：无 ACK 会自动转发现
            except OSError:
                pass
        self.on_state = on_state
        # 设备 → PC 的回程 JSON（NOW_CMD 媒体控制等）。设备端 link_manager_send_json
        # 固定发往 PC:50003，本 socket 必须 bind 50003 才收得到（start() 里做）。
        self.on_json = on_json
        self.online = False
        self.info: dict = {}
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._sock: socket.socket | None = None

    # ---- 对外 ----
    def set_device_ip(self, ip: str | None) -> None:
        """系统页手动指定。ip=None 表示回到自动发现。线程安全（GIL 原子赋值）。"""
        self.device_ip_manual = ip is not None
        self.device_ip = ip
        self.online = False
        if ip:
            self._save_memo(ip)
        elif MEMO_FILE.exists():
            try:
                MEMO_FILE.unlink()
            except OSError:
                pass

    def start(self) -> None:
        if self._thread:
            return
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.settimeout(0.25)
        try:  # 设备回程（NOW_CMD）固定发 PC:50003；占用失败只降级回程，心跳照常
            self._sock.bind(("", SIGNAL_PORT))
        except OSError as e:
            print(f"[link] bind {SIGNAL_PORT} failed: {e}（设备回程命令不可用）", flush=True)
        self._thread = threading.Thread(target=self._run, name="heartbeat", daemon=True)
        self._thread.start()

    def close(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(1.5)
            self._thread = None
        if self._sock:
            self._sock.close()
            self._sock = None

    def send(self, message: dict) -> None:
        if self._sock and self.device_ip:
            self._sock.sendto(json.dumps(message, separators=(",", ":")).encode(),
                              (self.device_ip, SIGNAL_PORT))

    # ---- 内部 ----
    @staticmethod
    def _save_memo(ip: str) -> None:
        try:
            MEMO_FILE.write_text(ip, encoding="ascii")
        except OSError:
            pass

    def _set_state(self, online: bool, info: dict | None = None) -> None:
        if self.online == online and not info:
            return
        self.online = online
        if info:
            self.info = info
        if self.on_state:
            self.on_state(online, self.info)

    def _run(self) -> None:
        assert self._sock
        last_ack = 0.0
        last_send = 0.0
        last_scan = 0.0
        ip_set_at = time.monotonic()
        scan_addrs = _scan_addrs()
        bcast_addrs = _bcast_addrs()
        mode = "auto-discover" if self.device_ip is None else \
            f"{self.device_ip} ({'manual' if self.device_ip_manual else 'memo'})"
        print(f"[link] start device={mode} (scan {len(scan_addrs)} addrs / {SCAN_INTERVAL:.0f}s)", flush=True)
        while not self._stop.is_set():
            now = time.monotonic()
            if now - last_send >= HEARTBEAT_INTERVAL:
                hb = {"type": "HEARTBEAT", "ver": 1, "ts": int(now * 1000),
                      "sw_ver": "0.2", "listen_ports": [50000, 50002, 50005]}
                pkt = json.dumps(hb, separators=(",", ":")).encode()
                try:
                    if self.device_ip is None:
                        # 发现阶段：广播（快） + /24 单播扫描（可靠，绕防火墙）
                        try:
                            self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
                        except OSError:
                            pass
                        for t in bcast_addrs:
                            try:
                                self._sock.sendto(pkt, (t, SIGNAL_PORT))
                            except OSError:
                                pass
                        if now - last_scan >= SCAN_INTERVAL:
                            for t in scan_addrs:
                                try:
                                    self._sock.sendto(pkt, (t, SIGNAL_PORT))
                                except OSError:
                                    pass
                            last_scan = now
                    else:
                        self._sock.sendto(pkt, (self.device_ip, SIGNAL_PORT))
                    last_send = now
                except OSError:
                    pass
            try:
                data, addr = self._sock.recvfrom(1024)
                message = json.loads(data)
                if message.get("type") == "HEARTBEAT_ACK":
                    if self.device_ip != addr[0] and not self.device_ip_manual:
                        self.device_ip = addr[0]
                        ip_set_at = now
                        self._save_memo(addr[0])
                        print(f"[link] discovered device at {self.device_ip}", flush=True)
                    last_ack = now
                    self._set_state(True, message)
                elif self.on_json is not None:
                    try:
                        self.on_json(message)
                    except Exception as e:
                        print(f"[link] on_json error: {e}", flush=True)
            except (TimeoutError, socket.timeout):
                pass
            except (OSError, json.JSONDecodeError):
                pass
            if self.online and time.monotonic() - last_ack > OFFLINE_AFTER:
                self._set_state(False)
            # 记忆 IP / 学到的 IP 长时间无 ACK（设备换网了）→ 退回全量发现
            if (not self.device_ip_manual and self.device_ip and not self.online
                    and now - last_ack > OFFLINE_AFTER and now - ip_set_at > MEMO_GRACE):
                print("[link] IP 无响应，转自动发现（广播+网段扫描）", flush=True)
                self.device_ip = None
                ip_set_at = now
