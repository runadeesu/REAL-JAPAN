"""Texture download (PLATEAU appearance images) and per-cell atlas packing."""

from __future__ import annotations

import concurrent.futures as cf
import io
import math
import os
import urllib.request

from PIL import Image


def fetch_images(base_url: str, uris: list[str], cache_dir: str, workers: int = 16) -> dict[str, str]:
    """Download images (cached). Returns uri -> local path for the ones available."""
    os.makedirs(cache_dir, exist_ok=True)

    def one(uri: str) -> tuple[str, str | None]:
        path = os.path.join(cache_dir, uri.replace("/", os.sep))
        if os.path.exists(path) and os.path.getsize(path) > 0:
            return uri, path
        os.makedirs(os.path.dirname(path), exist_ok=True)
        for attempt in range(3):
            try:
                with urllib.request.urlopen(base_url + uri, timeout=60) as r:
                    data = r.read()
                with open(path, "wb") as f:
                    f.write(data)
                return uri, path
            except Exception:
                if attempt == 2:
                    return uri, None
        return uri, None

    out: dict[str, str] = {}
    with cf.ThreadPoolExecutor(workers) as ex:
        for uri, path in ex.map(one, uris):
            if path:
                out[uri] = path
    return out


class Atlas:
    """Single-page shelf-packed atlas. Every texture is uniformly downscaled so that
    the page fits; tiles get an edge-extended border to limit bleeding."""

    def __init__(self, page: int = 4096, pad: int = 2):
        self.page = page
        self.pad = pad
        self.image: Image.Image | None = None
        self.rects: dict[int, tuple[int, int, int, int]] = {}  # key -> (x, y, w, h) in pixels
        self.scale = 1.0

    def build(self, images: dict[int, Image.Image]) -> None:
        if not images:
            return
        total = sum(im.width * im.height for im in images.values())
        scale = min(1.0, math.sqrt(0.80 * self.page * self.page / max(1, total)))
        while True:
            sizes = {k: (max(4, round(im.width * scale)), max(4, round(im.height * scale))) for k, im in images.items()}
            rects = self._pack(sizes)
            if rects is not None:
                break
            scale *= 0.92
        self.scale = scale
        self.rects = rects
        atlas = Image.new("RGB", (self.page, self.page), (128, 128, 128))
        p = self.pad
        for k, (x, y, w, h) in rects.items():
            tile = images[k].convert("RGB").resize((w, h), Image.LANCZOS if scale < 1 else Image.BILINEAR)
            # edge-extended border
            big = Image.new("RGB", (w + 2 * p, h + 2 * p))
            big.paste(tile.resize((w + 2 * p, h + 2 * p), Image.NEAREST), (0, 0))
            big.paste(tile, (p, p))
            atlas.paste(big, (x - p, y - p))
        self.image = atlas

    def _pack(self, sizes: dict[int, tuple[int, int]]) -> dict[int, tuple[int, int, int, int]] | None:
        p = self.pad
        order = sorted(sizes, key=lambda k: (-sizes[k][1], -sizes[k][0]))
        x, y, shelf_h = p, p, 0
        rects = {}
        for k in order:
            w, h = sizes[k]
            if x + w + p > self.page:
                x, y = p, y + shelf_h + 2 * p
                shelf_h = 0
            if y + h + p > self.page:
                return None
            rects[k] = (x, y, w, h)
            x += w + 2 * p
            shelf_h = max(shelf_h, h)
        return rects

    def remap(self, key: int, u: float, v: float) -> tuple[float, float]:
        """CityGML texture coordinates (origin bottom-left) -> atlas UV (origin top-left, GL/raylib)."""
        x, y, w, h = self.rects[key]
        u = min(1.0, max(0.0, u))
        v = min(1.0, max(0.0, v))
        return ((x + u * w) / self.page, (y + (1.0 - v) * h) / self.page)

    def jpeg(self, quality: int = 88) -> bytes:
        buf = io.BytesIO()
        assert self.image is not None
        self.image.save(buf, format="JPEG", quality=quality, optimize=True, subsampling=0)
        return buf.getvalue()
