"""固件 bin 元信息解析：esp_app_desc_t @ 偏移 0x20（C# OtaMode.cs 同口径移植）。

布局：esp_image_header(24B) + esp_image_segment_header(8B) = 32B 之后是
esp_app_desc_t：magic(0xABCD5432) @+0、version[32] @+16、project_name[32] @+48、
time[16] @+80、date[16] @+96、idf_ver[32] @+112、app_elf_sha256[32] @+144。

★ 版本管理第 3 层（2026-11-12，见 docs/项目规划/固件版本管理.md）：
  version  是"人决定"的 SemVer（version.txt），日常构建后面跟 +g<sha>；
  idf_ver  是编它的 IDF 版本；
  elf_sha256 是 IDF 自动写进描述符的 **ELF** 的 SHA-256 ——
  它比 commit 更精确（同 commit 但 sdkconfig 不同会得到不同 ELF），
  用来把"设备上报的 fw_ver"反查到唯一一个 bin（DB 里按 sha256 归档）。
"""
from __future__ import annotations

import hashlib
from dataclasses import dataclass

APP_DESC_MAGIC = 0xABCD5432
DESC_OFFSET = 0x20

OFF_VERSION = 16
OFF_PROJECT = 48
OFF_TIME = 80
OFF_DATE = 96
OFF_IDF_VER = 112
OFF_ELF_SHA = 144


@dataclass(frozen=True)
class FirmwareInfo:
    path: str
    version: str
    project_name: str
    build_time: str
    size: int
    sha256: str
    date: str = ""
    idf_ver: str = ""
    elf_sha256: str = ""
    commit: str = ""

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
        ver = _cstr(data, DESC_OFFSET + OFF_VERSION, 32)
        return cls(
            path=path,
            version=ver,
            project_name=_cstr(data, DESC_OFFSET + OFF_PROJECT, 32),
            build_time=_cstr(data, DESC_OFFSET + OFF_TIME, 16),
            size=len(data),
            sha256=hashlib.sha256(data).hexdigest(),
            date=_cstr(data, DESC_OFFSET + OFF_DATE, 16),
            idf_ver=_cstr(data, DESC_OFFSET + OFF_IDF_VER, 32),
            elf_sha256=bytes(
                data[DESC_OFFSET + OFF_ELF_SHA:DESC_OFFSET + OFF_ELF_SHA + 32]
            ).hex(),
            commit=_commit_of(ver),
        )


def _commit_of(version: str) -> str:
    """从 `2.1.0+g708d4ea[.dirty]` 里取出 `708d4ea`；不是这个格式就返回空串。"""
    if "+" not in version:
        return ""
    meta = version.split("+", 1)[1]
    return meta.split(".", 1)[0].lstrip("gG")


def _cstr(b: bytes, off: int, maxlen: int) -> str:
    end = off
    while end < off + maxlen and end < len(b) and b[end] != 0:
        end += 1
    return b[off:end].decode("ascii", errors="replace").strip()
