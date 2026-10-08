"""Korvo 统一上位机（pc_host v2）：通话对讲 + 录音库 + 音乐副屏 + 固件升级 + 壁纸 + 系统。

用法：
  python -m pc_host.main                 # 广播自动发现设备（推荐）
  python -m pc_host.main --device 192.168.4.1
  python -m pc_host.main --no-pcnow      # 不启动音乐副屏（50004 不占；50005 仍为 OTA 服务）

v0.2（2026-09-23）：UI 重构为侧边栏多页框架（ui/），合并副屏功能（pcnow/），
link 支持广播发现；对讲/录音链路代码不变。
v0.3（2026-10-07）：收编 OTA（pc_host/ota/）——固件升级页 + 50005 /firmware.* 路由；
link.on_json 改为按 type 分发（NOW_CMD → 副屏控制，OTA_* → OTA 会话）。
v0.4（2026-10-07）：收编壁纸上传（wallpaper.py，§76 设备拉）——系统页"壁纸管理"卡片
+ 50005 /wallpaper/<name> 路由 + WP_OFFER/WP_RESULT 信令。
"""
from __future__ import annotations

import argparse
import collections
import queue
import sys
from pathlib import Path

from .net.link import NetLink
from .net.voice_udp import UdpVoice
from .record.server import RecordingServer
from .audio.player import Player
from .audio.wav_player import WavPlayer
from .asr.backend import AsrBackend


class Ctx:
    """服务容器：页面只读轮询，不做跨线程 Qt 信号。"""

    def __init__(self) -> None:
        self.link: NetLink | None = None
        self.voice: UdpVoice | None = None
        self.record_server: RecordingServer | None = None
        self.player: Player | None = None
        self.asr: AsrBackend | None = None
        self.wav: WavPlayer | None = None
        self.smtc = None
        self.pusher = None
        self.store = None
        self.cdp = None
        self.httpd = None
        self.rec_dir = Path("recordings")
        self.rx_level = {"rms": 0, "count": 0}
        self.pcnow_log_lines: collections.deque = collections.deque(maxlen=200)
        # ★ F44 —— 闪退根因：pcnow_log 会被 **SMTC 线程 / pusher 线程 / CDP 线程 /
        #   网易云解析线程** 同时调用。旧实现把它直接接到页面的
        #   QPlainTextEdit.appendPlainText 上 —— 从非 Qt 线程碰 widget 是未定义行为，
        #   典型触发点就是"设备端按下一首"：一次换曲炸出 5~6 条日志（NOW_CMD / SMTC ->
        #   next / track: / 解析: / lyrics: / 进度走 CDP 真值），来自 3 个不同线程，
        #   于是随机闪退。
        #   改为只投队列，页面在 Qt 线程的定时器里排空 —— 与 call_page 的 _asr_q
        #   完全同一套写法（那里本来就是对的）。
        self.pcnow_q: queue.Queue = queue.Queue(maxsize=4000)
        # ---- OTA（v0.3 收编，见 ota/）----
        self.ota_q: queue.Queue = queue.Queue(maxsize=1000)     # 会话事件 → OTA 页排空
        self.ota_session = None                                  # OtaSession | None
        self.on_now_cmd = None                                   # 副屏 NOW_CMD 回调（pcnow 注入）
        # ---- 壁纸上传（v0.4 收编 §76，见 wallpaper.py）----
        self.wp_q: queue.Queue = queue.Queue(maxsize=200)        # 会话事件 → 系统页排空
        self.wp_session = None                                   # WallpaperSession | None
        # ---- M4（v0.5）：固件归档 + sqlite 升级历史 ----
        self.ota_store = None                                    # FirmwareStore | None

    def pcnow_log(self, s: str) -> None:
        line = f"[{__import__('time').strftime('%H:%M:%S')}] {s}"
        print(line, flush=True)
        self.pcnow_log_lines.append(line)
        try:
            self.pcnow_q.put_nowait(line)
        except queue.Full:
            pass                     # 页面未在排空（窗口已关）时丢弃，绝不阻塞工作线程

    def ota_log(self, s: str) -> None:
        line = f"[{__import__('time').strftime('%H:%M:%S')}] {s}"
        print(line, flush=True)
        try:
            self.ota_q.put_nowait({"ev": "log", "line": line})
        except queue.Full:
            pass


