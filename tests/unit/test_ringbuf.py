"""C4 单元测试：把 ringbuf 拉到 host 上编译并执行（无需 ESP-IDF、无需板子）。

为什么值得：环形缓冲是音频/日志/串口链路的地基，出错表现是"偶发丢帧"，
在真机上极难复现；搬到 host 上跑毫秒级用例，就把这类问题变成必现。
"""
import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SRC_DIR = ROOT / "main" / "3rd" / "ringbuf"
CASE = ROOT / "tests" / "unit" / "c" / "test_ringbuf.c"
CC = shutil.which("gcc") or shutil.which("cc")

pytestmark = pytest.mark.skipif(CC is None, reason="host 端无 C 编译器（gcc/cc）")


def test_ringbuf_host_unit(tmp_path):
    exe = tmp_path / ("ringbuf_test.exe" if os.name == "nt" else "ringbuf_test")
    build = subprocess.run(
        [CC, "-std=c99", "-Wall", f"-I{SRC_DIR}", str(CASE), str(SRC_DIR / "ringbuf.c"), "-o", str(exe)],
        capture_output=True, text=True,
    )
    assert build.returncode == 0, f"编译失败：\n{build.stderr}"

    run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    assert run.returncode == 0, f"C 用例失败：\n{run.stdout}\n{run.stderr}"
    assert "ALL PASS" in run.stdout
