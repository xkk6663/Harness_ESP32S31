"""§80 S3：OTA_OFFER 的 HMAC-SHA256 签名 / 校验（PC 侧唯一实现）。

设备端对应实现：`main/middleware/network/ota_service.c` 的
`build_canon()` + `ota_auth_check()`。两侧的**规范化串与 PSK 字面量必须一字不差**，
否则设备回 `OTA_REJECT{reason:"badauth"}`。

为什么要有这一层：原来 OFFER 的六道检查全是"状态检查"（在线/录音/会话/并发/
待自证/容量），没有任何身份校验 —— 同网段任意主机发一个 url 指向自己的 bin，
只要 size 和镜像 CRC 过得去，设备就会刷。HMAC 把门槛从"同网段任何人"抬到
"持有 PSK 的人"。

诚实边界：未开 Flash Encryption 时 PSK 能从设备 flash 里读出来。所以它是
"最划算的一档"，不是终点；根治靠量产的 Secure Boot v2 + Flash Encryption。
"""
from __future__ import annotations

import hashlib
import hmac
import os
import threading
import time

# ★ 与 ota_service.c 的 OTA_DEV_PSK 逐字一致（tests/unit/test_ota_auth.py 会读 C 源码断言）。
#   量产应改为产线写入设备 NVS "ota"/"psk"，PC 侧用 --psk 或环境变量 OTA_PSK 指定。
DEFAULT_PSK = "harness-dev-psk-2026-11"
PSK_ENV = "OTA_PSK"

# PC 侧时钟允许比设备快多少秒由设备端判（OTA_AUTH_SKEW_S=300）；这里只负责把 ts 发准。
_nonce_lock = threading.Lock()
_nonce_last = 0


def canonical_offer(ver, min_version, force, size, sha256, url, nonce, ts) -> str:
    """规范化签名串（ICD §8.1 固化）：

        ver|min_version|force|size|sha256|url|nonce|ts

    - `force` 归一成 0/1（C 侧 `%d` 打 int）
    - `size` / `nonce` / `ts` 一律十进制无符号整数（C 侧 `%lld` / `%llu`）
    - `min_version` 缺省时是空串（两侧都用空串，不是 "-"、不是省略）
    """
    return "|".join([
        str(ver or ""),
        str(min_version or ""),
        "1" if force else "0",
        str(int(size)),
        str(sha256 or ""),
        str(url or ""),
        str(int(nonce)),
        str(int(ts)),
    ])


def resolve_psk(explicit: str | None = None) -> bytes:
    """PSK 解析顺序：显式参数 → 环境变量 OTA_PSK → 开发默认值。"""
    if explicit:
        return explicit.encode("utf-8")
    env = os.environ.get(PSK_ENV)
    if env:
        return env.encode("utf-8")
    return DEFAULT_PSK.encode("utf-8")


def sign(psk: bytes, canon: str) -> str:
    """规范化串 → 64 hex HMAC-SHA256。"""
    return hmac.new(psk, canon.encode("utf-8"), hashlib.sha256).hexdigest()


def verify(psk: bytes, canon: str, hexsig: str) -> bool:
    """设备侧行为在 `ota_service.c`；这里给脱机替身（MockDevice）与自测用。"""
    return hmac.compare_digest(sign(psk, canon), str(hexsig or ""))


def make_nonce(store=None) -> int:
    """严格递增的 nonce（毫秒时间戳基准，冲突时 +1）。

    设备把最近一次通过鉴权的 nonce 落 NVS，重放同一个值会被 `REJECT replay`。
    优先用 sqlite 持久化（跨进程/重启单调），无 store 时退化为进程内单调。
    """
    if store is not None:
        try:
            return store.next_nonce()
        except Exception:      # noqa: BLE001 —— 归档/存储故障绝不能挡住升级
            pass
    global _nonce_last
    with _nonce_lock:
        n = max(int(time.time() * 1000), _nonce_last + 1)
        _nonce_last = n
        return n


def build_offer_fields(psk: bytes, *, ver: str, min_version: str, force: bool,
                       size: int, sha256: str, url: str, store=None) -> dict:
    """组装 OFFER 的鉴权/门禁字段（不含 type）。返回可直接并入 offer dict。"""
    nonce = make_nonce(store)
    ts = int(time.time())
    canon = canonical_offer(ver, min_version, force, size, sha256, url, nonce, ts)
    return {
        "min_version": min_version or "",
        "force": 1 if force else 0,
        "nonce": nonce,
        "ts": ts,
        "hmac": sign(psk, canon),
    }
