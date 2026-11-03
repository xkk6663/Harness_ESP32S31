"""C1 用例：串口日志事件化解析（tests/hil/logparse.py）。

判据全部来自源码格式串：
  main.c:49      [boot-diag] %-34s internal free=%u largest=%u | DMA free=%u | PSRAM free=%u
  ota_service.c  all 6 checks passed -> target slot %s / REJECT reason=%s / progress %d%% ...
"""
from pathlib import Path

import pytest

from tests.hil import logparse as lp

SAMPLE = Path(__file__).resolve().parents[1] / "hil" / "samples" / "boot_ota_ok.log"


@pytest.fixture(scope="module")
def events():
    return lp.parse_lines(SAMPLE.read_text(encoding="utf-8").splitlines())


def test_boot_stages_are_complete(events):
    """main.c 里 30 个 BOOT_DIAG 埋点必须一个不少（少一个=漏测一个启动依赖）。"""
    stages = lp.boot_stages(events)
    assert len(stages) == 30, f"实际 {len(stages)} 个：{stages}"
    assert stages[0] == "app_main enter"
    assert stages[-1] == "after PageManager_Load(splash)"


def test_boot_has_no_crash(events):
    assert not lp.has_crash(events)


def test_boot_budget_is_enforced(events):
    """启动内存预算：任一阶段 internal 最大连续块不得低于 48 KB（碎片红线）。"""
    boots = [e for e in events if e["event"] == "boot"]
    worst = min(boots, key=lambda e: e["int_largest"])
    assert worst["int_largest"] >= 49152, f"最差阶段 {worst['stage']}={worst['int_largest']}"


def test_ota_offer_accepted_to_expected_slot(events):
    ev = lp.assert_event(events, "ota_offer_accept")
    assert ev["slot"] == "ota_1"
    assert ev["free"] == 5_242_880


def test_ota_progress_monotonic_to_100(events):
    pcts = [e["percent"] for e in events if e["event"] == "ota_progress"]
    assert pcts == sorted(pcts)
    assert pcts[-1] == 100


def test_ota_self_verify_marks_valid(events):
    ev = lp.assert_event(events, "ota_valid")
    assert ev["run"] == "ota_1"


def test_boot_done_seen(events):
    lp.assert_event(events, "boot_done")


def test_parse_is_deterministic():
    """同一份日志两次解析必须完全一致（否则进 CI 会 flake）。"""
    lines = SAMPLE.read_text(encoding="utf-8").splitlines()
    assert lp.parse_lines(lines) == lp.parse_lines(lines)


def test_noise_lines_are_dropped_not_raised():
    noise = [
        "",
        "\x00\x01乱码 not a log line",
        "I (12) MAIN: 这行是中文，没有事件特征",
        "I (12)",                       # 截断
        "I (12) MAIN:",                 # 只有 tag
        "W (99) OTA: [OTA_DIAG]",       # 只有前缀
        "Random bootloader output",
    ]
    assert lp.parse_lines(noise) == []


@pytest.mark.parametrize("reason", ["network", "busy", "pending", "space", "badargs"])
def test_reject_reason_roundtrip(reason):
    line = f"W (1234) OTA: [OTA_DIAG] REJECT reason={reason}"
    ev = lp.parse_line(line)
    assert ev["event"] == "ota_reject" and ev["reason"] == reason


def test_selftest_expired_and_resume_and_switch_refused():
    lines = [
        "W (20400) MAIN: [OTA] self-test window expired (30s) without WiFi+link -> rollback armed",
        "W (3210) OTA: [OTA_RESUME] found: total=2833280 done=1133312 seg_done=4 sha=8f2c1d0e9a7b3c4d",
        "I (3300) OTA: [OTA_RESUME] verify seg 1/4 OK",
        'E (3410) OTA: [OTA_ERR] switch_slot: target ota_1 image invalid, refuse',
        "I (3500) OTA: [OTA_DIAG] init done: running=ota_0 next_update=ota_1 pending_verify=0",
    ]
    evs = lp.parse_lines(lines)
    assert evs[0]["event"] == "ota_selftest_expired"
    assert evs[1]["event"] == "ota_resume_found" and evs[1]["seg_done"] == 4
    assert evs[2]["event"] == "ota_resume_verify" and evs[2]["seg"] == 1
    assert evs[3]["event"] == "ota_switch_refused" and evs[3]["target"] == "ota_1"
    assert evs[4]["event"] == "ota_init" and evs[4]["running"] == "ota_0"


@pytest.mark.parametrize("marker", [
    "Guru Meditation Error: Core  0 panic'ed (LoadProhibited)",
    "assert failed: xQueueSemaphoreTake queue.c:1545 ((xSemaphore)->toBlock)",
    "task_wdt: Task watchdog got triggered",
    "Cache error detected",
])
def test_crash_markers(marker):
    assert lp.parse_line(marker)["event"] == "crash"


def test_crlf_and_trailing_spaces_tolerated():
    line = "I (140) MAIN: [boot-diag] app_main enter  internal free=100 largest=50 | DMA free=10 | PSRAM free=20\r\n"
    ev = lp.parse_line(line)
    assert ev["event"] == "boot" and ev["int_free"] == 100 and ev["stage"] == "app_main enter"
