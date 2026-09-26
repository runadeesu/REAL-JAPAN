"""Geodesy helpers mirroring rjcore (core/include/rj/geo): GRS80, ECEF, local ENU, JIS mesh codes."""

from __future__ import annotations

import math
from dataclasses import dataclass

A = 6378137.0
F = 1.0 / 298.257222101
E2 = F * (2.0 - F)


def geodetic_to_ecef(lat: float, lon: float, h: float) -> tuple[float, float, float]:
    la, lo = math.radians(lat), math.radians(lon)
    s = math.sin(la)
    n = A / math.sqrt(1.0 - E2 * s * s)
    return ((n + h) * math.cos(la) * math.cos(lo), (n + h) * math.cos(la) * math.sin(lo), (n * (1 - E2) + h) * s)


@dataclass(frozen=True)
class LocalFrame:
    """East-North-Up frame anchored at (lat, lon, h). Matches rj::geo::LocalFrame."""

    lat: float
    lon: float
    h: float = 0.0

    def __post_init__(self) -> None:
        la, lo = math.radians(self.lat), math.radians(self.lon)
        sl, cl, so, co = math.sin(la), math.cos(la), math.sin(lo), math.cos(lo)
        object.__setattr__(self, "_o", geodetic_to_ecef(self.lat, self.lon, self.h))
        object.__setattr__(
            self, "_m", ((-so, co, 0.0), (-sl * co, -sl * so, cl), (cl * co, cl * so, sl))
        )

    def to_local(self, lat: float, lon: float, h: float) -> tuple[float, float, float]:
        x, y, z = geodetic_to_ecef(lat, lon, h)
        ox, oy, oz = self._o  # type: ignore[attr-defined]
        dx, dy, dz = x - ox, y - oy, z - oz
        m = self._m  # type: ignore[attr-defined]
        return (
            m[0][0] * dx + m[0][1] * dy + m[0][2] * dz,
            m[1][0] * dx + m[1][1] * dy + m[1][2] * dz,
            m[2][0] * dx + m[2][1] * dy + m[2][2] * dz,
        )


# --- JIS X 0410 mesh codes (subset needed by the pipeline) -------------------


@dataclass(frozen=True)
class Mesh:
    code: str

    @property
    def level(self) -> int:
        return {4: 1, 6: 2, 8: 3, 9: 4, 10: 5, 11: 6}[len(self.code)]

    def bounds(self) -> tuple[float, float, float, float]:
        """(min_lat, min_lon, max_lat, max_lon)"""
        c = self.code
        lat = int(c[0:2]) / 1.5
        lon = int(c[2:4]) + 100.0
        dlat, dlon = 2.0 / 3.0, 1.0
        if len(c) >= 6:
            dlat, dlon = dlat / 8, dlon / 8
            lat += int(c[4]) * dlat
            lon += int(c[5]) * dlon
        if len(c) >= 8:
            dlat, dlon = dlat / 10, dlon / 10
            lat += int(c[6]) * dlat
            lon += int(c[7]) * dlon
        for q in c[8:]:
            dlat, dlon = dlat / 2, dlon / 2
            k = int(q)
            if k >= 3:
                lat += dlat
            if k in (2, 4):
                lon += dlon
        return (lat, lon, lat + dlat, lon + dlon)

    def center(self) -> tuple[float, float]:
        a, b, c, d = self.bounds()
        return ((a + c) / 2, (b + d) / 2)


def mesh3_from_latlon(lat: float, lon: float) -> str:
    i = math.floor(lat * 120 + 1e-9)  # 30" rows
    j = math.floor((lon - 100) * 80 + 1e-9)  # 45" columns
    p, u = i // 80, j // 80
    q, v = (i % 80) // 10, (j % 80) // 10
    r, w = i % 10, j % 10
    return f"{p:02d}{u:02d}{q}{v}{r}{w}"


def meshes3_covering(min_lat: float, min_lon: float, max_lat: float, max_lon: float) -> list[str]:
    out = []
    i0, i1 = math.floor(min_lat * 120 + 1e-9), math.ceil(max_lat * 120 - 1e-9)
    j0, j1 = math.floor((min_lon - 100) * 80 + 1e-9), math.ceil((max_lon - 100) * 80 - 1e-9)
    for i in range(i0, i1):
        for j in range(j0, j1):
            out.append(mesh3_from_latlon((i + 0.5) / 120, 100 + (j + 0.5) / 80))
    return out
