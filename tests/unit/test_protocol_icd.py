"""C4 单元测试：ICD v1.1 协议层（pc_host/protocol.py）。

覆盖策略（等价类 + 边界值 + 错误推测）：
  有效帧往返 / 长度边界 639-640-641 / 魔数、版本、类型、长度字段篡改 / 截断包 / 多余字节 /
  小端序约定 / WAV 头字段自洽 / 16k×16bit×20ms == 640 B 的交叉验算。
"""
import struct

import pytest

from pc_host.protocol import (
    BITS,
    CHANNELS,
    FRAME_MS,
    HEADER,
    MAGIC,
    PCM_BYTES,
    SAMPLE_RATE,
    VERSION,
    VoiceFrame,
    crc16_ccitt,
    pack_voice_frame,
    unpack_voice_frame,
    wav_header,
)


def test_crc16_check_value():
    """CRC16-CCITT 标准校验向量：'123456789' -> 0x29B1。"""
    assert crc16_ccitt(b"123456789") == 0x29B1
    assert crc16_ccitt(b"") == 0xFFFF


def test_pcm_bytes_matches_audio_math():
    """交叉验算：采样率 × 位深 × 帧长 == 每帧字节数（防止常量各自漂移）。"""
    assert SAMPLE_RATE * (BITS // 8) * CHANNELS * (FRAME_MS / 1000) == PCM_BYTES


def test_frame_roundtrip():
    pcm = bytes(range(256)) * 2 + bytes(128)   # 恰好 640 B
    pkt = pack_voice_frame(0x1234, 0xDEADBEEF, pcm)
    out = unpack_voice_frame(pkt)
    assert out == VoiceFrame(seq=0x1234, timestamp_ms=0xDEADBEEF, pcm=pcm)


def test_frame_total_size():
    assert HEADER.size == 12
    pkt = pack_voice_frame(1, 1000, bytes(PCM_BYTES))
    assert len(pkt) == 12 + 640


def test_frame_is_little_endian():
    pkt = pack_voice_frame(0x0102, 0x03040506, bytes(PCM_BYTES))
    assert pkt[:2] == MAGIC
    assert pkt[2] == VERSION
    assert pkt[3] == 0
    assert pkt[4:6] == b"\x02\x01"          # seq 小端
    assert pkt[6:10] == b"\x06\x05\x04\x03"  # timestamp 小端


@pytest.mark.parametrize("bad_len", [0, 1, 639, 641, 1024])
def test_pack_rejects_wrong_length(bad_len):
    with pytest.raises(ValueError):
        pack_voice_frame(1, 1, bytes(bad_len))


def test_pack_wraps_seq_and_timestamp():
    pkt = pack_voice_frame(0x1_0001, 0x1_0000_0001, bytes(PCM_BYTES))
    assert unpack_voice_frame(pkt).seq == 1
    assert unpack_voice_frame(pkt).timestamp_ms == 1


@pytest.mark.parametrize("packet,why", [
    (b"", "空包"),
    (bytes(11), "短于头长度"),
    (b"\x00\x00" + bytes(10), "魔数错"),
    (MAGIC + bytes([2, 0]) + bytes(8), "版本错"),
    (MAGIC + bytes([VERSION, 1]) + bytes(8), "类型错"),
    (MAGIC + bytes([VERSION, 0]) + struct.pack("<HI", 7, 1000) + struct.pack("<H", 639) + bytes(639), "长度字段错"),
    (pack_voice_frame(1, 1, bytes(PCM_BYTES)) + b"\x00", "多余尾字节"),
])
def test_unpack_rejects_malformed(packet, why):
    with pytest.raises(ValueError):
        unpack_voice_frame(packet)


def test_truncated_header_is_detected():
    pkt = pack_voice_frame(1, 1, bytes(PCM_BYTES))
    with pytest.raises(ValueError):
        unpack_voice_frame(pkt[:10])
    with pytest.raises(ValueError):
        unpack_voice_frame(pkt[:HEADER.size])       # 头齐了但无 PCM


def test_wav_header_self_consistent():
    pcm = 640 * 5
    h = wav_header(pcm)
    assert len(h) == 44
    riff_size = struct.unpack_from("<I", h, 4)[0]
    fmt_size, audio_format, channels, rate, byte_rate, block_align, bits = \
        struct.unpack_from("<IHHIIHH", h, 16)
    data_size = struct.unpack_from("<I", h, 40)[0]
    assert h[:4] == b"RIFF" and h[8:12] == b"WAVE" and h[12:16] == b"fmt " and h[36:40] == b"data"
    assert riff_size == pcm + 36        # 44 字节头里 RIFF 长度 = 文件长度 - 8
    assert fmt_size == 16
    assert (audio_format, channels, rate, bits) == (1, CHANNELS, SAMPLE_RATE, BITS)
    assert byte_rate == SAMPLE_RATE * CHANNELS * BITS // 8
    assert block_align == CHANNELS * BITS // 8
    assert data_size == pcm