def build_services(args) -> Ctx:
    ctx = Ctx()
    ctx.rec_dir = Path(args.recordings)
    ctx.rec_dir.mkdir(parents=True, exist_ok=True)

    # ---- 链路 ----
    ctx.link = NetLink(args.device)
    ctx.link.start()

    # ---- 对讲 / 录音 ----
    ctx.record_server = RecordingServer(ctx.rec_dir)
    ctx.record_server.start()
    ctx.voice = UdpVoice(args.device or "", on_pcm=None)  # device_ip 由 link 学到后更新
    ctx.player = Player()
    try:
        ctx.player.start()
    except Exception as e:
        print(f"[audio] speaker unavailable: {e}", flush=True)
    ctx.asr = AsrBackend(lambda text: None)
    ctx.asr.start()

    def on_pcm(pcm: bytes) -> None:
        assert ctx.player is not None and ctx.asr is not None
        ctx.player.push(pcm)
        ctx.asr.push(pcm)
        import array
        samples = array.array("h", pcm)
        n = len(samples)
        s = (sum(x * x for x in samples) / n) if n else 0
        ctx.rx_level["rms"] = min(100, int((s ** 0.5) / 32768.0 * 1500))
        ctx.rx_level["count"] += 1

    ctx.voice.on_pcm = on_pcm

    def send_ptt(pcm: bytes) -> None:
        # 广播发现模式下设备 IP 由 link 学到 —— 每次发送前同步
        assert ctx.voice is not None
        ip = ctx.link.device_ip if ctx.link else None
        if ip and ctx.voice.device_ip != ip:
            ctx.voice.device_ip = ip
        ctx.voice.send_ptt(pcm)

    ctx.voice.start()
    ctx.wav = WavPlayer(send_ptt)

    # ---- link.on_json 分发器（v0.3）：设备回包全部到 PC:50003，按 type 分流 ----
    # NOW_CMD → 副屏媒体控制；OTA_* → 当前 OTA 会话（若无会话则丢弃并记日志）。
    # ★ 设备端 link_manager_send_json() 强制回包到 PC:50003（link_manager.c:102-112），
    #   所以 OFFER 也必须走这条 50003 socket（link.send）——同端口同源，语义自洽。
    def dispatch_link_json(msg: dict) -> None:
        t = str(msg.get("type", ""))
        if t == "NOW_CMD":
            if ctx.on_now_cmd is not None:
                ctx.on_now_cmd(msg)
        elif t.startswith("OTA_"):
            if ctx.ota_session is not None:
                ctx.ota_session.feed(msg)
            else:
                print(f"[ota] 收到 {t} 但无活跃会话，忽略", flush=True)
        elif t.startswith("WP_"):
            if ctx.wp_session is not None:
                ctx.wp_session.feed(msg)
            else:
                print(f"[wp] 收到 {t} 但无上传会话，忽略", flush=True)

    ctx.link.on_json = dispatch_link_json

    # ---- 音乐副屏（可选；httpd 无论开关都起 —— OTA 数据面复用 50005） ----
    if not args.no_pcnow:
        from .pcnow.netease_cdp import NeteaseCdp
        from .pcnow.pusher import NowPusher, TrackStore
        from .pcnow.smtc import SmtcWatcher

        ctx.store = TrackStore()
        ctx.pusher = NowPusher(device_ip_provider=lambda: ctx.link.device_ip,
                               store=ctx.store, on_log=ctx.pcnow_log)
        ctx.pusher.start()

        # ★ P1（§28.51）：网易云 PC 的 SMTC 时间轴是零值死轴、<audio>.currentTime 恒 0，
        #   位置真值只在它的进度 DOM 文本里（实测 "00:06 / 04:02" → 5s 后 "00:11 / 04:02"）。
        #   前提：网易云带 --remote-debugging-port=9222 启动
        #   （双击 pc_host/restart_cloudmusic_debug.bat）。探测失败时自动降级为
        #   本地内插，不影响任何既有功能。
        ctx.cdp = NeteaseCdp(on_log=ctx.pcnow_log)
        ctx.pusher.attach_cdp(ctx.cdp)
        try:
            ctx.cdp.probe()
        except Exception as e:  # noqa: BLE001
            ctx.pcnow_log(f"CDP 探测异常（忽略）: {type(e).__name__}: {e}")

        ctx.smtc = SmtcWatcher(on_track=ctx.pusher.update_track,
                               on_position=ctx.pusher.update_position,
                               on_log=ctx.pcnow_log)
        ctx.smtc.start()

        # 设备端媒体控制回程：link 50003 收 NOW_CMD → SMTC 会话控制
        def on_now_cmd(msg: dict) -> None:
            if ctx.smtc is None:
                return
            cmd = str(msg.get("cmd", ""))
            pos_ms = msg.get("pos_ms")
            has_pos = isinstance(pos_ms, (int, float))
            ctx.pcnow_log(f"NOW_CMD <- {cmd}"
                          + (f" {pos_ms}ms" if has_pos else ""))

            # ★ P0-1：seek 先分清"这个播放器到底有没有时间轴"，再决定要不要真发 SMTC。
            if cmd == "seek" and has_pos:
                target = max(0.0, float(pos_ms) / 1000.0)
                before = ctx.pusher.anchor(target, source="manual")
                dev = target - before
                if ctx.pusher.has_timeline:
                    ctx.pcnow_log(
                        f"校准(seek): 内插 {before:.1f}s → 目标 {target:.1f}s"
                        f"（偏差 {dev:+.1f}s）｜该播放器有真时间轴，"
                        f"SMTC seek 随后生效并再次校准")
                else:
                    ctx.pcnow_log(
                        f"校准(锚点): 内插 {before:.1f}s → 实际 {target:.1f}s"
                        f"（偏差 {dev:+.1f}s）｜该播放器无 SMTC 时间轴，"
                        f"**跳过** seek（网易云曲内跳转本就无解），仅重锚内插时钟")
                    return          # 不把注定失败的 seek 丢给 SMTC
            ctx.smtc.send_command(cmd,
                                  int(pos_ms) if has_pos else None)

        ctx.on_now_cmd = on_now_cmd

    from .pcnow.http_server import NowHttpServer
    ctx.httpd = NowHttpServer(ctx.store, on_log=ctx.pcnow_log)
    ctx.httpd.start()

    # ---- §76 壁纸上传会话（httpd /wallpaper 路由 + WP_OFFER/RESULT） ----
    from .wallpaper import WallpaperSession
    ctx.wp_session = WallpaperSession(ctx)

    # ---- M4（v0.5）：固件归档 + sqlite 升级历史 ----
    from .ota.store import FirmwareStore
    ctx.ota_store = FirmwareStore(Path(__file__).resolve().parent / "firmware_store")
    return ctx


