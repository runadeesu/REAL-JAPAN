"""Terrain of 秋津国 (fictional): coastlines, relief (ranges, volcano, hills, plains, the capital's
upland and hollow), river valleys, lakes, beaches and the sea floor, as a height raster in
country-local metres. Deterministic for a given seed.

The relief is designed, not measured: crest heights, slopes and valley shapes follow general
Japanese landforms (steep forested ranges, a concave stratovolcano, flat alluvial plains with
rice fields, basins, ria coasts) without reproducing any real place.
"""

from __future__ import annotations

import math

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage
from shapely.geometry import LineString, Polygon
from shapely.ops import unary_union

from . import capital as C
from . import nation as N

BOUNDS = (-46000.0, -21000.0, 9000.0, 30000.0)  # x0, y0, x1, y1


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3 - 2 * t)


# --------------------------------------------------------------------------------------------
# noise
def _hash_tables(seed):
    rng = np.random.default_rng(seed)
    return rng.integers(0, 2**31 - 1, size=4096, dtype=np.int64), rng.random(4096).astype(np.float32)


def value_noise(X, Y, scale, seed):
    perm, vals = _hash_tables(seed)
    gx, gy = X / scale, Y / scale
    x0, y0 = np.floor(gx), np.floor(gy)
    fx, fy = (gx - x0).astype(np.float32), (gy - y0).astype(np.float32)
    x0 = x0.astype(np.int64)
    y0 = y0.astype(np.int64)

    def h(ix, iy):
        return vals[(perm[(ix * 73856093) % 4096] ^ (iy * 19349663)) % 4096]

    sx, sy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    a, b = h(x0, y0), h(x0 + 1, y0)
    c, d = h(x0, y0 + 1), h(x0 + 1, y0 + 1)
    return (a + (b - a) * sx) * (1 - sy) + (c + (d - c) * sx) * sy


def fbm(X, Y, scale, octaves, seed, ridged=False, gain=0.5):
    tot, amp, norm = 0.0, 1.0, 0.0
    for o in range(octaves):
        n = value_noise(X, Y, scale / (2**o), seed + o * 101)
        if ridged:
            n = 1.0 - np.abs(n * 2 - 1)
            n = n * n
        tot = tot + n * amp
        norm += amp
        amp *= gain
    return tot / norm


# --------------------------------------------------------------------------------------------
# coastlines
def roughen(coast, seed: int, min_len: float = 70.0) -> list:
    """Fractal midpoint displacement of a closed coastline [(x, y, roughness)]."""
    rng = np.random.default_rng(seed)
    out = []
    n = len(coast)
    for i in range(n):
        x0, y0, r = coast[i]
        x1, y1 = coast[(i + 1) % n][:2]
        seg = [(x0, y0), (x1, y1)]
        if r > 0:
            amp0 = 0.24 * r if r < 2 else 0.42
            level = 0
            while True:
                longest = max(math.hypot(b[0] - a[0], b[1] - a[1]) for a, b in zip(seg, seg[1:]))
                if longest < min_len:
                    break
                nxt = [seg[0]]
                for a, b in zip(seg, seg[1:]):
                    L = math.hypot(b[0] - a[0], b[1] - a[1])
                    if L >= min_len:
                        nx, ny = -(b[1] - a[1]) / L, (b[0] - a[0]) / L
                        amp = amp0 * (0.66 if r < 2 else 0.78) ** level
                        off = rng.normal(0.0, amp) * L
                        t = rng.uniform(0.4, 0.6)
                        nxt.append((a[0] + (b[0] - a[0]) * t + nx * off, a[1] + (b[1] - a[1]) * t + ny * off))
                    nxt.append(b)
                seg = nxt
                level += 1
        out.extend(seg[:-1])
    return out


def _largest(g):
    if g.geom_type == "Polygon":
        return g
    return max(getattr(g, "geoms", []), key=lambda p: p.area)


def land_polygons(seed: int = 7):
    """Main island (with the capital's reclaimed airport), the southern island, the capital's islet."""
    main = _largest(Polygon(roughen(N.MAIN_COAST, seed)).buffer(0))
    main = main.union(Polygon(C.AIRPORT).buffer(0))
    main = _largest(main.buffer(0))
    south = _largest(Polygon(roughen(N.SOUTH_ISLAND, seed + 1)).buffer(0))
    islet = Polygon(C.ISLET).buffer(0)
    return main, south, islet


