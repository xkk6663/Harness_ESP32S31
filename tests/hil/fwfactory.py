"""合成 app bin 夹具：布局严格按 pc_host/ota/fw_info.py 的解析口径。

真实 ESP-IDF app bin 布局：
    esp_image_header(24B) + esp_image_segment_header(8B) = 0x20
    @0x20: esp_app_desc_t —— magic(0xABCD5432)@+0, version[32]@+16,
            project_name[32]@+48, time[16]@+80
"""
from __future__ import annotations

import struct
from pathlib import Path

DESC_OFFSET = 0x20
APP_DESC_MAGIC = 0xABCD5432


def build_app_bin(version: str = "1.1.0", project: str = "display_audio_photo",
                  build_time: str = "Nov  1 2026 22:13:07", size: int = 2_833_280,
                  magic: int = APP_DESC_MAGIC) -> bytes:
    if size < DESC_OFFSET + 256:
        raise ValueError("size 太小，不像 app bin")
    buf = bytearray(size)
    buf[0] = 0xE9                      # esp_image_header.magic
    buf[1] = 1                         # segment_count
    struct.pack_into("<I", buf, DESC_OFFSET, magic)
    _put_cstr(buf, DESC_OFFSET + 16, version, 32)
    _put_cstr(buf, DESC_OFFSET + 48, project, 32)
    _put_cstr(buf, DESC_OFFSET + 80, build_time, 16)
    return bytes(buf)


def _put_cstr(buf: bytearray, off: int, value: str, maxlen: int) -> None:
    raw = value.encode("ascii")[:maxlen]
    buf[off:off + len(raw)] = raw


def write_app_bin(path: Path | str, **kw) -> str:
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(build_app_bin(**kw))
    return str(p)
