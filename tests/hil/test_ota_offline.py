"""C5 用例：OTA 状态机脱机全覆盖（固件行为替身驱动，无需真机）。

用例 ID 与验收手册一一对应：
  TC-OTA-A1 正常闭环          | 手册 A 组（all 6 checks passed / marked VALID）
  TC-OTA-D  校验链拒绝        | 手册 D 组（REJECT reason=busy）
  TC-OTA-E  limit 断点语义    | 手册 E 组（只发 40%）
  TC-OTA-F  坏固件            | 手册 F 组（corrupt 翻 1 bit）
  TC-OTA-H  自证失败=回滚     | 手册 H 组（回来版本不一致）
  TC-OTA-K  设备未上线        | 前置条件失败要给出可行动的错误
  TC-OTA-L  下载中掉线        | 逻辑：8s 链断 -> 发 OTA_ABORT
  TC-OTA-M  下载停滞          | 逻辑：只在"无进度"时超时（慢而活着不算死）
"""
import io

import pytest

import pc_host.ota.session as S
from pc_host.ota.fw_info import FirmwareInfo
from pc_host.ota.harness import MockDevice, build_offline, wire
from pc_host.ota.session import OtaSession
from pc_host.ota_cli import EXIT_NOT_NEWER, EXIT_OFFLINE, EXIT_OK, EXIT_REJECTED, \
    EXIT_VERIFY_FAILED, classify
from tests.hil.fwfactory import write_app_bin

TIMEOUT_S = 30


@pytest.fixture()
def fw(tmp_path) -> FirmwareInfo:
    info = FirmwareInfo.parse(write_app_bin(tmp_path / "app.bin", version="1.1.0"))
    assert info is not None
    return info


def run(device: MockDevice, fw: FirmwareInfo, *, limit=0, corrupt=False, force=False):
    ctx, _ = build_offline(device)
    session = OtaSession(ctx, fw, limit_percent=limit, corrupt=corrupt, force=force)
    wire(session, device)              # 设备回包 -> session.feed（等价真机链路）
    session.start()
    session._thread.join(timeout=TIMEOUT_S)
    assert not session._thread.is_alive(), "会话线程未在超时内结束（可能死等）"
    return session, ctx


def detail_of(ctx) -> str:
    return next((str(e.get("detail", "")) for e in reversed(ctx.drain_events())
                 if e.get("ev") == "result"), "")


# ---------------------------------------------------------------- 正常闭环
def test_tc_ota_a1_happy_path(fw):
    device = MockDevice(dev_ver="1.0.0")
    session, ctx = run(device, fw)

    assert session.result_ok is True
    assert session.state == "done"
    assert device.accepts == 1
    assert "版本核对通过" in "\n".join(ctx.logs)
    states = [e["state"] for e in ctx.drain_events() if e.get("ev") == "state"]
    assert states[:4] == ["wait_online", "offer", "download", "verify"]
    # 就绪才宣告：会话结束必须摘掉 httpd 路由
    assert ctx.httpd.attach_count == 1 and ctx.httpd.detach_count == 1


def test_progress_events_are_forwarded_to_ui(fw):
    device = MockDevice(progress_step_ms=2)
    _, ctx = run(device, fw)
    prog = [e for e in ctx.drain_events() if e.get("ev") == "progress"]
    assert prog and prog[-1]["percent"] == 100
    assert [p["percent"] for p in prog] == sorted(p["percent"] for p in prog)


# ---------------------------------------------------------------- 校验链拒绝
@pytest.mark.parametrize("reason,expect_exit", [
    ("busy", EXIT_REJECTED),
    ("pending", EXIT_REJECTED),
    ("space", EXIT_REJECTED),
    ("network", EXIT_REJECTED),
])
def test_tc_ota_d_reject(fw, reason, expect_exit):
    session, ctx = run(MockDevice(reject=reason), fw)
    d = detail_of(ctx)
    assert session.result_ok is False
    assert reason in d
    assert classify(False, d) == expect_exit


# ---------------------------------------------------------------- 版本比对
def test_tc_ota_b_not_newer_is_skipped_not_failed(fw, monkeypatch):
    device = MockDevice(dev_ver="1.5.0")
    session, ctx = run(device, fw)
    assert session.result_ok is False
    d = detail_of(ctx)
    assert "版本未提升" in d
    assert device.offers == []            # 关键：连 OFFER 都不该发
    assert classify(False, d) == EXIT_NOT_NEWER


def test_tc_ota_b2_force_bypasses_version_check(fw):
    device = MockDevice(dev_ver="1.5.0")
    session, ctx = run(device, fw, force=True)
    assert session.result_ok is True
    assert device.offers, "--force 时应当发出 OFFER"


