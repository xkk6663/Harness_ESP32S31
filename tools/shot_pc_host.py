"""无头冒烟测试 + 页面截图（QT offscreen）。用法：
  python tools_pcnow/shot_pc_host.py   # 任意工作目录，输出到 .workbuddy/pc_host_shots/
"""
import os
import sys

os.environ["QT_QPA_PLATFORM"] = "offscreen"
ROOT = r"C:\Users\xiao1\Desktop\esp32\display_audio_photo"
sys.path.insert(0, ROOT)
OUT = r"C:\Users\xiao1\Desktop\esp32\.workbuddy\pc_host_shots"
os.makedirs(OUT, exist_ok=True)

import argparse  # noqa: E402

sys.argv = ["shot", "--recordings", os.path.join(ROOT, "pc_host", "recordings")]
from pc_host.main import Ctx, build_services, teardown  # noqa: E402
from pc_host.ui.theme import QSS  # noqa: E402


def main() -> int:
    args = argparse.Namespace(device=None, recordings=os.path.join(ROOT, "pc_host", "recordings"),
                              no_pcnow=False)
    ctx = build_services(args)

    from PySide6.QtWidgets import QApplication
    app = QApplication([])
    app.setStyleSheet(QSS)
    from pc_host.ui.main_window import MainWindow
    win = MainWindow(ctx)
    win.resize(1020, 660)
    win.show()
    app.processEvents()
    import time
    time.sleep(2.5)          # 等 SMTC/HTTP 线程就绪，徽标与日志反映真实状态
    app.processEvents()

    # 模拟一些状态让截图有内容
    ctx.rx_level["rms"] = 42
    ctx.rx_level["count"] = 137
    ctx.pcnow_log("SMTC init ok（示例日志）")
    ctx.pcnow_log("解析: 方大同 - Love Song (hash 7bb9ba6d)")
    ctx.pcnow_log("lyrics: 32 lines | cover: 5306B")
    if ctx.pcnow_sink:
        for line in list(ctx.pcnow_log_lines):
            ctx.pcnow_sink(line)

    names = ["1_call", "2_record", "3_pcnow", "4_system"]
    for i, name in enumerate(names):
        win.switch(i)
        page = win.stack.widget(i)
        if hasattr(page, "_poll"):
            page._poll()
        app.processEvents()
        pix = win.grab()
        pix.save(os.path.join(OUT, name + ".png"), "PNG")
        print("saved", name, flush=True)

    teardown(ctx)
    app.processEvents()
    print("SMOKE OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
