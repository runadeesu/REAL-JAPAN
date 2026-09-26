#!/usr/bin/env python3
"""Subset BIZ UDPGothic (SIL OFL 1.1, no Reserved Font Name) to what the game can display:
ASCII/Latin-1, punctuation, CJK symbols, kana, full-width forms, all JIS X 0208 kanji
(levels 1+2) and every character used in the language files and cooked world data.
Keeps the download small; the subset stays under the OFL."""

import glob
import os
import sys

from fontTools import subset

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def jis_x0208_chars() -> set[int]:
    out = set()
    for hi in range(0x81, 0xF0):
        if 0xA0 <= hi <= 0xDF:
            continue
        for lo in list(range(0x40, 0x7F)) + list(range(0x80, 0xFD)):
            try:
                out.add(ord(bytes([hi, lo]).decode("shift_jis")))
            except UnicodeDecodeError:
                pass
    return out


def main() -> int:
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "third_party", "gfonts", "ofl", "bizudpgothic",
                                                              "BIZUDPGothic-Regular.ttf")
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "game", "data", "fonts", "BIZUDPGothic-Regular.ttf")
    cps = set(range(0x20, 0x7F)) | set(range(0xA0, 0x180)) | set(range(0x2000, 0x2070)) | set(range(0x2190, 0x2200))
    cps |= set(range(0x3000, 0x3100)) | set(range(0x31F0, 0x3200)) | set(range(0xFF00, 0xFFF0)) | {0x2022, 0x00B0}
    cps |= jis_x0208_chars()
    for pattern in ["game/data/lang/*.lang", "game/data/world/*/client.txt", "game/data/world/*/pois.json"]:
        for p in glob.glob(os.path.join(ROOT, pattern)):
            cps |= {ord(ch) for ch in open(p, encoding="utf-8").read()}
    opts = subset.Options()
    opts.layout_features = ["*"]
    opts.name_IDs = ["*"]
    opts.name_legacy = True
    opts.name_languages = ["*"]
    opts.notdef_outline = True
    font = subset.load_font(src, opts)
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=sorted(cps))
    sub.subset(font)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    subset.save_font(font, dst, opts)
    print(f"{dst}: {len(cps)} codepoints, {os.path.getsize(dst) / 1e6:.2f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
