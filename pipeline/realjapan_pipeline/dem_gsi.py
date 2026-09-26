"""GSI elevation tiles (国土地理院 標高タイル, PNG encoding).

Encoding (GSI spec): x = R*2^16 + G*2^8 + B; x < 2^23 -> h = x*0.01 m;
x == 2^23 -> no data; x > 2^23 -> h = (x - 2^24)*0.01 m. Heights are T.P.
DEM5A (laser survey, 5 m) is preferred, falling back to DEM5B then DEM10B.
Attribution required: 「国土地理院」 (出典: 国土地理院 標高タイル).
"""

from __future__ import annotations

import io
import math
import os
import urllib.request

import numpy as np
from PIL import Image

TILE_URL = "https://cyberjapandata.gsi.go.jp/xyz/{layer}/{z}/{x}/{y}.png"
LAYERS = [("dem5a_png", 15), ("dem5b_png", 15), ("dem_png", 14)]  # dem_png = DEM10B


def decode_png(rgb: np.ndarray) -> np.ndarray:
    r = rgb[..., 0].astype(np.int64)
    g = rgb[..., 1].astype(np.int64)
    b = rgb[..., 2].astype(np.int64)
    x = r * 65536 + g * 256 + b
    h = np.where(x < 2**23, x * 0.01, (x - 2**24) * 0.01).astype(np.float64)
    h[x == 2**23] = np.nan
    return h


def lonlat_to_tile(lon: float, lat: float, z: int) -> tuple[float, float]:
    n = 2**z
    x = (lon + 180.0) / 360.0 * n
    y = (1.0 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2.0 * n
    return x, y


class DemSampler:
    def __init__(self, cache_dir: str, offline: bool = False):
        self.cache_dir = cache_dir
        self.offline = offline
        self._tiles: dict[tuple[str, int, int, int], np.ndarray | None] = {}
        os.makedirs(cache_dir, exist_ok=True)

    def _tile(self, layer: str, z: int, x: int, y: int) -> np.ndarray | None:
        key = (layer, z, x, y)
        if key in self._tiles:
            return self._tiles[key]
        path = os.path.join(self.cache_dir, layer, str(z), str(x), f"{y}.png")
        data = None
        if os.path.exists(path):
            with open(path, "rb") as f:
                data = f.read()
        elif not self.offline:
            try:
                with urllib.request.urlopen(TILE_URL.format(layer=layer, z=z, x=x, y=y), timeout=30) as r:
                    data = r.read()
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "wb") as f:
                    f.write(data)
            except Exception:
                data = None
        arr = decode_png(np.asarray(Image.open(io.BytesIO(data)).convert("RGB"))) if data else None
        self._tiles[key] = arr
        return arr

    def height(self, lat: float, lon: float) -> float:
        for layer, z in LAYERS:
            fx, fy = lonlat_to_tile(lon, lat, z)
            tx, ty = int(fx), int(fy)
            t = self._tile(layer, z, tx, ty)
            if t is None:
                continue
            px, py = (fx - tx) * 256 - 0.5, (fy - ty) * 256 - 0.5
            i0, j0 = int(math.floor(py)), int(math.floor(px))
            vals, wsum, acc = [], 0.0, 0.0
            for di in (0, 1):
                for dj in (0, 1):
                    i, j = min(max(i0 + di, 0), 255), min(max(j0 + dj, 0), 255)
                    w = (1 - abs(py - (i0 + di))) * (1 - abs(px - (j0 + dj)))
                    hv = t[i, j]
                    if not math.isnan(hv) and w > 0:
                        acc += hv * w
                        wsum += w
                    vals.append(hv)
            if wsum > 0:
                return acc / wsum
        return float("nan")
