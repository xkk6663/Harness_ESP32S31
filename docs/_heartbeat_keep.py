#!/usr/bin/env python
"""验收用：只发 HEARTBEAT 的保活器。

为什么需要它：设备端自证（mark_valid）的条件是 "WiFi 已连 + link 在线"。
ota_cli 收到 RESULT 就退出，link 随即离线，设备在 10s 自证 tick 时看不到 PC
→ 30s 窗口过期 → 一直停在 PENDING_VERIFY（下次重启就回滚）。
本脚本在 CLI 退出后继续发心跳，把自证窗口撑过去。

用法：python docs/_heartbeat_keep.py <device_ip> <秒数>
"""
import json
import socket
import sys
import time

ip = sys.argv[1]
secs = int(sys.argv[2]) if len(sys.argv) > 2 else 120
port = 50003

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
t0 = time.monotonic()
n = 0
while time.monotonic() - t0 < secs:
    hb = {"type": "HEARTBEAT", "ver": 1, "ts": int(time.monotonic() * 1000),
          "sw_ver": "0.2", "listen_ports": [50000, 50002, 50005]}
    try:
        sock.sendto(json.dumps(hb, separators=(",", ":")).encode(), (ip, port))
        n += 1
    except OSError as e:
        print(f"[keep] send failed: {e}", flush=True)
    time.sleep(1.0)
print(f"[keep] done, sent {n} heartbeats to {ip}:{port}", flush=True)
