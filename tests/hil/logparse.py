"""C1 串口日志事件化：把 ESP-IDF 文本日志解析为可断言的结构化事件。

设计约束（三条）：
  1. 只读不写 —— 不改固件、不改日志格式；解析失败的行**丢弃而不抛异常**（真机日志一定有噪声）；
  2. 确定性 —— 同一份日志解析两次结果完全一致（无时间依赖、无字典序依赖）；
  3. 事件锚点取自源码里的格式串（main.c / ota_service.c），不靠"我记得长什么样"。

用法：
    events = parse_lines(text)
    assert_event(events, "ota_offer_accept", slot="ota_1")
    rejects = [e for e in events if e["event"] == "ota_reject"]
"""
from __future__ import annotations

import re
from typing import Iterable

LINE_RE = re.compile(r"^([IWEDV]) \(\d+\) ([^:]+): (.+)$")

# 崩溃/复位特征（出现即判"非正常结束"，用于启动类用例的兜底断言）
CRASH_MARKERS = (
    "Guru Meditation Error",
    "Backtrace:",
    "assert failed",
    "abort() was called",
    "StoreProhibited",
    "LoadProhibited",
    "InstrFetchProhibited",
    "Unhandled exception",
    "task_wdt: Task watchdog got triggered",
    "Interrupt wdt timeout",
    "Cache error",
    "Stack canary watchpoint",
)

_OTA_PATTERNS = (
    ("ota_offer_accept", re.compile(r"all 6 checks passed -> target slot (\S+) \((\d+) B free\)")),
    ("ota_reject", re.compile(r"REJECT reason=(\S+)")),
    ("ota_check", re.compile(r"check#(\d+) (\S.*)$")),
    ("ota_offer_received", re.compile(r"OFFER received: ver=(\S+) size=(\d+) sha=(\S+)")),
    ("ota_progress", re.compile(r"progress (\d+)% \((\d+) B, (\d+) KB/s\)")),
    ("ota_valid", re.compile(r"self-test OK: app marked VALID \(rollback cancelled\), run=(\S+)")),
    ("ota_selftest_expired", re.compile(r"self-test window expired")),
    ("ota_abort_received", re.compile(r"OTA_ABORT received")),
    ("ota_switch", re.compile(r"boot slot -> (\S+) \(image ver=(\S+)\)")),
    ("ota_init", re.compile(r"init done: running=(\S+) next_update=(\S+) pending_verify=(\d+)")),
    ("ota_switch_refused", re.compile(r"switch_slot: target (\S+) (?:image invalid|state=\d+)")),
)

_RESUME_PATTERNS = (
    ("ota_resume_found", re.compile(r"found: total=(\d+) done=(\d+) seg_done=(\d+) sha=(\S+)")),
    ("ota_resume_verify", re.compile(r"verify seg (\d+)/(\d+) OK")),
    ("ota_resume_cleared", re.compile(r"\[OTA_RESUME\] cleared")),
    ("ota_resume_none", re.compile(r"no valid resume record")),
)

BOOT_RE = re.compile(
    r"\[boot-diag\] (?P<stage>.+?)\s+internal\s+free=(?P<int_free>\d+) largest=(?P<int_largest>\d+)"
    r"\s+\|\s+DMA free=(?P<dma_free>\d+)\s+\|\s+PSRAM free=(?P<psram_free>\d+)"
)
HWM_RE = re.compile(r"\[stack-hwm\] (?P<task>\S+) high-water = (?P<words>\d+) words \((?P<bytes>\d+) bytes\)")
OTA_MEM_RE = re.compile(r"\[OTA_MEM\] (?P<what>.+?)\s+INT free=(?P<int_free>\d+) largest=(?P<int_largest>\d+)")
OTA_ERR_RE = re.compile(r"\[OTA_ERR\] (?P<msg>.+)$")
SWITCH_REFUSED_RE = re.compile(r"switch_slot: target (\S+) (?:image invalid|state=\d+)")


