"""验证 SmtcWatcher 在真实 venv 中可用（3 秒后打印 available）。"""
import sys
import time

sys.path.insert(0, r"C:\Users\xiao1\Desktop\esp32\display_audio_photo")
from pc_host.pcnow.smtc import SmtcWatcher

logs = []
w = SmtcWatcher(on_track=lambda t: logs.append(("track", t)),
                on_position=lambda p, s: None,
                on_log=lambda s: logs.append(("log", s)))
w.start()
time.sleep(3)
w.stop()
out = ["available=%s" % w.available] + ["%s: %s" % x for x in logs]
open(r"C:\Users\xiao1\Desktop\esp32\.workbuddy\tmp_smtc2.txt", "w", encoding="utf-8").write("\n".join(out))
