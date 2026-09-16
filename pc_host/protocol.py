"""ICD v1.1 constants and framing helpers shared by every PC service."""
from __future__ import annotations
import struct
from dataclasses import dataclass

SAMPLE_RATE = 16_000
CHANNELS = 1
BITS = 16
FRAME_MS = 20
PCM_BYTES = 640
MAGIC = b"\xA5\x5A"
VERSION = 1
HEADER = struct.Struct("<2sBBHIH")
CHUNK = struct.Struct("<IIH")

@dataclass(frozen=True)
class VoiceFrame:
    seq: int
    timestamp_ms: int
    pcm: bytes

def pack_voice_frame(seq: int, timestamp_ms: int, pcm: bytes) -> bytes:
    if len(pcm) != PCM_BYTES:
        raise ValueError(f"PCM frame must be {PCM_BYTES} bytes")
    return HEADER.pack(MAGIC, VERSION, 0, seq & 0xFFFF, timestamp_ms & 0xFFFFFFFF, len(pcm)) + pcm

def unpack_voice_frame(packet: bytes) -> VoiceFrame:
    if len(packet) < HEADER.size:
        raise ValueError("truncated audio header")
    magic, version, typ, seq, timestamp_ms, size = HEADER.unpack_from(packet)
    if magic != MAGIC or version != VERSION or typ != 0 or size != PCM_BYTES or len(packet) != HEADER.size + size:
        raise ValueError("invalid audio frame")
    return VoiceFrame(seq, timestamp_ms, packet[HEADER.size:])

def crc16_ccitt(payload: bytes) -> int:
    crc = 0xFFFF
    for value in payload:
        crc ^= value << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc

def wav_header(pcm_bytes: int) -> bytes:
    return struct.pack("<4sI4s4sIHHIIHH4sI", b"RIFF", pcm_bytes + 36, b"WAVE", b"fmt ", 16,
                       1, CHANNELS, SAMPLE_RATE, SAMPLE_RATE * 2, 2, BITS, b"data", pcm_bytes)
