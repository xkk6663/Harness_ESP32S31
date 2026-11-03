"""C4 单元测试：固件 bin 元信息解析（pc_host/ota/fw_info.py）。

要点：抓"误选 bootloader.bin / partition-table.bin"这类真实操作错误 ——
这是升级失败里最常见、也最容易被忽略的一类"输入缺陷"。
"""
import hashlib

import pytest

from pc_host.ota.fw_info import APP_DESC_MAGIC, DESC_OFFSET, FirmwareInfo
from tests.hil.fwfactory import build_app_bin, write_app_bin


@pytest.fixture()
def app_bin(tmp_path):
    return write_app_bin(tmp_path / "app.bin", version="1.1.0")


def test_parse_ok(app_bin):
    fw = FirmwareInfo.parse(app_bin)
    assert fw is not None
    assert fw.version == "1.1.0"
    assert fw.project_name == "display_audio_photo"
    assert fw.build_time.startswith("Nov")
    assert fw.size == 2_833_280
    assert fw.sha256 == hashlib.sha256(open(app_bin, "rb").read()).hexdigest()


def test_parse_rejects_non_app_bin(tmp_path):
    """magic 不对 => 明确拒绝（不是 app bin）。"""
    p = tmp_path / "partition-table.bin"
    p.write_bytes(build_app_bin(magic=0x12345678))
    assert FirmwareInfo.parse(str(p)) is None


def test_parse_rejects_tiny_file(tmp_path):
    p = tmp_path / "bootloader.bin"
    p.write_bytes(b"\xE9" + bytes(20))
    assert FirmwareInfo.parse(str(p)) is None


def test_parse_rejects_missing_file(tmp_path):
    assert FirmwareInfo.parse(str(tmp_path / "nope.bin")) is None


def test_desc_offset_is_0x20():
    """偏移是协议事实，不能随手改（改了会把版本号读成别的字节）。"""
    assert DESC_OFFSET == 0x20
    assert APP_DESC_MAGIC == 0xABCD5432


def test_sha256_is_content_based(tmp_path):
    """同名同版本、内容不同 -> sha 必须不同（否则"版本核对"就失去意义）。"""
    a = write_app_bin(tmp_path / "a.bin", version="1.1.0", size=2_833_280)
    b = write_app_bin(tmp_path / "b.bin", version="1.1.0", size=2_833_281)
    fa, fb = FirmwareInfo.parse(a), FirmwareInfo.parse(b)
    assert fa.sha256 != fb.sha256
    assert fa.version == fb.version