def warp_line(line, amp, seed, step=150.0):
    """Densify a polyline and displace it sideways by smooth noise (crest lines are not straight)."""
    L = LineString(line)
    n = max(2, int(L.length / step))
    P = np.array([L.interpolate(t, normalized=True).coords[0] for t in np.linspace(0, 1, n)])
    T = np.gradient(P, axis=0)
    T /= np.maximum(np.linalg.norm(T, axis=1, keepdims=True), 1e-9)
    Nrm = np.column_stack([-T[:, 1], T[:, 0]])
    s_ = np.linspace(0, L.length, n)
    off = (value_noise(s_, np.full(n, 17.3 + seed), 3500.0, seed) - 0.5) * 2.0 * amp \
        + (value_noise(s_, np.full(n, 3.1 + seed), 1100.0, seed + 1) - 0.5) * 0.8 * amp
    return P + Nrm * off[:, None]


def line_distance(P, shape, x0, y0, res):
    """Exact distance (m) from every raster cell to polyline P and the index of the nearest segment
    (nearest rasterised segment found by EDT, then refined analytically over it and its neighbours,
    so the distance field has no pixel steps)."""
    ny, nx = shape
    img = Image.new("I", (nx, ny), 0)
    d = ImageDraw.Draw(img)
    for i in range(len(P) - 1):
        d.line([((P[i][0] - x0) / res, (P[i][1] - y0) / res), ((P[i + 1][0] - x0) / res, (P[i + 1][1] - y0) / res)],
               fill=i + 1, width=1)
    lab = np.asarray(img, np.int32)
    _, (ii, jj) = ndimage.distance_transform_edt(lab == 0, return_indices=True)
    seg = np.clip(lab[ii, jj] - 1, 0, len(P) - 2)
    del ii, jj
    P = np.asarray(P, float)
    best = np.full(shape, np.inf, np.float32)
    bseg = seg.copy()
    xs = (x0 + np.arange(nx) * res).astype(np.float32)
    for r0 in range(0, ny, 500):
        r1 = min(ny, r0 + 500)
        X = np.broadcast_to(xs, (r1 - r0, nx))
        Y = np.broadcast_to((y0 + np.arange(r0, r1) * res).astype(np.float32)[:, None], (r1 - r0, nx))
        sb = seg[r0:r1]
        bb = best[r0:r1]
        bs = bseg[r0:r1]
        for o in (-1, 0, 1):
            k = np.clip(sb + o, 0, len(P) - 2)
            ax, ay = P[k, 0], P[k, 1]
            vx, vy = P[k + 1, 0] - ax, P[k + 1, 1] - ay
            l2 = np.maximum(vx * vx + vy * vy, 1e-9)
            t = np.clip(((X - ax) * vx + (Y - ay) * vy) / l2, 0.0, 1.0)
            dd = np.hypot(X - (ax + t * vx), Y - (ay + t * vy)).astype(np.float32)
            better = dd < bb
            bb[better] = dd[better]
            bs[better] = k[better]
    return best, bseg


def priority_flood(h, land, eps=0.01):
    """Fill depressions so every land cell drains to the sea (Barnes et al. 2014, +epsilon)."""
    import heapq
    ny, nx = h.shape
    hf = h.astype(np.float64).copy()
    done = ~land.copy()
    edge = land & ndimage.binary_dilation(~land, structure=np.ones((3, 3), bool))
    edge[0, :] |= land[0, :]
    edge[-1, :] |= land[-1, :]
    edge[:, 0] |= land[:, 0]
    edge[:, -1] |= land[:, -1]
    flat = hf.ravel()
    dn = done.ravel()
    heap = [(flat[i], i) for i in np.flatnonzero(edge.ravel())]
    for _, i in heap:
        dn[i] = True
    heapq.heapify(heap)
    offs = [(-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1)]
    push, pop = heapq.heappush, heapq.heappop
    while heap:
        z, i = pop(heap)
        r, c = divmod(i, nx)
        for dr, dc in offs:
            rr, cc = r + dr, c + dc
            if rr < 0 or cc < 0 or rr >= ny or cc >= nx:
                continue
            j = rr * nx + cc
            if dn[j]:
                continue
            dn[j] = True
            zj = flat[j]
            if zj <= z:
                zj = z + eps
                flat[j] = zj
            push(heap, (zj, j))
    return hf


