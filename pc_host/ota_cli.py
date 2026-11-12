"""C3 无界面 OTA 客户端：把「点按钮升级」变成「跑命令 + 看退出码」。

退出码（与验收手册的判据一一对应，供 pytest / CI 直接消费）：
    0  升级成功（设备以新版本回来并通过版本核对）
    1  会话异常 / 用户取消
    2  设备未上线（同 WiFi？防火墙 UDP 50003？）
    3  设备拒绝（6 条校验链中的某一条）
    4  升级后校验失败（坏固件 / 版本不一致 / 自证超时回滚 / 下载停滞）
    5  输入问题（bin 解析失败 / 参数非法）
    6  版本未提升（设备已是更高版本，按"跳过"而非"失败"处理）

用法：
    # 脱机自检（无真机，跑 MockDevice 固件替身）
    python -m pc_host.ota_cli --bin build/app.bin --offline

    # 真机
    python -m pc_host.ota_cli --bin build/app.bin --device 192.168.1.88
    # 故障注入（等价于 UI 上的三个测试勾选）
    python -m pc_host.ota_cli --bin build/app.bin --device 192.168.1.88 --limit 40
    python -m pc_host.ota_cli --bin build/app.bin --device 192.168.1.88 --corrupt
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

from .ota.fw_info import FirmwareInfo
from .ota.session import (
    DOWNLOAD_TIMEOUT_S,
    VERIFY_TIMEOUT_S,
    WAIT_ONLINE_S,
    OtaSession,
)

EXIT_OK = 0
EXIT_ERROR = 1
EXIT_OFFLINE = 2
EXIT_REJECTED = 3
EXIT_VERIFY_FAILED = 4
EXIT_BAD_INPUT = 5
EXIT_NOT_NEWER = 6


def classify(ok: bool | None, detail: str) -> int:
    """把会话结论映射成稳定退出码（顺序即优先级）。"""
    if ok is True:
        return EXIT_OK
    if ok is None:
        return EXIT_ERROR
    d = detail or ""
    if "未上线" in d:
        return EXIT_OFFLINE
    if "版本未提升" in d:
        return EXIT_NOT_NEWER
    if "拒绝" in d or "REJECT" in d:
        return EXIT_REJECTED
    if any(k in d for k in ("version-mismatch", "回滚", "升级失败", "SHA", "sha", "不一致", "停滞")):
        return EXIT_VERIFY_FAILED
    return EXIT_ERROR


def build_ctx(device_ip: str | None, echo: bool):
    """真实模式：NetLink + NowHttpServer（两者都不依赖 Qt）。"""
    from .net.link import NetLink
    from .pcnow.http_server import NowHttpServer
    from .ota.harness import NullCtx

    httpd = NowHttpServer()
    if not httpd.start():
        raise RuntimeError("httpd 启动失败（端口被占用？）")
    link = NetLink(device_ip)
    link.start()
    return NullCtx(httpd, link, echo=echo), httpd, link


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="pc_host.ota_cli", description="无界面 OTA（退出码即判据）")
    ap.add_argument("--bin", required=True, help="app bin 路径（含 esp_app_desc_t）")
    ap.add_argument("--device", default=None, help="设备 IP；缺省=UDP 50003 广播自动发现")
    ap.add_argument("--limit", type=int, default=0, help="只发 N%%（断点续传/中断测试）")
    ap.add_argument("--corrupt", action="store_true", help="流中翻 1 bit（坏固件测试）")
    ap.add_argument("--force", action="store_true", help="允许降级/同版本（设备端仍会判 min_version）")
    ap.add_argument("--psk", default=None,
                    help="OFFER 的 HMAC 预共享密钥；缺省读环境变量 OTA_PSK，再缺省用开发默认值")
    ap.add_argument("--min-version", default="", metavar="VER",
                    help="OFFER 里申报的最低可接受版本（设备 ver<min_version ⇒ REJECT oldver）")
    ap.add_argument("--offline", action="store_true", help="脱机模式：用固件行为替身跑状态机")
    ap.add_argument("--offline-reject", default=None, metavar="REASON",
                    help="脱机模式下让替身拒绝 OFFER（busy/pending/space/network）")
    ap.add_argument("--offline-back-ver", default=None, metavar="VER",
                    help="脱机模式下设备重启后上报的版本（不等于目标=模拟回滚）")
    ap.add_argument("--no-progress-report", action="store_true",
                    help="脱机模式下设备不回报进度（模拟下载停滞）")
    ap.add_argument("--echo", action="store_true", help="实时打印会话日志")
    ap.add_argument("--json-out", default=None, help="把结论写成 JSON（CI 采集用）")
    ap.add_argument("--timeout", type=float, default=None, help="会话整体超时秒数")
    args = ap.parse_args(argv)

    if not 0 <= args.limit <= 99:
        print("--limit 必须在 0~99", file=sys.stderr)
        return EXIT_BAD_INPUT

    fw = FirmwareInfo.parse(args.bin)
    if fw is None:
        return EXIT_BAD_INPUT
    print(f"[ota-cli] 固件 {fw.version} ({fw.project_name}) {fw.size} B sha={fw.sha256[:12]}", flush=True)

    httpd = link = None
    device = None
    if args.offline:
        from .ota.harness import MockDevice, build_offline
        device = MockDevice(reject=args.offline_reject,
                            progress=not args.no_progress_report,
                            back_ver=args.offline_back_ver)
        ctx, _ = build_offline(device, echo=args.echo)
        ctx.httpd.port = 18080
    else:
        try:
            ctx, httpd, link = build_ctx(args.device, args.echo)
        except Exception as e:  # noqa: BLE001
            print(f"[ota-cli] 上下文初始化失败: {type(e).__name__}: {e}", file=sys.stderr)
            return EXIT_ERROR

    session = OtaSession(ctx, fw, limit_percent=args.limit, corrupt=args.corrupt,
                         force=args.force, psk=args.psk, min_version=args.min_version)
    if device is not None:
        from .ota.harness import wire
        wire(session, device)
    if link is not None:
        # 真机模式：设备回包（OTA_ACCEPT / OTA_PROGRESS / OTA_RESULT）固定发 PC:50003，
        # 必须把 link.on_json 接到会话 —— GUI 侧由 main.py 的分发器做这件事，
        # CLI 之前漏接，会话永远收不到 ACCEPT（2026-11-12 验收实测）。
        link.on_json = session.feed
    t0 = time.monotonic()
    session.start()

    timeout = args.timeout
    if timeout is None:
        timeout = WAIT_ONLINE_S + DOWNLOAD_TIMEOUT_S + VERIFY_TIMEOUT_S + 10
    session._thread.join(timeout)

    elapsed = time.monotonic() - t0
    events = ctx.drain_events()
    detail = next((str(e.get("detail", "")) for e in reversed(events) if e.get("ev") == "result"), "")
    code = classify(session.result_ok, detail)
    if session._thread.is_alive():
        session.stop("cli_timeout")
        code = EXIT_ERROR
    summary = {
        "firmware": {"version": fw.version, "size": fw.size, "sha256": fw.sha256, "limit_percent": args.limit},
        "state": session.state,
        "result_ok": session.result_ok,
        "served_bytes": session.served_bytes,
        "serve_count": session.serve_count,
        "corrupt": args.corrupt,
        "force": args.force,
        "min_version": args.min_version,
        "elapsed_s": round(elapsed, 2),
        "exit_code": code,
        "logs": ctx.logs,
        "events": events,
    }

    if link is not None:
        link.close()
    if httpd is not None:
        httpd.stop()

    if args.json_out:
        Path(args.json_out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json_out).write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")

    print(f"[ota-cli] state={session.state} result_ok={session.result_ok} "
          f"served={session.served_bytes} B 用时={elapsed:.1f}s → exit={code}", flush=True)
    return code


if __name__ == "__main__":
    sys.exit(main())