# ---------------------------------------------------------------- 前置条件
def test_tc_ota_k_device_offline_reports_actionable_error(fw, monkeypatch):
    monkeypatch.setattr(S, "WAIT_ONLINE_S", 1)
    session, ctx = run(MockDevice(online_at_start=False), fw)
    d = detail_of(ctx)
    assert session.result_ok is False
    assert "未上线" in d and "50003" in d
    assert classify(False, d) == EXIT_OFFLINE


# ---------------------------------------------------------------- 掉线 / 停滞
def test_tc_ota_l_peer_lost_sends_abort(fw, monkeypatch):
    monkeypatch.setattr(S, "PEER_LOST_ABORT_S", 1)
    device = MockDevice(drop_link_after_accept=True)
    session, _ = run(device, fw)
    assert session.result_ok is None       # 取消语义，不是失败
    assert device.aborts, "链断超过阈值必须向设备发 OTA_ABORT"
    assert device.aborts[0]["reason"] == "peer_lost"


def test_tc_ota_m_stalled_download_times_out(fw, monkeypatch):
    monkeypatch.setattr(S, "DOWNLOAD_IDLE_TIMEOUT_S", 1)
    session, ctx = run(MockDevice(progress=False), fw)
    d = detail_of(ctx)
    assert session.result_ok is False
    assert "停滞" in d
    assert classify(False, d) == EXIT_VERIFY_FAILED


# ---------------------------------------------------------------- 自证 / 回滚
def test_tc_ota_h_version_mismatch_means_rollback(fw):
    session, ctx = run(MockDevice(back_ver="1.0.0"), fw)
    d = detail_of(ctx)
    assert session.result_ok is False
    assert "version-mismatch" in d
    assert classify(False, d) == EXIT_VERIFY_FAILED


def test_tc_ota_h2_never_came_back(fw, monkeypatch):
    monkeypatch.setattr(S, "VERIFY_TIMEOUT_S", 3)
    session, ctx = run(MockDevice(reboot=False), fw)
    d = detail_of(ctx)
    assert session.result_ok is False
    assert "回滚" in d or "未恢复心跳" in d


# ---------------------------------------------------------------- 故障注入：limit / corrupt
def test_tc_ota_e_limit_only_serves_percent(fw):
    ctx, _ = build_offline(MockDevice())
    session = OtaSession(ctx, fw, limit_percent=40)

    assert session.served_size == fw.size * 40 // 100
    assert b'"limit_percent": 40' in session.firmware_json()

    out = io.BytesIO()
    session.serve_firmware(out, 0, False)
    assert len(out.getvalue()) == fw.size * 40 // 100
    assert session.served_bytes == fw.size * 40 // 100


def test_tc_ota_e2_resume_offset_is_honored(fw):
    """设备带 Range 重推时，服务端从偏移继续发（断点续传的另一半）。"""
    ctx, _ = build_offline(MockDevice())
    session = OtaSession(ctx, fw, limit_percent=40)
    total = fw.size * 40 // 100
    out = io.BytesIO()
    session.serve_firmware(out, 262144, False)
    assert len(out.getvalue()) == total - 262144


def test_tc_ota_f_corrupt_flips_exactly_one_bit(fw):
    ctx, _ = build_offline(MockDevice())
    session = OtaSession(ctx, fw, corrupt=True)
    out = io.BytesIO()
    session.serve_firmware(out, 0, False)
    data = out.getvalue()

    with open(fw.path, "rb") as f:
        raw = f.read()
    diff_bits = 0
    for a, b in zip(raw, data):
        diff_bits += bin(a ^ b).count("1")
    assert len(data) == len(raw)
    assert diff_bits == 1, f"应当只翻 1 bit，实际 {diff_bits} bit"
    assert "CORRUPT" in "\n".join(ctx.logs)


def test_head_request_does_not_send_body(fw):
    ctx, _ = build_offline(MockDevice())
    session = OtaSession(ctx, fw)
    out = io.BytesIO()
    session.serve_firmware(out, 0, True)
    assert out.getvalue() == b""


# ---------------------------------------------------------------- 退出码契约
@pytest.mark.parametrize("ok,detail,code", [
    (True, "updated", EXIT_OK),
    (False, "设备未上线（20s 超时）", EXIT_OFFLINE),
    (False, "设备拒绝 OTA_OFFER: busy", EXIT_REJECTED),
    (False, "version-mismatch", EXIT_VERIFY_FAILED),
    (False, "下载停滞（120s 无 PROGRESS）→ 判定失败", EXIT_VERIFY_FAILED),
    (False, "版本未提升（1.5.0 >= 1.1.0）", EXIT_NOT_NEWER),
    (None, "cancelled", 1),
])
def test_exit_code_contract(ok, detail, code):
    assert classify(ok, detail) == code