def erode_drainage(h, land, R, seed):
    """Cut valleys along the drainage network (computed on a 40 m grid): each channel is lowered by a
    depth growing with its catchment area, kept falling downstream, and widened into V-shaped
    valleys with side slopes that vary with the rock (noise). Plains and towns are left alone."""
    f = max(1, int(round(40.0 / R)))
    hc = h[::f, ::f].astype(np.float64)
    lc = land[::f, ::f]
    ny, nx = hc.shape
    cell = R * f
    Xc, Yc = np.meshgrid(np.arange(nx) * cell, np.arange(ny) * cell)
    # route flow over a slightly perturbed surface (no parallel D8 grooves on smooth slopes)
    pert = (value_noise(Xc, Yc, 260.0, seed + 91) - 0.5) * 24.0 + (value_noise(Xc, Yc, 90.0, seed + 92) - 0.5) * 8.0
    hf = priority_flood(hc + pert, lc)
    # receivers: steepest descent among the 8 neighbours
    pad = np.pad(hf, 1, mode="edge")
    best = np.zeros_like(hf)
    rec = np.arange(ny * nx).reshape(ny, nx)
    idx = np.arange(ny * nx).reshape(ny, nx)
    pidx = np.pad(idx, 1, mode="edge")
    for dr, dc in [(-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1)]:
        nb = pad[1 + dr:1 + dr + ny, 1 + dc:1 + dc + nx]
        slope = (hf - nb) / (math.hypot(dr, dc) * cell)
        better = slope > best
        best = np.where(better, slope, best)
        rec = np.where(better, pidx[1 + dr:1 + dr + ny, 1 + dc:1 + dc + nx], rec)
    rec = np.where(lc, rec, idx).ravel()
    order = np.argsort(-hf.ravel(), kind="stable")
    order = order[lc.ravel()[order]]
    acc = np.ones(ny * nx)
    rl = rec.tolist()
    al = acc.tolist()
    for i in order.tolist():
        j = rl[i]
        if j != i:
            al[j] += al[i]
    area = np.asarray(al).reshape(ny, nx) * cell * cell / 1e6  # km2
    gy_, gx_ = np.gradient(hc, cell)
    rough = ndimage.gaussian_filter(np.hypot(gx_, gy_), 450.0 / cell)
    relief = np.clip((hc - 20.0) / 120.0, 0.0, 1.0) * smoothstep(0.06, 0.16, rough)
    for name, vx, vy, H, Rb, rc, cd in (N.VOLCANO, N.SOUTH_CONE):  # young cones keep their own radial gullies
        relief = relief * smoothstep(Rb * 0.75, Rb * 1.05, np.hypot(Xc + BOUNDS[0] - vx, Yc + BOUNDS[1] - vy))
    depth = np.where(lc, 55.0 * area ** 0.33 * relief, 0.0)
    floor = hf - depth
    # keep the floor falling downstream (process from the sea upwards)
    fl = floor.ravel().tolist()
    for i in reversed(order.tolist()):
        j = rl[i]
        if j != i and fl[i] < fl[j] + 0.02:
            fl[i] = fl[j] + 0.02
    floor = np.asarray(fl).reshape(ny, nx)
    chan = lc & (area > 0.25) & (relief > 0.0)
    dist, (ii, jj) = ndimage.distance_transform_edt(~chan, return_indices=True)
    side = 0.35 + 0.55 * value_noise(Xc, Yc, 2500.0, seed + 71)
    valley = floor[ii, jj] + side * dist * cell
    delta = np.where(lc, np.minimum(valley - hc, 0.0), 0.0)
    delta = ndimage.gaussian_filter(delta, 0.8)
    if f > 1:
        delta = ndimage.zoom(delta, f, order=1)[: h.shape[0], : h.shape[1]]
        if delta.shape != h.shape:
            delta = np.pad(delta, ((0, h.shape[0] - delta.shape[0]), (0, h.shape[1] - delta.shape[1])), mode="edge")
    return (h + delta.astype(np.float32) * land).astype(np.float32)


