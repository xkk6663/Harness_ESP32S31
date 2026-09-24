#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""fake_device.py — 假音箱：验证 pc_now_agent 推流协议（M0 联调用）。

行为：
  - 绑 UDP :50003，收到 HEARTBEAT 打印来源 IP 并回 HEARTBEAT_ACK（模拟设备在线）；
  - 绑 UDP :50004，打印收到的 NOW_META / NOW_PROG / NOW_LRC；
  - 可选 --http：向 PC 的 :50005 拉一次 /lyrics.json 与 /cover 验证 HTTP 通道。

用法：python fake_device.py [--http]
"""
import socket, json, sys, argparse

SIG_PORT = 50003
NOW_PORT = 50004

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--http", action="store_true", help="收到 META 后尝试 HTTP 拉取")
    args = ap.parse_args()

    sig = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sig.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sig.bind(("0.0.0.0", SIG_PORT))
    now = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    now.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    now.bind(("0.0.0.0", NOW_PORT))
    sig.settimeout(0.2)
    now.settimeout(0.2)
    print(f"fake device up: udp/{SIG_PORT} (sig) + udp/{NOW_PORT} (now)")

    peer_ip = None
    last_meta = None
    while True:
        try:
            data, addr = sig.recvfrom(4096)
            s = data.decode("utf-8", "replace")
            if "HEARTBEAT" in s and "ACK" not in s:
                if peer_ip != addr[0]:
                    print(f"[sig] HEARTBEAT from {addr[0]} -> ACK")
                    peer_ip = addr[0]
                ack = json.dumps({"type": "HEARTBEAT_ACK", "ver": 1, "proto_ver": 1,
                                  "sample_rate": 16000, "fw_ver": "fake"})
                sig.sendto(ack.encode(), addr)
            else:
                print(f"[sig] {s[:120]}")
        except socket.timeout:
            pass
        try:
            data, addr = now.recvfrom(4096)
            s = data.decode("utf-8", "replace")
            try:
                obj = json.loads(s)
            except json.JSONDecodeError:
                obj = {"raw": s[:80]}
            t = obj.get("type", "?")
            if t == "NOW_META":
                if obj.get("title") != (last_meta or {}).get("title"):
                    print(f"[now] META: {obj.get('artist')} - {obj.get('title')} "
                          f"({obj.get('duration_ms')}ms, lrc={obj.get('lrc')}, cover={obj.get('cover')})")
                    last_meta = obj
                    if args.http and peer_ip:
                        http_probe(peer_ip, obj)
            elif t == "NOW_PROG":
                print(f"\r[now] PROG {obj.get('pos_ms')}ms playing={obj.get('playing')}   ",
                      end="", flush=True)
            elif t == "NOW_LRC":
                print(f"\n[now] LRC: {obj.get('text')}")
        except socket.timeout:
            pass

def http_probe(ip, meta):
    import urllib.request
    k = meta.get("lrc")
    c = meta.get("cover")
    if k:
        try:
            body = urllib.request.urlopen(f"http://{ip}:50005/lyrics.json?k={k}", timeout=4).read()
            lines = json.loads(body).get("lines", [])
            print(f"[http] lyrics.json: {len(lines)} lines, first: {lines[0] if lines else '-'}")
        except Exception as e:
            print(f"[http] lyrics.json FAIL: {e}")
    if c:
        try:
            body = urllib.request.urlopen(f"http://{ip}:50005/cover/{c}.jpg", timeout=4).read()
            print(f"[http] cover/{c}.jpg: {len(body)} bytes")
        except Exception as e:
            print(f"[http] cover FAIL: {e}")

if __name__ == "__main__":
    main()
