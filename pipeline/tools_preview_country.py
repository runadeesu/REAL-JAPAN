#!/usr/bin/env python3
"""Top-down preview map of the fictional country's layout (for checking the design by eye).

Usage: python3 pipeline/tools_preview_country.py OUT.png [--terrain]
"""
from __future__ import annotations

import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from country import capital as C  # noqa: E402
from country import nation as N  # noqa: E402

X0, Y0, X1, Y1 = -46000, -21000, 9000, 30000
S = 0.03  # px per metre (1 px = 33 m)
W, H = int((X1 - X0) * S), int((Y1 - Y0) * S)


def px(p):
    return ((p[0] - X0) * S, (Y1 - p[1]) * S)


def main():
    out = sys.argv[1]
    img = Image.new("RGB", (W, H), (40, 70, 110))
    terrain = "--terrain" in sys.argv
    if terrain:
        from country.terrain import build_country_terrain
        t = build_country_terrain(preview=True)
        img = t.preview_image(X0, Y0, X1, Y1, S)
    d = ImageDraw.Draw(img, "RGBA")
    try:
        font = ImageFont.truetype("/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf", 22)
        small = ImageFont.truetype("/usr/share/fonts/opentype/ipafont-gothic/ipag.ttf", 16)
    except OSError:
        font = small = ImageFont.load_default()
    if not terrain:
        for poly in (N.MAIN_COAST, N.SOUTH_ISLAND, C.AIRPORT, C.ISLET):
            d.polygon([px(p) for p in poly], fill=(120, 150, 95))
        for name, poly, _, _ in N.PLAINS:
            d.polygon([px(p) for p in poly], fill=(160, 180, 110, 120))
        for name, line, hgt, hw in N.RANGES:
            d.line([px(p) for p in line], fill=(110, 80, 50, 200), width=max(2, int(hw * S * 0.5)))
            d.text(px(line[len(line) // 2]), name, fill=(60, 30, 10), font=small)
        v = N.VOLCANO
        for r in (v[4], v[4] * 0.6, v[4] * 0.3, v[4] * 0.08):
            d.ellipse([px((v[1] - r, v[2] + r)), px((v[1] + r, v[2] - r))], outline=(90, 50, 30), width=2)
        d.text(px((v[1] + 400, v[2])), v[0], fill=(40, 10, 0), font=font)
    for name, pts in N.RIVERS + [("千景川", C.RIVER)]:
        d.line([px(p[:2]) for p in pts], fill=(60, 120, 200), width=3)
    for name, lvl, poly in N.LAKES:
        d.polygon([px(p) for p in poly], fill=(60, 120, 200))
    for name, style, poly in C.DISTRICTS:
        d.polygon([px(p) for p in poly], outline=(255, 255, 255, 120))
    for name, en, style, poly in N.CITIES:
        d.polygon([px(p) for p in poly], fill=(230, 200, 180, 160), outline=(255, 255, 255))
        d.text(px(poly[0]), name, fill=(0, 0, 0), font=small)
    for name, en, x, y, r in N.VILLAGES:
        d.ellipse([px((x - r, y + r)), px((x + r, y - r))], fill=(220, 190, 160, 160))
        d.text(px((x + r, y)), name, fill=(20, 20, 20), font=small)
    for name, w, pts in N.ROADS + [(n, w, p) for n, w, p in C.ARTERIALS]:
        d.line([px(p) for p in pts], fill=(250, 240, 120), width=2 if w < 12 else 3)
    rails = [C.RAIL_LOOP, C.RAIL_BRANCH + N.MAIN_LINE_EXT, C.SHINKANSEN[:7] + N.SHINKANSEN_EXT, N.NORTH_SHINKANSEN]
    cols = [(90, 200, 90), (230, 120, 40), (40, 90, 230), (170, 60, 200)]
    for pts, col in zip(rails, cols):
        d.line([px(p) for p in pts], fill=col, width=4)
    for name, x, y, hd in C.STATIONS + N.MAIN_LINE_STATIONS + C.SHINKANSEN_STATIONS + N.SHINKANSEN_STATIONS_EXT + N.NORTH_SHINKANSEN_STATIONS:
        d.rectangle([px((x - 250, y + 250)), px((x + 250, y - 250))], fill=(255, 255, 255), outline=(0, 0, 0))
    for (a, b, w) in (C.RUNWAY, N.SOUTH_RUNWAY):
        d.line([px(a), px(b)], fill=(40, 40, 40), width=5)
    for a, b, kind, pts in N.FERRY_ROUTES:
        d.line([px(p) for p in pts], fill=(255, 255, 255, 180), width=2)
    # scale bar 10 km
    d.line([px((X0 + 2000, Y0 + 2000)), px((X0 + 12000, Y0 + 2000))], fill=(255, 255, 255), width=4)
    d.text(px((X0 + 2000, Y0 + 3200)), "10 km", fill=(255, 255, 255), font=font)
    img.save(out)
    print(out, img.size)


if __name__ == "__main__":
    main()