# --------------------------------------------------------------------------------------------
class River:
    def __init__(self, name, pts, smooth=3, meander=0.0, seed=0):
        self.name = name
        P = np.array([(x, y) for x, y, _ in pts], float)
        W = np.array([w for _, _, w in pts], float)
        line = LineString(P)
        n = max(2, int(line.length / 20))
        s = np.linspace(0, line.length, n)
        Q = np.array([line.interpolate(v).coords[0] for v in s])
        cum = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(P, axis=0), axis=1))])
        self.widths = np.interp(s / line.length * cum[-1], cum, W)
        Q0 = Q.copy()
        for _ in range(smooth + 6):
            Q0[1:-1] = 0.25 * Q0[:-2] + 0.5 * Q0[1:-1] + 0.25 * Q0[2:]
        self.valley = Q0  # the valley's axis (the channel meanders inside its floor)
        self.belt = np.zeros(n)
        if meander > 0:
            # bends: wavelength ~ 14 channel widths, amplitude ~ 3 widths (plus slow wander), both ends fixed
            T = np.gradient(Q, axis=0)
            T /= np.maximum(np.linalg.norm(T, axis=1, keepdims=True), 1e-9)
            Nrm = np.column_stack([-T[:, 1], T[:, 0]])
            wl = 14.0 * np.maximum(self.widths, 12.0)
            phase = np.cumsum(np.concatenate([[0.0], np.diff(s) / wl[1:]])) * 2 * np.pi
            amp = meander * 3.0 * np.maximum(self.widths, 12.0)
            wander = (value_noise(s, np.full(n, 5.5 + seed), 2600.0, seed) - 0.5) * 2.0 * meander * 260.0
            env = np.clip(s / 600.0, 0, 1) * np.clip((s[-1] - s) / 600.0, 0, 1)
            off = (np.sin(phase + seed) * amp + wander) * env
            Q = Q + Nrm * off[:, None]
            self.belt = np.abs(off) + amp * env
        for _ in range(smooth):
            Q[1:-1] = 0.25 * Q[:-2] + 0.5 * Q[1:-1] + 0.25 * Q[2:]
        self.center = LineString(Q)
        polys = [LineString(Q[i:i + 2]).buffer((self.widths[i] + self.widths[i + 1]) / 4, cap_style=1)
                 for i in range(len(Q) - 1)]
        self.poly = unary_union(polys).buffer(1.0).buffer(-1.0)
        self.water = np.zeros(len(Q))


def make_rivers():
    out = [River("千景川", [(-2500, 6500, 8), (-1500, 4500, 10)] + list(C.RIVER))]
    out += [River(n, pts, meander=1.0, seed=31 + k) for k, (n, pts) in enumerate(N.RIVERS)]
    return out


