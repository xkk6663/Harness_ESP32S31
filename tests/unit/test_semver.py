"""C4 单元测试：SemVer 粗比对（pc_host/ota/session.py:34）。

只用于"要不要升"的粗判，所以重点测**不该误判**的形态：
v 前缀、-dirty、+build、位数不足、非法串。
"""
import pytest

from pc_host.ota.session import compare_semver


@pytest.mark.parametrize("a,b,expect", [
    ("1.1.0", "1.0.0", 1),
    ("1.0.0", "1.1.0", -1),
    ("1.0.0", "1.0.0", 0),
    ("v1.2.3", "1.2.3", 0),
    ("V1.2.3", "1.2.3", 0),
    ("1.2.3-dirty", "1.2.3", 0),        # 后缀不参与主版本比较
    ("1.2.3+build7", "1.2.3", 0),
    ("1.0", "1.0.0", 0),                # 缺位补 0
    ("1", "1.0.0", 0),
    ("1.10.0", "1.9.0", 1),             # 数值比大小，不是字符串比大小
    ("2.0.0", "10.0.0", -1),
    ("abc", "1.0.0", -1),               # 非法串按 0.0.0 处理
    ("1.x.0", "1.0.0", 0),
])
def test_compare_semver(a, b, expect):
    # 契约只保证"正/负/0"，不保证具体差值 -> 归一化符号再比
    got = compare_semver(a, b)
    assert (got > 0) - (got < 0) == expect


def test_dirty_is_not_newer_than_clean():
    """1.2.3-dirty 不能因为字符串更长就被判"更新"（否则会反复重刷）。"""
    assert compare_semver("1.2.3-dirty", "1.2.3") == 0
    assert compare_semver("1.2.3", "1.2.3-dirty") == 0
