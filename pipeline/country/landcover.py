"""Land cover of 秋津国 (fictional) on the terrain raster: forest, rice paddies, upland fields,
grassland, bare volcanic ground / rock, sand, and a snow-potential field (how much snow lies in
winter: the northern coast beyond the mountain spine and the high mountains; the client scales it
by season). Rules of thumb (not measured data): paddies on flat alluvial plains and valley floors,
fields on gentle slopes near villages, forest on slopes and hills, grass on the volcano's lower
skirt and river flood plains, bare scoria above the volcano's tree line.
"""

from __future__ import annotations

import numpy as np
from scipy import ndimage
from PIL import Image, ImageDraw
from shapely.geometry import Point, Polygon

from . import layout as L
from . import nation as N
from .terrain import smoothstep, value_noise

CLASSES = ("forest", "paddy", "field", "grass", "bare", "sand")


def _open_runs(mask):
    d = np.diff(np.concatenate([[0], np.asarray(mask, np.int8), [0]]))
    return list(zip(np.nonzero(d == 1)[0], np.nonzero(d == -1)[0]))


class LandCover:
    def __init__(self, ctry, spec, seed: int = 7):
        T = ctry.terrain
        R = T.RES
        self.x0, self.y0, self.RES, self.nx, self.ny = T.x0, T.y0, R, T.nx, T.ny
        h = T.h
        gy, gx = np.gradient(h, R)
        slope = np.hypot(gx, gy).astype(np.float32)
        del gx, gy
        land = T.land & (h > 0.4)
        urban = T._raster([Polygon(p).buffer(30) for n, s, p in L.DISTRICTS if s not in ("village",)])
        village = T._raster([Polygon(p).buffer(60) for n, s, p in L.DISTRICTS if s == "village"])
        plains = T._raster([Polygon(p) for _, p, _, _ in N.PLAINS])
        water = T._raster([rv.poly for rv in ctry.rivers] + [Polygon(pp) for _, _, pp in L.LAKES])
        dist_river = ndimage.distance_transform_edt(~water).astype(np.float32) * R
        dist_village = ndimage.distance_transform_edt(~village).astype(np.float32) * R
        n1 = self._noise(400.0, seed + 1)
        n2 = self._noise(1500.0, seed + 2)
        n3 = self._noise(160.0, seed + 3)
        X = (self.x0 + np.arange(self.nx) * R).astype(np.float32)[None, :]
        Y = (self.y0 + np.arange(self.ny) * R).astype(np.float32)[:, None]
        vname, vx, vy, vh, vrb, vrc, vcd = N.VOLCANO
        rv = np.hypot(X - vx, Y - vy)
        sname, sx, sy, sh, srb, src, scd = N.SOUTH_CONE
        rs = np.hypot(X - sx, Y - sy)
        free = land & ~urban & ~water
        # paddies: flat plains and valley floors near rivers, below the hills
        floor = (dist_river < 1100) & (slope < 0.03) & (h < 520)
        paddy = free & (slope < 0.035) & (h < 420) & (plains | floor) & (n1 > 0.22)
        # upland fields: gentle ground around villages, patches on the plains
        field = free & ~paddy & (slope < 0.12) & (h < 650) & (((dist_village < 1400) & (n1 > 0.3)) | (plains & (n2 > 0.62)))
        # grass: the volcano's lower skirt (pasture), flood-plain strips beside the rivers, airports
        skirt = (rv > vrb * 0.42) & (rv < vrb * 0.78) & (n2 > 0.42) & (slope < 0.18)
        grass = free & ~paddy & ~field & (skirt | ((dist_river < 45) & (slope < 0.1)))
        if spec is not None and spec.grass is not None:
            grass |= T._raster([spec.grass])
        # bare: scoria above the volcano's tree line, the southern cone's summit, cliffs
        treeline = vh * 0.8 + 90.0 * (n1 - 0.5)
        bare = land & (((rv < vrb * 0.5) & (h > treeline)) | ((rs < srb * 0.22) & (h > sh * 0.72)) | (slope > 1.1))
        sand = T._raster([Polygon(b).buffer(40) for b in N.BEACHES] + [Polygon(L.BEACH).buffer(60)]) & (h < 4.0) & ~urban
        sand |= land & (T.sd < 25.0) & (T.sd > -5.0) & (h < 3.2) & (n3 > 0.72) & ~urban  # small beaches in coves
        # clearings along the roads and the railways (no canopy over a carriageway or a viaduct)
        clear = Image.new("1", (self.nx, self.ny), 0)
        dr = ImageDraw.Draw(clear)
        pix = lambda P: [((x - self.x0) / R, (y - self.y0) / R) for x, y in P]  # noqa: E731
        for r in ctry.net.roads:
            dr.line(pix(r.line.coords), fill=1, width=max(1, int(round((r.width + 8.0) / R))))
        if spec is not None:
            for li, Rl in enumerate(spec.rails):
                tun = spec.tunnel_flags[li] if li < len(spec.tunnel_flags) else np.zeros(len(Rl), bool)
                for a, b in _open_runs(~np.asarray(tun, bool)):  # (the forest stays over the tunnels)
                    if b - a >= 2:
                        dr.line(pix(Rl[a:b, :2]), fill=1, width=max(2, int(round(30.0 / R))))
        cleared = np.array(clear, bool)
        if spec is not None:  # ... and over the road tunnels
            for rs in getattr(spec, "road_structs", []):
                if rs.kind == "tunnel" and len(rs.pts) > 6:
                    keep = Image.new("1", (self.nx, self.ny), 0)
                    ImageDraw.Draw(keep).line(pix(rs.pts[3:-3, :2]), fill=1, width=max(1, int(round((rs.width + 10.0) / R))))
                    cleared &= ~np.asarray(keep, bool)
        forest = free & ~paddy & ~field & ~grass & ~bare & ~sand & (h > 2.5) & ~cleared
        # roadside and village clearings keep some trees; plains keep only groves (shrine woods, windbreaks)
        forest &= ~(plains & (n3 < 0.78) & (slope < 0.05))
        self.masks = {"forest": forest, "paddy": paddy, "field": field, "grass": grass, "bare": bare, "sand": sand}
        # snow potential (0..1): the north side of the spine and high ground; none on the southern plains
        north = smoothstep(19000.0, 23000.0, Y + 1800.0 * (n2 - 0.5) + np.zeros_like(X))
        high = smoothstep(700.0, 1400.0, h)
        self.snow = np.clip(np.maximum(north * 0.95, high), 0.0, 1.0).astype(np.float32)
        self.snow[~land] = 0.0
        self.slope = slope

    def _noise(self, scale, seed):
        out = np.empty((self.ny, self.nx), np.float32)
        xs = self.x0 + np.arange(self.nx) * self.RES
        for r0 in range(0, self.ny, 600):
            r1 = min(self.ny, r0 + 600)
            Xg, Yg = np.meshgrid(xs, self.y0 + np.arange(r0, r1) * self.RES)
            out[r0:r1] = value_noise(Xg, Yg, scale, seed)
        return out

    def sample(self, name, x, y):
        """Bilinear weight (0..1) of class `name` (or 'snow') at arrays x, y."""
        a = self.snow if name == "snow" else self.masks[name]
        gx = np.clip((np.asarray(x, float) - self.x0) / self.RES, 0, self.nx - 1.001)
        gy = np.clip((np.asarray(y, float) - self.y0) / self.RES, 0, self.ny - 1.001)
        i0, j0 = np.floor(gy).astype(int), np.floor(gx).astype(int)
        fy, fx = gy - i0, gx - j0
        A = a.astype(np.float32) if a.dtype == bool else a
        return (A[i0, j0] * (1 - fx) + A[i0, j0 + 1] * fx) * (1 - fy) + (A[i0 + 1, j0] * (1 - fx) + A[i0 + 1, j0 + 1] * fx) * fy
