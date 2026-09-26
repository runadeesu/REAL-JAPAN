#!/usr/bin/env python3
"""Generate the application icon (original artwork): client/res/realjapan.ico and game/data/icon.png."""

import os

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def draw(size: int) -> Image.Image:
    s = size * 4  # supersample
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    r = s // 6
    d.rounded_rectangle([0, 0, s - 1, s - 1], radius=r, fill=(245, 244, 240, 255))
    # rising sun
    cx, cy, rad = s // 2, int(s * 0.46), int(s * 0.26)
    d.ellipse([cx - rad, cy - rad, cx + rad, cy + rad], fill=(214, 0, 40, 255))
    # city skyline silhouette along the bottom
    base = int(s * 0.80)
    blocks = [(0.08, 0.60), (0.18, 0.52), (0.27, 0.66), (0.35, 0.40), (0.45, 0.58), (0.53, 0.30),
              (0.62, 0.55), (0.71, 0.45), (0.80, 0.62), (0.88, 0.50)]
    w = int(s * 0.085)
    for x, top in blocks:
        x0 = int(s * x)
        d.rectangle([x0, int(s * top), x0 + w, base], fill=(28, 30, 38, 255))
    d.rectangle([int(s * 0.06), base, int(s * 0.94), int(s * 0.86)], fill=(28, 30, 38, 255))
    return img.resize((size, size), Image.LANCZOS)


def main() -> None:
    big = draw(256)
    os.makedirs(os.path.join(ROOT, "client", "res"), exist_ok=True)
    big.save(os.path.join(ROOT, "client", "res", "realjapan.ico"),
             sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    os.makedirs(os.path.join(ROOT, "game", "data"), exist_ok=True)
    draw(128).save(os.path.join(ROOT, "game", "data", "icon.png"))


if __name__ == "__main__":
    main()
