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
        # §80 S3/S4：OFFER nonce 计数器（严格递增，跨进程/重启单调）
        self._conn.execute(
            "CREATE TABLE IF NOT EXISTS kv(k TEXT PRIMARY KEY, v INTEGER)")
        # ★ 老库迁移：history 补 force / min_version 两列（§80 S4 起才有这两个语义）。
        #   sqlite 的 ADD COLUMN 幂等失败码是 OperationalError（duplicate column），
        #   这里吞掉即可，新库第一次建表后也会走一遍。
        for ddl in ("ALTER TABLE history ADD COLUMN force INTEGER",
                    "ALTER TABLE history ADD COLUMN min_version TEXT"):
            try:
                self._conn.execute(ddl)
            except sqlite3.OperationalError:
                pass
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
    def add_history(self, device_ip: str, from_ver: str, fw, ok: bool, detail: str, *,
                    force: bool = False, min_version: str = "") -> None:
        with self._lock:
            self._conn.execute(
                "INSERT INTO history VALUES(?,?,?,?,?,?,?,?,?,?)",
                (time.time(), device_ip, from_ver, fw.version, fw.size,
                 fw.sha256, 1 if ok else 0, detail,
                 1 if force else 0, min_version or ""))
            self._conn.commit()

    # ---- §80 S3：OFFER nonce（OTA 会话线程调用） ----
    def next_nonce(self) -> int:
        """返回严格递增的 nonce 并落库。

        设备端把"最近一次通过鉴权的 nonce"存 NVS，收到 `nonce <= last` 直接
        REJECT replay —— 所以这里必须**持久化**（进程重启后不能回到 0），
        并且至少是毫秒时间戳量级（换一台 PC 也不会撞号）。
        """
        with self._lock:
            row = self._conn.execute("SELECT v FROM kv WHERE k='nonce'").fetchone()
            last = int(row[0]) if row else 0
            n = max(int(time.time() * 1000), last + 1)
            self._conn.execute("INSERT OR REPLACE INTO kv VALUES('nonce', ?)", (n,))
            self._conn.commit()
            return n

    # ---- 查询（Qt 线程调用） ----
    def recent(self, n: int = 100) -> list[tuple]:
        with self._lock:
            return list(self._conn.execute(
                "SELECT ts, device_ip, from_ver, to_ver, size, sha, ok, detail, "
                "force, min_version "
                "FROM history ORDER BY ts DESC LIMIT ?", (n,)))

    def close(self) -> None:
        with self._lock:
            self._conn.close()
