#!/usr/bin/env python3
"""
_serial_capture.py —— 串口日志抓取（闭环验证用）

为什么需要它：
    `idf.py monitor` 依赖真 TTY，在无交互终端 / 被工具调用的场景下会直接失败。
    本脚本用 pyserial 直接读串口，稳定且可脚本化。

用法：
    python _serial_capture.py                          # 默认 COM9 / 12s / 打到 stdout
    python _serial_capture.py --port COM9 --secs 15 --out docs/_log.txt
    python _serial_capture.py --reset                  # 抓之前先拉 RTS 复位板子

输出：
    - 始终 print 到 stdout（供上层解析）
    - 若给了 --out，同时写文件（UTF-8，换行统一 \n）
"""

import argparse
import sys
import time

try:
    import serial
except ImportError:
    print("ERROR: pyserial not installed. pip install pyserial", file=sys.stderr)
    sys.exit(2)


def main() -> int:
    ap = argparse.ArgumentParser(description="Capture ESP32 serial log")
    ap.add_argument("--port", default="COM9", help="serial port (default COM9)")
    ap.add_argument("--baud", type=int, default=115200, help="baud rate (default 115200)")
    ap.add_argument("--secs", type=float, default=12.0, help="capture duration in seconds")
    ap.add_argument("--out", default=None, help="also write captured lines to this file")
    ap.add_argument("--reset", action="store_true",
                    help="toggle RTS/DTR to hard-reset the board before capture")
    args = ap.parse_args()

    try:
        s = serial.Serial(args.port, args.baud, timeout=1)
    except Exception as exc:  # noqa: BLE001 - surface any open failure to caller
        print(f"ERROR: cannot open {args.port}: {exc}", file=sys.stderr)
        return 2

    if args.reset:
        # ESP32 自动下载电路：DTR/RTS 翻转可硬复位（与 esptool 的 hard reset 等价）
        s.setDTR(False)
        s.setRTS(True)
        time.sleep(0.1)
        s.setRTS(False)
        s.setDTR(False)
        time.sleep(0.1)

    buf = []
    end = time.time() + args.secs
    try:
        while time.time() < end:
            line = s.readline()
            if not line:
                continue
            txt = line.decode("utf-8", "replace").rstrip()
            print(txt, flush=True)
            buf.append(txt)
    finally:
        s.close()

    print("\n===== CAPTURE END =====", flush=True)

    if args.out:
        try:
            with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
                fh.write("\n".join(buf))
                fh.write("\n")
            print(f"[capture] wrote {len(buf)} lines -> {args.out}", flush=True)
        except OSError as exc:
            print(f"ERROR: cannot write {args.out}: {exc}", file=sys.stderr)
            return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