def parse_line(line: str) -> dict | None:
    """解析单行；非目标日志返回 None。"""
    line = line.rstrip("\r\n")
    if not line:
        return None
    for marker in CRASH_MARKERS:
        if marker in line:
            return {"event": "crash", "marker": marker, "raw": line}

    m = LINE_RE.match(line)
    if not m:
        return None
    level, tag, msg = m.group(1), m.group(2).strip(), m.group(3)
    base = {"level": level, "tag": tag}

    b = BOOT_RE.search(msg)
    if b:
        d = b.groupdict()
        return {
            "event": "boot",
            "stage": d["stage"].strip(),
            "int_free": int(d["int_free"]),
            "int_largest": int(d["int_largest"]),
            "dma_free": int(d["dma_free"]),
            "psram_free": int(d["psram_free"]),
            **base,
        }

    h = HWM_RE.search(msg)
    if h:
        d = h.groupdict()
        return {"event": "stack_hwm", "task": d["task"], "words": int(d["words"]),
                "bytes": int(d["bytes"]), **base}

    if "self-test window expired" in msg:
        return {"event": "ota_selftest_expired", **base}

    mm = SWITCH_REFUSED_RE.search(msg)
    if mm:
        return {"event": "ota_switch_refused", "target": mm.group(1), **base}

    if msg.startswith("[OTA_DIAG]"):
        body = msg[len("[OTA_DIAG] "):] if msg.startswith("[OTA_DIAG] ") else msg
        for name, pat in _OTA_PATTERNS:
            mm = pat.search(body)
            if not mm:
                continue
            g = mm.groups()
            if name == "ota_offer_accept":
                return {"event": name, "slot": g[0], "free": int(g[1]), **base}
            if name == "ota_reject":
                return {"event": name, "reason": g[0], **base}
            if name == "ota_check":
                return {"event": name, "index": int(g[0]), "detail": g[1], **base}
            if name == "ota_offer_received":
                return {"event": name, "ver": g[0], "size": int(g[1]), "sha": g[2], **base}
            if name == "ota_progress":
                return {"event": name, "percent": int(g[0]), "bytes": int(g[1]),
                        "kbps": int(g[2]), **base}
            if name == "ota_valid":
                return {"event": name, "run": g[0], **base}
            if name == "ota_switch":
                return {"event": name, "slot": g[0], "ver": g[1], **base}
            if name == "ota_init":
                return {"event": name, "running": g[0], "next_update": g[1],
                        "pending_verify": int(g[2]), **base}
            if name == "ota_switch_refused":
                return {"event": name, "target": g[0], **base}
            return {"event": name, **base}

    if msg.startswith("[OTA_RESUME]"):
        for name, pat in _RESUME_PATTERNS:
            mm = pat.search(msg)
            if mm:
                g = mm.groups()
                if name == "ota_resume_found":
                    return {"event": name, "total": int(g[0]), "done": int(g[1]),
                            "seg_done": int(g[2]), "sha": g[3], **base}
                if name == "ota_resume_verify":
                    return {"event": name, "seg": int(g[0]), "seg_done": int(g[1]), **base}
                return {"event": name, **base}

    mm = OTA_MEM_RE.search(msg)
    if mm:
        d = mm.groupdict()
        return {"event": "ota_mem", "what": d["what"].strip(),
                "int_free": int(d["int_free"]), "int_largest": int(d["int_largest"]), **base}

    mm = OTA_ERR_RE.search(msg)
    if mm:
        return {"event": "ota_err", "msg": mm.group("msg"), **base}

    if "boot done, waiting swipe up" in msg:
        return {"event": "boot_done", **base}
    return None


def parse_lines(lines: Iterable[str]) -> list[dict]:
    out = []
    for line in lines:
        ev = parse_line(line)
        if ev is not None:
            out.append(ev)
    return out


def event_names(events: list[dict]) -> list[str]:
    return [e["event"] for e in events]


def find(events: list[dict], name: str, **match) -> dict | None:
    for e in events:
        if e["event"] != name:
            continue
        if all(e.get(k) == v for k, v in match.items()):
            return e
    return None


def assert_event(events: list[dict], name: str, **match) -> dict:
    """用例里用：找不到就带上下文失败（便于看日志定位）。"""
    ev = find(events, name, **match)
    assert ev is not None, f"未找到事件 {name} {match}；已解析事件：{event_names(events)}"
    return ev


def boot_stages(events: list[dict]) -> list[str]:
    return [e["stage"] for e in events if e["event"] == "boot"]


def has_crash(events: list[dict]) -> bool:
    return any(e["event"] == "crash" for e in events)
