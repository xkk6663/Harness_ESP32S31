"""pc_host OTA 升级模块（WiFi 局域网 OTA，ICD §8 / 方案 §4）。

结构：
  fw_info.py   固件 bin 元信息解析（esp_app_desc_t @0x20）
  session.py   OTA 会话（OFFER → ACCEPT → PROGRESS → RESULT → 自证核对）

★ 协议要点（设备端源码实证，2026-10-07）：
  设备回 OTA_ACCEPT / OTA_PROGRESS / OTA_RESULT 全部走
  link_manager_send_json() → 固定发往「心跳源 IP 的 UDP 50003」
  （link_manager.c:102-112 强制覆写端口）—— 所以本模块不发独立 UDP，
  OFFER 直接走 NetLink 的 50003 socket（link.send），回包由 main.py 的
  link.on_json 分发器喂回 session.feed()。这与 C# agent 用临时 UDP 口
  等 ACCEPT 的写法不同（那个写法在设备端语义下收不到包）。
"""
from .fw_info import FirmwareInfo
from .session import OtaSession, compare_semver

__all__ = ["FirmwareInfo", "OtaSession", "compare_semver"]
