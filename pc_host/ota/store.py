"""M4 固件归档 + sqlite 升级历史（pc_host v0.5.0）。

两类数据都落在 pc_host/firmware_store/ 下：
  - firmware_store/<ver>_<sha8>.bin   固件归档（按 sha 去重，同 bin 只存一份）
  - firmware_store/ota.db             sqlite：firmwares（归档索引）+ history（升级历史）

线程纪律：archive()/add_history() 会被 OTA 会话线程调用（F44），
sqlite 连接 check_same_thread=False + 内部锁串行化 —— 会话线程绝不碰 Qt。
"""
from __future__ import annotations

import re
import shutil
import sqlite3
import threading
import time
from pathlib import Path


def _safe_name(s: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]", "_", s) or "unknown"


class FirmwareStore:
    """固件归档 + 升级历史。一个进程一份（ctx.ota_store）。"""

    def __init__(self, root: Path) -> None:
        self.root = root
        root.mkdir(parents=True, exist_ok=True)
        self._lock = threading.Lock()
        self._conn = sqlite3.connect(str(root / "ota.db"), check_same_thread=False)
        self._conn.execute(
            "CREATE TABLE IF NOT EXISTS firmwares("
            "sha TEXT PRIMARY KEY, ver TEXT, path TEXT, size INTEGER,"
            "build_time TEXT, added REAL)")
        self._conn.execute(
            "CREATE TABLE IF NOT EXISTS history("
            "ts REAL, device_ip TEXT, from_ver TEXT, to_ver TEXT,"
            "size INTEGER, sha TEXT, ok INTEGER, detail TEXT)")
        self._conn.commit()

    # ---- 归档（OTA 会话线程调用） ----
    def archive(self, fw) -> str:
        """把固件 bin 归档进 firmware_store/（按 sha256 去重）。@return 归档路径。"""
        with self._lock:
            row = self._conn.execute(
                "SELECT path FROM firmwares WHERE sha=?", (fw.sha256,)).fetchone()
            if row and Path(row[0]).exists():
                return row[0]
            dest = self.root / f"{_safe_name(fw.version)}_{fw.sha256[:8]}.bin"
            if not dest.exists():
                shutil.copyfile(fw.path, dest)
            self._conn.execute(
                "INSERT OR REPLACE INTO firmwares VALUES(?,?,?,?,?,?)",
                (fw.sha256, fw.version, str(dest), fw.size, fw.build_time, time.time()))
            self._conn.commit()
            return str(dest)

    # ---- 升级历史（OTA 会话线程调用） ----
    def add_history(self, device_ip: str, from_ver: str, fw, ok: bool, detail: str) -> None:
        with self._lock:
            self._conn.execute(
                "INSERT INTO history VALUES(?,?,?,?,?,?,?,?)",
                (time.time(), device_ip, from_ver, fw.version, fw.size,
                 fw.sha256, 1 if ok else 0, detail))
            self._conn.commit()

    # ---- 查询（Qt 线程调用） ----
    def recent(self, n: int = 100) -> list[tuple]:
        with self._lock:
            return list(self._conn.execute(
                "SELECT ts, device_ip, from_ver, to_ver, size, sha, ok, detail "
                "FROM history ORDER BY ts DESC LIMIT ?", (n,)))

    def close(self) -> None:
        with self._lock:
            self._conn.close()
