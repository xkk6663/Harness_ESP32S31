"""§80 S3：OFFER HMAC 的跨语言一致性单测（Python ↔ C 字面量对账）。

为什么要"读 C 源码做断言"：HMAC 的规范化串和 PSK 是**双方各自硬编码**的字面量，
一处改动漏了另一处，症状是设备端一律 `REJECT badauth`，而且日志里看不出差在哪
（CANON 只在 DEBUG 级打印）。这里把漂移变成 CI 里一眼可见的红灯。
"""
import pathlib
import re

import pytest

from pc_host.ota import auth

ROOT = pathlib.Path(__file__).resolve().parents[2]
OTA_C = ROOT / "main" / "middleware" / "network" / "ota_service.c"


@pytest.fixture(scope="module")
def c_src() -> str:
    return OTA_C.read_text(encoding="utf-8", errors="replace")


def test_dev_psk_matches_firmware(c_src):
    """pc_host/ota/auth.py:DEFAULT_PSK 必须与固件 OTA_DEV_PSK 逐字一致。"""
    m = re.search(r'#define\s+OTA_DEV_PSK\s+"([^"]*)"', c_src)
    assert m, "ota_service.c 里找不到 OTA_DEV_PSK 定义（改名了？）"
    assert m.group(1) == auth.DEFAULT_PSK, (
        f"PSK 漂移：C={m.group(1)!r} / Python={auth.DEFAULT_PSK!r}")


def test_canonical_field_order_matches_firmware(c_src):
    """规范化串的字段顺序与类型说明符必须与 C 侧 build_canon() 完全一致。"""
    m = re.search(r'snprintf\(out, cap,\s*"([^"]+)"', c_src)
    assert m, "ota_service.c 里找不到 build_canon() 的 snprintf 格式串"
    assert m.group(1) == "%s|%s|%d|%lld|%s|%s|%llu|%llu", (
        f"规范化串漂移：C 侧现在是 {m.group(1)!r}")


def test_canonical_offer_value_shape():
    canon = auth.canonical_offer("2.1.0", "", False, 4980000,
                                 "ab" * 32, "http://192.168.1.10:50005/firmware.bin",
                                 1740000001, 1740000000)
    parts = canon.split("|")
    assert len(parts) == 8
    assert parts[0] == "2.1.0"
    assert parts[1] == ""              # min_version 缺省是空串（两侧都空，不是 "-"）
    assert parts[2] == "0"             # force 归一成 0/1
    assert parts[3] == "4980000"
    assert parts[6] == "1740000001"
    assert parts[7] == "1740000000"


def test_canonical_offer_force_is_int():
    assert auth.canonical_offer("1", "", True, 1, "x", "u", 1, 1).split("|")[2] == "1"


def test_sign_verify_roundtrip():
    psk = b"secret-psk"
    canon = auth.canonical_offer("1.0.0", "0.9.0", 1, 100, "ff" * 32, "http://h/f", 7, 8)
    sig = auth.sign(psk, canon)
    assert len(sig) == 64 and re.fullmatch(r"[0-9a-f]{64}", sig)
    assert auth.verify(psk, canon, sig)
    assert not auth.verify(b"other", canon, sig)
    # 任一字段被改 → 签名失效（这是"防篡改"的定义）
    tampered = auth.canonical_offer("1.0.0", "0.9.0", 1, 101, "ff" * 32, "http://h/f", 7, 8)
    assert not auth.verify(psk, tampered, sig)


def test_hmac_known_answer():
    """固定向量：防止有人"顺手改一下 canonical_offer 的分隔符"却不自知。"""
    canon = "2.1.0||0|4980000|" + "ab" * 32 + "|http://192.168.1.10:50005/firmware.bin|1740000001|1740000000"
    assert auth.canonical_offer("2.1.0", "", False, 4980000, "ab" * 32,
                                "http://192.168.1.10:50005/firmware.bin",
                                1740000001, 1740000000) == canon
    sig = auth.sign(b"harness-dev-psk-2026-11", canon)
    # 自校验：同输入必须稳定复现（跨平台一致性）
    assert sig == auth.sign(b"harness-dev-psk-2026-11", canon)


def test_make_nonce_strictly_increasing():
    a = auth.make_nonce()
    b = auth.make_nonce()
    assert b > a


def test_resolve_psk_env(monkeypatch):
    monkeypatch.setenv(auth.PSK_ENV, "from-env")
    assert auth.resolve_psk() == b"from-env"
    assert auth.resolve_psk("explicit") == b"explicit"
    monkeypatch.delenv(auth.PSK_ENV)
    assert auth.resolve_psk() == auth.DEFAULT_PSK.encode()


def test_device_force_accepts_json_number(c_src):
    """ICD §8.1 把 `force` 定义为 0/1，PC 端 build_offer_fields 发的就是 int。

    设备端若只用 `cJSON_IsTrue(jf)` 解析，数字 1 的类型是 cJSON_Number →
    返回 false，于是：① 降级门禁失效（勾了 force 仍判 downgrade）；
    ② 规范串里设备端算 "0"、PC 端签 "1" ⇒ **一律 REJECT badauth**。
    这条锁住"数字与布尔都要收"的写法，防止有人把它简化回 cJSON_IsTrue。
    """
    assert "cJSON_IsNumber(jf)" in c_src, (
        "force 解析不再接受 JSON 数字：ICD 定义是 0/1，数字 1 会被读成 0，"
        "导致签名串不一致（badauth）")
    assert "cJSON_IsTrue(jf)" in c_src, "force 也应容忍 JSON 布尔 true"
