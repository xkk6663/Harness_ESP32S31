"""§80 S2/S3/S4 用例：OFFER 鉴权 / 版本门禁 / 无 sha 拒绝（脱机，无需真机）。

对应验收矩阵（docs/项目规划 的 OTA 优化方案 §5）：
  T4  坏包        → 已在 tests/hil/test_ota_offline.py（corrupt）
  T5  无 sha      → REJECT nosha
  T6  错 PSK      → REJECT badauth
  T7  重放 nonce  → REJECT replay
  T8  降级        → REJECT downgrade；force=1 可刷
  T9  min_version → REJECT oldver

★ 这些用例走的是"发布形态那条路径"：OtaSession 真发 OFFER → MockDevice 按
  设备端 ota_service.c 的判定顺序回 REJECT，不是 mock 出来的假分支。
"""
import pytest

from pc_host.ota import auth
from pc_host.ota.fw_info import FirmwareInfo
from pc_host.ota.harness import MockDevice, build_offline, wire
from pc_host.ota.session import OtaSession
from pc_host.ota_cli import EXIT_OK, EXIT_REJECTED, classify
from tests.hil.fwfactory import write_app_bin

TIMEOUT_S = 30


@pytest.fixture()
def fw(tmp_path) -> FirmwareInfo:
    info = FirmwareInfo.parse(write_app_bin(tmp_path / "app.bin", version="1.1.0"))
    assert info is not None
    return info


def run(device: MockDevice, fw: FirmwareInfo, **kw):
    ctx, _ = build_offline(device)
    session = OtaSession(ctx, fw, **kw)
    wire(session, device)
    session.start()
    session._thread.join(timeout=TIMEOUT_S)
    assert not session._thread.is_alive(), "会话线程未在超时内结束（可能死等）"
    detail = next((str(e.get("detail", "")) for e in reversed(ctx.drain_events())
                   if e.get("ev") == "result"), "")
    return session, detail


# ---------------------------------------------------------------- T6 鉴权
def test_t6_wrong_psk_is_rejected(fw):
    """设备 PSK 与 PC 不一致 → badauth，且设备端不接受（不进入下载）。"""
    device = MockDevice(dev_ver="1.0.0")            # 设备用开发默认 PSK
    session, detail = run(device, fw, psk="wrong-psk")
    assert session.result_ok is False
    assert "badauth" in detail
    assert device.rejects and device.rejects[0] == "badauth"
    assert device.accepts == 0                      # 关键：一次都没接受
    assert classify(False, detail) == EXIT_REJECTED


def test_t6b_correct_psk_still_works(fw):
    device = MockDevice(dev_ver="1.0.0")
    session, _ = run(device, fw, psk=auth.DEFAULT_PSK)
    assert session.result_ok is True
    assert device.accepts == 1


def test_offer_carries_all_auth_fields(fw):
    """OFFER 必须同时带 min_version/force/nonce/ts/hmac，缺一个设备就拒。"""
    device = MockDevice(dev_ver="1.0.0")
    run(device, fw, min_version="1.0.0")
    assert device.offers, "应当发出 OFFER"
    o = device.offers[0]
    for k in ("ver", "size", "sha256", "url", "min_version", "force", "nonce", "ts", "hmac"):
        assert k in o, f"OFFER 缺字段 {k}"
    assert o["force"] == 0
    assert o["min_version"] == "1.0.0"
    assert len(o["hmac"]) == 64


# ---------------------------------------------------------------- 工具
def _offer(*, ver="1.1.0", size=1024, sha256="aa" * 32, min_version="", force=0,
           nonce=1740000000001, ts=1740000000, hmac=True):
    """按 ICD §8.1 手工造一条 OFFER（用于绕过 OtaSession 直接打设备端判定链）。"""
    url = "http://127.0.0.1/firmware.bin"
    msg = {"type": "OTA_OFFER", "ver": ver, "size": size, "sha256": sha256,
           "url": url, "min_version": min_version, "force": force,
           "nonce": nonce, "ts": ts}
    if hmac:
        canon = auth.canonical_offer(ver, min_version, bool(force), size, sha256,
                                     url, nonce, ts)
        msg["hmac"] = auth.sign(auth.resolve_psk(), canon)
    return msg


def _first_reply(dev: MockDevice, msg: dict, timeout_s: float = 2.0) -> dict:
    """把 OFFER 丢给设备替身并发等第一条回包（_simulate 跑在子线程）。"""
    import time
    box: list[dict] = []
    dev.feed_sink = box.append
    dev.on_link_message(dict(msg))
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline and not box:
        time.sleep(0.02)
    dev.feed_sink = None
    return box[0] if box else {}


# ---------------------------------------------------------------- T5 无 sha
def test_t5_offer_without_sha_is_rejected():
    """设备端：hmac 合法但 sha256 缺失 → nosha（S2 的前置条件）。"""
    dev = MockDevice(dev_ver="1.0.0")
    r = _first_reply(dev, _offer(sha256=""))
    assert r.get("type") == "OTA_REJECT"
    assert r.get("reason") == "nosha"


def test_t5b_offer_without_hmac_is_rejected():
    """鉴权优先于一切：连 hmac 都没有的 OFFER 直接 noauth（早于状态检查）。"""
    dev = MockDevice(dev_ver="1.0.0")
    r = _first_reply(dev, _offer(hmac=False))
    assert r.get("type") == "OTA_REJECT"
    assert r.get("reason") == "noauth"


# ---------------------------------------------------------------- T7 重放
def test_t7_replayed_nonce_is_rejected():
    """同一个 OFFER（同 nonce）发两次 → 第二次 replay。"""
    dev = MockDevice(dev_ver="1.0.0")
    msg = _offer()
    first = _first_reply(dev, msg)
    second = _first_reply(dev, msg)
    assert first.get("type") == "OTA_ACCEPT"
    assert second.get("type") == "OTA_REJECT"
    assert second.get("reason") == "replay"


# ---------------------------------------------------------------- T8 降级
def test_t8_downgrade_rejected_without_force():
    """设备端独立拦降级：PC 侧就算绕过了"版本未提升"软拦，OFFER 不带 force 也刷不进去。"""
    dev = MockDevice(dev_ver="2.0.0")              # 设备已 2.0.0，OFFER 是 1.1.0
    r = _first_reply(dev, _offer(ver="1.1.0", force=0))
    assert r.get("type") == "OTA_REJECT"
    assert r.get("reason") == "downgrade"
    assert dev.accepts == 0


def test_t8b_force_in_offer_allows_downgrade(fw):
    device = MockDevice(dev_ver="2.0.0")
    session, _ = run(device, fw, force=True)
    # session 的 force 会同时写进 OFFER（force=1），设备端据此放行
    assert device.offers and device.offers[0]["force"] == 1
    # 老版本刷入后"版本核对"会失败（回滚语义），这里只验设备端接受
    assert device.accepts == 1


# ---------------------------------------------------------------- T9 min_version
def test_t9_below_min_version_rejected(fw):
    device = MockDevice(dev_ver="1.0.0")
    session, detail = run(device, fw, min_version="9.9.9")   # 目标 1.1.0 < 9.9.9
    assert session.result_ok is False
    assert device.accepts == 0
    assert device.rejects and device.rejects[0] == "oldver"
    assert classify(False, detail) == EXIT_REJECTED


def test_happy_path_unchanged(fw):
    """S2/S3/S4 全开也不能挡住正常升级。"""
    device = MockDevice(dev_ver="1.0.0")
    session, _ = run(device, fw, min_version="1.0.0")
    assert session.result_ok is True
    assert classify(True, "updated") == EXIT_OK