# --------------------------------------------------------------------------------------------
class CountryTerrain:
    def __init__(self, land_polys, rivers, seed: int = 7, res: float = 10.0):
        self.RES = res
        x0, y0, x1, y1 = BOUNDS
        self.x0, self.y0 = x0, y0
        self.nx = int((x1 - x0) / res) + 1
        self.ny = int((y1 - y0) / res) + 1
        R = res
        mask = self._raster(land_polys)
        self.land = mask
        sd = (ndimage.distance_transform_edt(mask) - ndimage.distance_transform_edt(~mask)).astype(np.float32) * R
        self.sd = sd
        h = np.zeros((self.ny, self.nx), np.float32)
        B = 400  # rows per block for the noise fields
        cities = self._raster([Polygon(p).buffer(300) for _, _, _, p in N.CITIES]
                              + [Polygon(p).buffer(250) for _, s, p in C.DISTRICTS if s not in ("mountain",)])
        city_w = ndimage.gaussian_filter(cities.astype(np.float32), 250.0 / R)
        plains_w = np.zeros_like(h)
        plains_t = np.zeros_like(h)
        for name, poly, e0, e1 in N.PLAINS:
            m = ndimage.gaussian_filter(self._raster([Polygon(poly)]).astype(np.float32), 450.0 / R)
            t = e0 + (e1 - e0) * smoothstep(0.0, 7000.0, sd)
            plains_t = np.where(m > plains_w, t, plains_t)
            plains_w = np.maximum(plains_w, m)
        low = np.zeros_like(h)  # the capital's lowland districts (shitamachi, bay, airport)
        for name, style, poly in C.DISTRICTS:
            if style in ("shitamachi", "bay", "islet", "airport"):
                low = np.maximum(low, self._raster([Polygon(poly).buffer(250)]).astype(np.float32))
        low = ndimage.gaussian_filter(low, 400.0 / R)
        # ranges: distance to each crest line (EDT of the rasterised line)
        rng_h = np.zeros_like(h)
        for ri, (name, line, crest, hw) in enumerate(N.RANGES):
            d, _ = line_distance(warp_line(line, 0.35 * hw, seed + 50 + ri), (self.ny, self.nx), x0, y0, R)
            prof = np.clip(1.0 - d / hw, 0.0, 1.0)
            prof = prof ** 1.5 * (3 - 2 * prof) * 0.5 + prof ** 1.5 * 0.5
            rng_h = np.maximum(rng_h, prof * crest)
        for row in range(0, self.ny, B):
            r1 = min(self.ny, row + B)
            ys = y0 + np.arange(row, r1, dtype=np.float64) * R
            xs = x0 + np.arange(self.nx, dtype=np.float64) * R
            X, Y = np.meshgrid(xs, ys)
            sdb = sd[row:r1]
            # rolling hills everywhere inland (gentle in towns and plains), rising away from the sea
            hills = (fbm(X, Y, 3200, 4, seed + 3) - 0.35) * 150.0 + (fbm(X, Y, 700, 3, seed + 4) - 0.5) * 30.0
            inland = smoothstep(0.0, 2500.0, sdb)
            base = 3.0 + np.minimum(np.maximum(sdb, 0) * 0.012, 60.0)
            amp = inland * (1.0 - 0.8 * city_w[row:r1])
            hb = base + np.maximum(hills, -20.0) * amp
            # capital upland / hollow as on the island: undulating yamanote, flat shitamachi
            cap_hills = (7.0 * np.sin(X / 430 + 0.6) * np.sin(Y / 510 - 0.3) + 4.0 * np.sin(X / 260 + Y / 310)
                         + 6.0 * (fbm(X, Y, 900, 3, seed) - 0.5))
            capw = smoothstep(5200.0, 3800.0, np.hypot(X - 0.0, Y + 200.0))
            cap_plain = 3.0 + np.minimum(np.maximum(sdb, 0) * 0.006, 16.0)
            lw = low[row:r1]
            hc = cap_plain + cap_hills * (1.0 - 0.85 * lw)
            hc = hc * (1.0 - 0.6 * lw) + (2.6 + 1.2 * fbm(X, Y, 600, 2, seed + 5)) * 0.6 * lw
            for vx, vy, depth, r in C.VALLEYS:
                hc -= depth * np.exp(-((X - vx) ** 2 + (Y - vy) ** 2) / (2 * r * r))
            hb = hb * (1.0 - capw) + hc * capw
            # plains: flatten towards their target elevation (slight undulation)
            pw = plains_w[row:r1]
            hb = hb * (1.0 - pw) + (plains_t[row:r1] + (fbm(X, Y, 900, 2, seed + 6) - 0.5) * 4.0) * pw
            # mountains: crest profile with ridged detail (spurs and valleys), varying along the crest
            vary = 0.35 + 1.0 * fbm(X, Y, 6500, 3, seed + 7)
            ridge = fbm(X, Y, 1600, 6, seed + 11, ridged=True, gain=0.52)
            mnt = rng_h[row:r1] * vary * (0.45 + 0.75 * ridge)
            # the capital's own peaks (千景山 etc.)
            for px_, py_, ph, pr in C.PEAKS:
                d2 = ((X - px_) ** 2 + (Y - py_) ** 2) / (pr * pr)
                mnt = np.maximum(mnt, ph * np.exp(-1.6 * d2) * (0.75 + 0.5 * ridge) + 40 * ridge * np.exp(-0.5 * d2))
            # volcanoes: concave cones with radial gullies and a summit crater
            for name, vx, vy, H, Rb, rc, cd in (N.VOLCANO, N.SOUTH_CONE):
                r = np.hypot(X - vx, Y - vy)
                t = np.clip(1.0 - r / Rb, 0.0, 1.0)
                cone = H * t ** 1.9
                ang = np.arctan2(Y - vy, X - vx)
                wob = ang + (value_noise(X, Y, 2200.0, seed + 14) - 0.5) * 0.35
                gul = fbm(np.cos(wob) * 3000.0 + r * 0.04, np.sin(wob) * 3000.0 + r * 0.04, 800, 3, seed + 13, ridged=True)
                sector = smoothstep(0.35, 0.8, value_noise(np.cos(ang) * 2500.0, np.sin(ang) * 2500.0, 1400.0, seed + 15))
                cone = cone - gul * 70.0 * sector * np.sin(np.pi * t) ** 1.5 * (H / 1850.0)
                crater = cd * np.clip(1.0 - (r / rc) ** 2, 0.0, 1.0)
                rim = 12.0 * (H / 1850.0) * np.exp(-((r - rc) / (rc * 0.35)) ** 2)
                mnt = np.maximum(mnt, cone - crater + rim)
            # mountains rise out of the plains / hills, but not inside the towns (the cities sit on
            # their plains / uplands; the ranges end at their edges)
            hb = hb + mnt * (1.0 - 0.9 * pw) * (1.0 - 0.97 * city_w[row:r1])
            h[row:r1] = hb
        # drainage: dendritic valleys cut along the flow network of the relief
        h = erode_drainage(h, mask, R, seed)
        # coast: sea walls in the capital, beaches, rocky shores; sea floor shelving out
        beach = self._raster([Polygon(C.BEACH).buffer(150)] + [Polygon(b).buffer(120) for b in N.BEACHES]).astype(np.float32)
        beach = ndimage.gaussian_filter(beach, 20.0 / R * 10)
        ramp = smoothstep(0.0, 120.0, sd)
        land_h = h * ramp + (2.3 - 1.8 * beach) * (1.0 - ramp)
        land_h = np.maximum(land_h, 1.6)
        sea_h = -1.0 - np.minimum(60.0, 6.0 * (1 - beach) + 0.03 * beach + 0.012 * np.maximum(-sd, 0) + 0.04 * np.minimum(np.maximum(-sd, 0), 900))
        sea_h = np.where(beach > 0.5, -0.02 * np.maximum(-sd, 0) - 0.1, sea_h)
        self.h = np.where(sd > 0, land_h, sea_h).astype(np.float32)
        del land_h, sea_h
        for rv in rivers:
            self._carve_river(rv)
        self.lake_levels = {}
        for name, lvl, poly in N.LAKES:
            self._lake(Polygon(poly), lvl, name)

    # ------------------------------------------------------------------------------------
    def _pix(self, p):
        return ((p[0] - self.x0) / self.RES, (p[1] - self.y0) / self.RES)

    def _raster(self, polys) -> np.ndarray:
        img = Image.new("L", (self.nx, self.ny), 0)
        d = ImageDraw.Draw(img)
        for poly in polys:
            for p in (getattr(poly, "geoms", None) or [poly]):
                if p.is_empty:
                    continue
                d.polygon([self._pix(q) for q in p.exterior.coords], fill=255)
                for hole in p.interiors:
                    d.polygon([self._pix(q) for q in hole.coords], fill=0)
        return np.asarray(img) > 127

    def _carve_river(self, river: River):
        """Water level along the centre line (never rising downstream), a bed under the water and a
        valley cut through high ground (wider where it is deeper)."""
        P = np.asarray(river.center.coords)
        bank = self.sample(P[:, 0], P[:, 1])
        # long profile: concave (steep near the source, flat near the mouth), never above the banks,
        # never rising downstream, smooth (no terraces across the valley)
        u = np.linspace(0.0, 1.0, len(P))
        z0 = max(0.3, float(bank[0]) - 3.2)
        mouth = max(0.3, float(np.min(bank[-5:])) - 3.2) if bank[-1] > 0 else 0.3
        curve = mouth + (z0 - mouth) * (1.0 - u) ** 2.2
        water = np.maximum(0.3, np.minimum(curve, bank - 3.2))
        water = np.minimum.accumulate(water)
        water = ndimage.gaussian_filter1d(water, 25, mode="nearest")
        water = np.minimum.accumulate(np.minimum(water, bank - 2.0))
        river.water = water
        # exact distance to the centre line and the nearest segment for every raster cell
        dist, k = line_distance(P, (self.ny, self.nx), self.x0, self.y0, self.RES)
        wl = water[k].astype(np.float32)
        halfw = (river.widths[k] / 2.0).astype(np.float32)
        # valley: ground above the banks is cut down to a V-shaped profile around the valley axis,
        # with a flat floor wide enough for the meander belt
        dv, kv = line_distance(river.valley, (self.ny, self.nx), self.x0, self.y0, self.RES)
        side = 0.28 + 0.22 * self._noise_like(dv, 1900.0, 77)
        floorw = (river.widths[kv] / 2.0 + river.belt[kv] + 20.0).astype(np.float32)
        lowered = (water[kv].astype(np.float32) + 2.5) + side * np.maximum(dv - floorw, 0.0)
        self.h = np.minimum(self.h, np.where(dv < 4000.0, lowered, self.h)).astype(np.float32)
        del dv, kv, side, floorw, lowered
        # bed under the water surface, with sloping banks
        bed = wl - 2.6
        tb = np.clip((dist - (halfw - 6.0)) / 7.0, 0.0, 1.0)
        inside = dist < halfw + 1.0
        self.h = np.where(inside, np.minimum(self.h, bed * (1.0 - tb) + self.h * tb), self.h)

    def _noise_like(self, arr, scale, seed):
        ys = self.y0 + np.arange(self.ny) * self.RES
        xs = self.x0 + np.arange(self.nx) * self.RES
        out = np.empty((self.ny, self.nx), np.float32)
        for r0 in range(0, self.ny, 600):
            r1 = min(self.ny, r0 + 600)
            X, Y = np.meshgrid(xs, ys[r0:r1])
            out[r0:r1] = value_noise(X, Y, scale, seed)
        return out

    def _lake(self, poly: Polygon, level: float, name: str = ""):
        """A lake filling the low part of its basin: the water level is set just below the lowest
        point of its shore line (so it never stands above the land around it) and the ground inside
        is dug into a bowl under the water. Returns the level used."""
        ring = np.asarray(poly.exterior.coords)
        L_ = LineString(ring)
        pts = np.array([L_.interpolate(t, normalized=True).coords[0] for t in np.linspace(0, 1, 400)])
        rim = float(np.min(self.sample(pts[:, 0], pts[:, 1])))
        level = rim - 0.8
        m = self._raster([poly])
        din = ndimage.distance_transform_edt(m).astype(np.float32) * self.RES
        bed = level - np.minimum(1.5 + din * 0.08, 18.0)
        self.h = np.where(m, np.minimum(self.h, bed), self.h).astype(np.float32)
        self.lake_levels[name] = level
        return level

    def sample(self, x, y):
        x = np.asarray(x, float)
        y = np.asarray(y, float)
        gx = np.clip((x - self.x0) / self.RES, 0, self.nx - 1.001)
        gy = np.clip((y - self.y0) / self.RES, 0, self.ny - 1.001)
        i0, j0 = np.floor(gy).astype(int), np.floor(gx).astype(int)
        fy, fx = gy - i0, gx - j0
        h = self.h
        return (h[i0, j0] * (1 - fx) + h[i0, j0 + 1] * fx) * (1 - fy) + (h[i0 + 1, j0] * (1 - fx) + h[i0 + 1, j0 + 1] * fx) * fy

    def slope(self):
        gy, gx = np.gradient(self.h, self.RES)
        return np.hypot(gx, gy)

    def preview_image(self, X0, Y0, X1, Y1, S):
        """Hill-shaded elevation map (north up) at S px per metre."""
        W, H = int((X1 - X0) * S), int((Y1 - Y0) * S)
        xs = X0 + (np.arange(W) + 0.5) / S
        ys = Y1 - (np.arange(H) + 0.5) / S
        Xg, Yg = np.meshgrid(xs, ys)
        z = self.sample(Xg, Yg)
        gy, gx = np.gradient(z, -1.0 / S, 1.0 / S)
        shade = np.clip(0.55 + (-gx * 0.7 + gy * 0.7) * 1.2, 0.2, 1.3)
        col = np.zeros((H, W, 3), np.float32)
        stops = [(-60, (20, 40, 80)), (-5, (40, 80, 120)), (0, (60, 110, 150)), (0.01, (110, 140, 80)), (40, (130, 150, 90)),
                 (200, (100, 125, 70)), (600, (90, 100, 70)), (1100, (120, 110, 95)), (1500, (150, 140, 130)), (1900, (235, 235, 235))]
        zs = np.array([s[0] for s in stops], float)
        for c in range(3):
            col[..., c] = np.interp(z, zs, [s[1][c] for s in stops])
        land = z > 0.0
        col[land] *= shade[land, None]
        return Image.fromarray(np.clip(col, 0, 255).astype(np.uint8), "RGB")


def build_country_terrain(preview: bool = False, seed: int = 7):
    main, south, islet = land_polygons(seed)
    rivers = make_rivers()
    return CountryTerrain([main, south, islet], rivers, seed=seed, res=40.0 if preview else 10.0)