def teardown(ctx: Ctx) -> None:
    if ctx.ota_session is not None:
        try:
            ctx.ota_session.stop("app_exit")
        except Exception as e:
            print(f"[teardown] ota_session: {e}", flush=True)
    if ctx.ota_store is not None:
        try:
            ctx.ota_store.close()
        except Exception as e:
            print(f"[teardown] ota_store: {e}", flush=True)
    for closer in ("smtc", "httpd", "pusher", "wav", "asr", "player", "voice", "record_server", "link"):
        obj = getattr(ctx, closer, None)
        if obj is not None and hasattr(obj, "close"):
            try:
                obj.close()
            except Exception as e:
                print(f"[teardown] {closer}: {e}", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(prog="pc_host")
    parser.add_argument("--device", default=None, help="设备 IP；缺省=UDP 50003 广播自动发现")
    parser.add_argument("--recordings", default="recordings")
    parser.add_argument("--no-pcnow", action="store_true",
                        help="不启动音乐副屏模块（50004 不占；50005 仍供 OTA 使用）")
    args = parser.parse_args()

    try:
        from PySide6.QtWidgets import QApplication
    except ImportError as exc:
        raise SystemExit("缺少 PySide6：pip install -e .[desktop]") from exc

    from .ui.main_window import MainWindow
    from .ui.theme import QSS

    ctx = build_services(args)

    app = QApplication(sys.argv)
    app.setStyleSheet(QSS)
    window = MainWindow(ctx)
    window.show()
    result = app.exec()
    teardown(ctx)
    return result


if __name__ == "__main__":
    raise SystemExit(main())
