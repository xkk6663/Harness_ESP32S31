"""PC 音乐副屏：SMTC 采集 + 网易云歌词/封面 + NOW_* 推流 + HTTP 服务。

移植自 tools/pc_now_agent（C#），协议保持 ICD V1.3 一致：
UDP 50004 推 NOW_META/NOW_PROG/NOW_LRC；设备 HTTP 拉取 50005 /cover /lyrics.json。
winsdk 为可选依赖（pip install winsdk），缺失时仅 SMTC 采集不可用，其余正常。
"""
