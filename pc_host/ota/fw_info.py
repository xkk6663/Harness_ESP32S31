"""固件 bin 元信息解析：esp_app_desc_t @ 偏移 0x20（C# OtaMode.cs 同口径移植）。

布局：esp_image_header(24B) + esp_image_segment_header(8B) = 32B 之后是
esp_app_desc_t：magic(0xABCD5432) @+0、version[32] @+16、project_name[32] @+48、
time[16] @+80。
"""
from __future__ import annotations

import hashlib
from dataclasses import dataclass

APP_DESC_MAGIC = 0xABCD5432
DESC_OFFSET = 0x20


@dataclass(frozen=True)
class FirmwareInfo:
    path: str
    version: str
    project_name: str
    build_time: str
    size: int
    sha256: str

    @classmethod
    def parse(cls, path: str) -> "FirmwareInfo | None":
        """解析失败返回 None（挡住误选 bootloader.bin / partition-table.bin）。"""
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError as e:
            print(f"[ota] 打不开 {path}: {e}", flush=True)
            return None
        if len(data) < DESC_OFFSET + 256:
            print(f"[ota] 文件太小（{len(data)} B），不是 app bin", flush=True)
            return None
        magic = int.from_bytes(data[DESC_OFFSET:DESC_OFFSET + 4], "little")
        if magic != APP_DESC_MAGIC:
            print(f"[ota] magic 不对（0x{magic:08X} != 0x{APP_DESC_MAGIC:08X}）——"
                  "这不是 app bin（是不是误选了 bootloader.bin / partition-table.bin？）",
                  flush=True)
            return None
        return cls(
            path=path,
            version=_cstr(data, DESC_OFFSET + 16, 32),
            project_name=_cstr(data, DESC_OFFSET + 48, 32),
            build_time=_cstr(data, DESC_OFFSET + 80, 16),
            size=len(data),
            sha256=hashlib.sha256(data).hexdigest(),
        )


def _cstr(b: bytes, off: int, maxlen: int) -> str:
    end = off
    while end < off + maxlen and end < len(b) and b[end] != 0:
        end += 1
    return b[off:end].decode("ascii", errors="replace").strip()
