#!/usr/bin/env python3
"""_p4_cut.py —— P4 断电回滚的自动化计时器。

为什么需要它：
    P4 要求"看到 init done: running=X pending_verify=1 之后，按重启时刻数 5~8 秒
    再断掉电源"。人肉数秒抓不准，而且拔电时机只有 30s 窗口里那几秒（自证 tick
    约在 boot+14s 就把 app 标 VALID，错过就只能重跑一轮）。

做法：
    1. 一直读串口并打时间戳；
    2. 等 "OTA OK, rebooting in 1000 ms"；
    3. 等其后第一次 "init done: (... pending_verify=1)" —— 记下这一刻（新 boot 的
       boot+4.3s 左右），再等 --delay 秒（默认 2.0s ⇒ 约等于重启后 6.3s）；
    4. 用 RTS 脉冲复位板子（这是本机唯一能做的"断电"等价物：otadata 状态在 flash
       里，bootloader 判据与真实掉电一致）；继续录 --tail 秒看回滚结果。

用法：
    python _p4_cut.py --port COM9 --secs 90 --delay 2.0 --tail 40 --out log.txt
"""

import argparse
import time

try:
    import serial
except ImportError:
    print("ERROR: pyserial not installed", flush=True)
    raise SystemExit(2)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM9")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--secs", type=float, default=90.0, help="total capture time")
    ap.add_argument("--delay", type=float, default=2.0,
                    help="seconds after 'pending_verify=1' before cutting power")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    s = serial.Serial(args.port, args.baud, timeout=1)
    buf: list[str] = []
    t0 = time.time()
    armed = False
    t_ota_ok = 0.0
    t_cut = 0.0
    done = False

    def emit(tag: str) -> None:
        line = f"@@@@ [{time.time() - t0:7.2f}s] {tag}"
        print(line, flush=True)
        buf.append(line)

    try:
        while time.time() - t0 < args.secs:
            line = s.readline()
            if line:
                txt = line.decode("utf-8", "replace").rstrip()
                print(txt, flush=True)
                buf.append(txt)
                if "rebooting in 1000 ms" in txt:
                    armed = True
                    t_ota_ok = time.time()
                    emit("P4: OTA OK seen -> armed, 等新 boot 的 pending_verify=1")
                elif armed and "pending_verify=1" in txt and not done:
                    done = True
                    emit(f"P4: 新 boot 已进 PENDING_VERIFY，{args.delay}s 后复位"
                         f"（距 OTA OK {time.time() - t_ota_ok:.1f}s）")
                    time.sleep(args.delay)
                    # 断电等价物：DTR/RTS 脉冲（与 esptool hard reset 等效）
                    s.setDTR(False)
                    s.setRTS(True)
                    time.sleep(0.1)
                    s.setRTS(False)
                    s.setDTR(False)
                    t_cut = time.time()
                    emit("P4: 已复位（模拟掉电），继续录回滚结果")
            if done and t_cut and time.time() - t_cut > 40:
                break
    finally:
        s.close()

    print("\n===== CAPTURE END =====", flush=True)
    if args.out:
        with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(buf) + "\n")
        print(f"[capture] wrote {len(buf)} lines -> {args.out}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
