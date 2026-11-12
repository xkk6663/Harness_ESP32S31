#!/usr/bin/env python3
"""§80 S6：构建后固件体积门禁 —— 装不进 OTA 槽就在构建设备上失败，别等设备回 reason=space。

背景（改动记录 §80）：
  ota_0 / ota_1 各 **5MB**（partitions.csv: 0x500000），当前固件已到 4.99MB，
  物理余量只剩 ~8.7KB。设备端 `ota_service.c:handle_offer()` 的判据是
  `if (size > upd->size) send_reject("space")` —— **只看槽的物理大小**，
  不额外预留。超限的后果是 PC 会话本身不失败，只有设备回
  `OTA_REJECT{"reason":"space"}`，构建侧毫无感知 ⇒ "能编过、烧不进"。

★ 两级判定（§80j ⑦，用户决策"内存够就没关系"）：
  ① **物理溢出** size > slot → FAIL / exit 1：确实会 REJECT space，必须拦。
  ② **安全余量不足** size > slot - 64KB 但未溢出 → WARN / exit 0：
     当前仍能 OTA，只是"贴红线"，不该每次构建都红（狼来了）；
     发布或 CI 想严格时加 `--strict` 让它变成 exit 1。
  留 64KB 只是"下一版还能长胖"的预警档，不是设备端的硬约束。

用法：
  python tools/check_fw_size.py                     # 默认找 build/*.bin 里最大的
  python tools/check_fw_size.py build/display_audio_photo.bin
  python tools/check_fw_size.py --strict            # 发布口径：余量不足也失败
  python tools/check_fw_size.py --limit-slot 0x500000 --margin 0x10000
退出码：0 = 通过（含 WARN）；1 = 超限（物理溢出，或 --strict 下余量不足）；
        2 = 找不到 bin。

★ 注意：build/ 下还有 bootloader.bin / partition-table.bin / ota_data_initial.bin /
  storage.bin，所以默认按"最大文件"取 app —— 与 ota_service 的选槽补丁同口径
  （只认 ota_0/ota_1，不碰 uext/storage）。
"""
from __future__ import annotations

import argparse
import pathlib
import sys

SLOT_SIZE_DEFAULT = 0x500000     # 5MB，见 partitions.csv 的 ota_0 / ota_1
MARGIN_DEFAULT = 0x10000         # 64KB 安全余量（仅预警档，非设备硬约束）


def find_app_bin(build_dir: pathlib.Path) -> pathlib.Path | None:
    bins = [p for p in build_dir.glob("*.bin") if p.is_file()]
    if not bins:
        return None
    return max(bins, key=lambda p: p.stat().st_size)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="固件体积门禁（对比 ota_0/ota_1 槽容量）")
    ap.add_argument("bin", nargs="?", help="app bin 路径；缺省取 build/*.bin 中最大的")
    ap.add_argument("--build-dir", default="build", help="构建目录（默认 build）")
    ap.add_argument("--limit-slot", default=hex(SLOT_SIZE_DEFAULT),
                    help="槽大小，支持 0x 前缀（默认 0x500000 = 5MB）")
    ap.add_argument("--margin", default=hex(MARGIN_DEFAULT),
                    help="安全余量预警档（默认 0x10000 = 64KB）")
    ap.add_argument("--strict", action="store_true",
                    help="发布口径：安全余量不足也算失败（exit 1）")
    ap.add_argument("-q", "--quiet", action="store_true", help="只输出一行结论")
    args = ap.parse_args(argv)

    slot = int(str(args.limit_slot), 0)
    margin = int(str(args.margin), 0)
    limit = slot - margin

    if args.bin:
        p = pathlib.Path(args.bin)
        if not p.is_file():
            print(f"FAIL: 找不到 {p}（先 idf.py build）", file=sys.stderr)
            return 2
    else:
        p = find_app_bin(pathlib.Path(args.build_dir))
        if p is None:
            print(f"FAIL: {args.build_dir}/*.bin 不存在（先 idf.py build）", file=sys.stderr)
            return 2

    size = p.stat().st_size
    physical_over = size - slot      # >0：真装不下，设备必回 space
    margin_over = size - limit       # >0：贴红线，但仍可 OTA

    if args.quiet:
        if physical_over > 0:
            tag = "FAIL(overflow)"
        elif margin_over > 0:
            tag = "FAIL(strict)" if args.strict else "WARN(margin)"
        else:
            tag = "OK"
        print(f"{p.name}: {size} B / slot {slot} B / limit {limit} B {tag}")
    else:
        print(f"app bin : {p}")
        print(f"大小    : {size} B ({size / 1048576:.3f} MB)")
        print(f"槽       : {slot} B ({slot / 1048576:.2f} MB)  "
              f"预警档 {margin} B → {limit} B ({limit / 1048576:.3f} MB)")
        print(f"占用    : {size * 100.0 / slot:.1f}% of slot, "
              f"物理剩余 {slot - size} B（安全余量后 {limit - size} B）")

    # ① 物理溢出：硬失败，与设备端 space 判据逐字对应
    if physical_over > 0:
        print(f"FAIL: 超出槽物理大小 {physical_over} B —— "
              f"设备会回 OTA_REJECT reason=space，OTA 直接失败；"
              f"先删素材/裁功能或扩分区表", file=sys.stderr)
        return 1

    # ② 安全余量不足：默认只提示，--strict 下才拦
    if margin_over > 0:
        detail = (f"已吃掉 {margin} B 安全余量的 {margin_over} B "
                  f"（物理剩余 {slot - size} B）→ 本次仍可 OTA，但下一版加功能就会溢出。"
                  f"建议先做 -Os 或裁未引用组件（不要降日志级别："
                  f"timing / whole-sha / progress 都是 INFO）。")
        if args.strict:
            print(f"FAIL(--strict): {detail}", file=sys.stderr)
            return 1
        print(f"WARN: {detail}", file=sys.stderr)
        return 0

    return 0


if __name__ == "__main__":
    sys.exit(main())
