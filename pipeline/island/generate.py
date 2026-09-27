"""Procedural generation of 千景島 (fictional) from the hand-made layout in layout.py.

Deterministic for a given seed. Produces, in island-local metres (x east, y north, z up):
terrain heights, water (sea, river), road centerlines and polygons (carriageway / sidewalk /
median / plaza), city blocks split into parcels. Buildings are made in buildings.py.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np
import shapely
import shapely.geometry.polygon
from PIL import Image, ImageDraw
from scipy import ndimage
from shapely import affinity
from shapely.geometry import LineString, MultiPolygon, Point, Polygon, box
from shapely.ops import split, unary_union
from shapely.strtree import STRtree

from . import layout as L

BOUNDS = (-4300.0, -3600.0, 4300.0, 3900.0)  # x0, y0, x1, y1 (covers island, islet and shelf)


def parts(g):
    if g is None or g.is_empty:
        return []
    if isinstance(g, Polygon):
        return [g]
    return [p for p in getattr(g, "geoms", []) if isinstance(p, Polygon) and not p.is_empty]


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def value_noise(X, Y, scale, seed):
    """Smooth value noise sampled at arrays X, Y (metres)."""
    rng = np.random.default_rng(seed)
    gx, gy = X / scale, Y / scale
    x0, y0 = np.floor(gx).astype(np.int64), np.floor(gy).astype(np.int64)
    fx, fy = gx - x0, gy - y0
    perm = rng.integers(0, 2**31 - 1, size=4096, dtype=np.int64)
    vals = rng.random(4096)

    def h(ix, iy):
        return vals[(perm[(ix * 73856093) % 4096] ^ (iy * 19349663)) % 4096]

    sx, sy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    a, b = h(x0, y0), h(x0 + 1, y0)
    c, d = h(x0, y0 + 1), h(x0 + 1, y0 + 1)
    return (a + (b - a) * sx) * (1 - sy) + (c + (d - c) * sx) * sy


def fbm(X, Y, scale, octaves, seed, ridged=False):
    tot, amp, norm = 0.0, 1.0, 0.0
    for o in range(octaves):
        n = value_noise(X, Y, scale / (2**o), seed + o * 101)
        if ridged:
            n = 1.0 - np.abs(n * 2 - 1)
            n = n * n
        tot = tot + n * amp
        norm += amp
        amp *= 0.5
    return tot / norm


# --------------------------------------------------------------------------------------------
@dataclass
class River:
    center: LineString
    poly: Polygon
    widths: np.ndarray  # per centerline vertex
    water: np.ndarray = field(default_factory=lambda: np.zeros(0))  # water level per centerline vertex


def make_river() -> River:
    pts = np.array([(x, y) for x, y, _ in L.RIVER], float)
    ws = np.array([w for _, _, w in L.RIVER], float)
    # densify every 20 m with Catmull-Rom-ish smoothing
    line = LineString(pts)
    n = max(2, int(line.length / 20))
    s = np.linspace(0, line.length, n)
    P = np.array([line.interpolate(v).coords[0] for v in s])
    for _ in range(3):  # smooth
        P[1:-1] = 0.25 * P[:-2] + 0.5 * P[1:-1] + 0.25 * P[2:]
    cum = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(pts, axis=0), axis=1))])
    W = np.interp(s / line.length * cum[-1], cum, ws)
    polys = [LineString(P[i:i + 2]).buffer((W[i] + W[i + 1]) / 4, cap_style=1) for i in range(len(P) - 1)]
    return River(LineString(P), unary_union(polys).buffer(1.0).buffer(-1.0), W)


# --------------------------------------------------------------------------------------------
class Terrain:
    RES = 5.0

    def __init__(self, land: Polygon, islet: Polygon, river: River, seed: int = 7):
        x0, y0, x1, y1 = BOUNDS
        self.nx = int((x1 - x0) / self.RES) + 1
        self.ny = int((y1 - y0) / self.RES) + 1
        xs = x0 + np.arange(self.nx) * self.RES
        ys = y0 + np.arange(self.ny) * self.RES
        X, Y = np.meshgrid(xs, ys)
        self.x0, self.y0 = x0, y0
        mask = self._raster([land, islet])
        sd = ndimage.distance_transform_edt(mask) * self.RES - ndimage.distance_transform_edt(~mask) * self.RES
        self.sd = sd
        # Districts: hills where the city sits on the "yamanote" upland, flat lowland elsewhere.
        low = np.zeros_like(X)
        for name, style, poly in L.DISTRICTS:
            if style in ("shitamachi", "bay", "islet", "airport"):
                low = np.maximum(low, self._raster([Polygon(poly).buffer(250)]).astype(float))
        low = ndimage.gaussian_filter(low, 40)
        plain = 3.0 + np.minimum(np.maximum(sd, 0) * 0.006, 16.0)
        hills = 7.0 * np.sin(X / 430 + 0.6) * np.sin(Y / 510 - 0.3) + 4.0 * np.sin(X / 260 + Y / 310) \
            + 6.0 * (fbm(X, Y, 900, 3, seed) - 0.5)
        h = plain + hills * (1.0 - 0.85 * low)
        h = h * (1.0 - 0.6 * low) + (2.6 + 1.2 * fbm(X, Y, 600, 2, seed + 5)) * 0.6 * low
        for vx, vy, depth, r in L.VALLEYS:
            d2 = (X - vx) ** 2 + (Y - vy) ** 2
            h -= depth * np.exp(-d2 / (2 * r * r))
        # Mountains: peaks + ridged detail, masked to the north.
        mnt = np.zeros_like(X)
        for px, py, ph, pr in L.PEAKS:
            d2 = ((X - px) ** 2 + (Y - py) ** 2) / (pr * pr)
            mnt += ph * np.exp(-1.6 * d2)
        mask_m = smoothstep(900, 1500, Y + 0.25 * X * 0) * 1.0
        ridge = fbm(X, Y, 700, 5, seed + 11, ridged=True)
        mnt = mnt * (0.75 + 0.5 * ridge) + 40 * ridge * mask_m
        h = h + mnt * mask_m
        h = np.maximum(h, 1.6)
        # Coast: sea walls in the city (quay), gentle beach in the south-west.
        beach = self._raster([Polygon(L.BEACH).buffer(150)]).astype(float)
        beach = ndimage.gaussian_filter(beach, 20)
        land_h = np.where(sd > 0, h * smoothstep(0, 120, sd) + (2.3 - 1.8 * beach) * (1 - smoothstep(0, 120, sd)), 0)
        sea_h = -1.0 - np.minimum(45.0, (6.0 * (1 - beach) + 0.03 * beach) + 0.05 * np.maximum(-sd, 0))
        sea_h = np.where(beach > 0.5, -0.02 * np.maximum(-sd, 0) - 0.1, sea_h)
        self.h = np.where(sd > 0, land_h, sea_h)
        self._carve_river(river)

    def _raster(self, polys) -> np.ndarray:
        img = Image.new("L", (self.nx, self.ny), 0)
        d = ImageDraw.Draw(img)
        for poly in polys:
            for p in parts(poly):
                d.polygon([((x - self.x0) / self.RES, (y - self.y0) / self.RES) for x, y in p.exterior.coords], fill=255)
                for hole in p.interiors:
                    d.polygon([((x - self.x0) / self.RES, (y - self.y0) / self.RES) for x, y in hole.coords], fill=0)
        return np.asarray(img) > 127

    def _carve_river(self, river: River):
        P = np.asarray(river.center.coords)
        bank = self.sample(P[:, 0], P[:, 1])
        water = np.maximum(0.3, bank - 3.2)
        water = np.minimum.accumulate(water)  # never rises downstream
        water = ndimage.gaussian_filter1d(water, 6)
        river.water = water
        rmask = self._raster([river.poly])
        din = ndimage.distance_transform_edt(rmask) * self.RES
        # nearest centerline vertex for every river cell -> local water level
        ys, xs = np.nonzero(rmask)
        if len(xs) == 0:
            return
        from scipy.spatial import cKDTree
        tree = cKDTree(P)
        _, k = tree.query(np.column_stack([self.x0 + xs * self.RES, self.y0 + ys * self.RES]))
        bed = water[k] - 2.6
        cur = self.h[ys, xs]
        t = np.clip(din[ys, xs] / 7.0, 0, 1)
        self.h[ys, xs] = cur * (1 - t) + bed * t

    def sample(self, x, y):
        x = np.asarray(x, float)
        y = np.asarray(y, float)
        gx = np.clip((x - self.x0) / self.RES, 0, self.nx - 1.001)
        gy = np.clip((y - self.y0) / self.RES, 0, self.ny - 1.001)
        i0, j0 = np.floor(gy).astype(int), np.floor(gx).astype(int)
        fy, fx = gy - i0, gx - j0
        h = self.h
        return (h[i0, j0] * (1 - fx) + h[i0, j0 + 1] * fx) * (1 - fy) + (h[i0 + 1, j0] * (1 - fx) + h[i0 + 1, j0 + 1] * fx) * fy


# --------------------------------------------------------------------------------------------
@dataclass
class Road:
    name: str
    width: float
    line: LineString
    kind: str  # arterial | street | alley | mountain | bridge
    district: str = ""

    @property
    def sidewalk(self) -> float:
        w = self.width
        return 4.5 if w >= 30 else 3.5 if w >= 16 else 2.0 if w >= 9 else 0.0

    @property
    def median(self) -> float:
        return 2.5 if self.width >= 30 else 0.0

    @property
    def carriage(self) -> float:
        return self.width - 2 * self.sidewalk


STYLE_GRID = {
    # angle deg, spacing x, spacing y, node jitter, widths [(w, p)], drop prob, warp amplitude
    "center": (14, 72, 58, 11, [(8, 0.5), (12, 0.3), (5, 0.2)], 0.14, 4),
    "business": (0, 150, 135, 0, [(22, 0.6), (16, 0.4)], 0.0, 0),
    "shitamachi": (-8, 68, 44, 5, [(6, 0.5), (4, 0.3), (9, 0.2)], 0.08, 6),
    "residential": (24, 88, 72, 9, [(6, 0.62), (9, 0.26), (4.5, 0.12)], 0.1, 22),
    "bay": (0, 190, 170, 0, [(20, 1.0)], 0.0, 0),
    "islet": (-6, 170, 130, 0, [(18, 1.0)], 0.0, 0),
}


def grid_streets(poly: Polygon, style: str, district: str, rng) -> list[Road]:
    if style not in STYLE_GRID:
        return []
    ang, sx, sy, jit, widths, drop, warp = STYLE_GRID[style]
    c = poly.centroid
    rp = affinity.rotate(poly, -ang, origin=c)
    x0, y0, x1, y1 = rp.bounds
    nxl = int((x1 - x0) / sx) + 3
    nyl = int((y1 - y0) / sy) + 3
    ox, oy = x0 - sx * rng.random(), y0 - sy * rng.random()
    node = np.zeros((nyl, nxl, 2))
    for j in range(nyl):
        for i in range(nxl):
            node[j, i] = (ox + i * sx + rng.normal(0, jit), oy + j * sy + rng.normal(0, jit))
    wlist, wp = [w for w, _ in widths], np.array([p for _, p in widths])
    roww = rng.choice(wlist, size=nyl, p=wp / wp.sum())
    colw = rng.choice(wlist, size=nxl, p=wp / wp.sum())
    segs = []
    for j in range(nyl):
        for i in range(nxl):
            if i + 1 < nxl and rng.random() >= drop:
                segs.append((node[j, i], node[j, i + 1], roww[j]))
            if j + 1 < nyl and rng.random() >= drop:
                segs.append((node[j, i], node[j + 1, i], colw[i]))
    out = []
    for a, b, w in segs:
        pts = [a, b]
        if warp > 0:  # curvy streets: bend the middle
            m = (a + b) / 2 + rng.normal(0, warp, 2)
            pts = [a, m, b]
        ln = affinity.rotate(LineString(pts), ang, origin=c)
        g = ln.intersection(poly)
        for part in getattr(g, "geoms", [g]):
            if isinstance(part, LineString) and part.length > 8:
                kind = "alley" if w < 5.5 else "street"
                out.append(Road("", float(w), part, kind, district))
    return out


@dataclass
class RoadNet:
    roads: list[Road]
    whole: Polygon          # full right-of-way (carriageway + sidewalks + medians)
    carriage: Polygon
    sidewalk: Polygon
    median: Polygon
    plaza: Polygon


def build_roads(land: Polygon, river: River, rng) -> RoadNet:
    roads = [Road(n, float(w), LineString(p), "arterial") for n, w, p in L.ARTERIALS]
    n, w, p = L.MOUNTAIN_ROAD
    roads.append(Road(n, float(w), LineString(p), "mountain"))
    art_bufs = [r.line.buffer(max(1.0, r.width / 2 - 1.0)) for r in roads]
    art_tree = STRtree(art_bufs)
    blocked = unary_union([Polygon(pp).buffer(0) for _, pp in L.PARKS] + [Polygon(pp) for pp in L.PLAZAS]
                          + [river.poly.buffer(4)])
    for name, style, poly in L.DISTRICTS:
        area = Polygon(poly).buffer(0).intersection(land).difference(blocked)
        for part in parts(area):
            for r in grid_streets(part, style, name, rng):
                # keep only the bits outside the arterials' right-of-way (they join at its edge)
                near = art_tree.query(r.line)
                g = r.line.difference(unary_union([art_bufs[i] for i in near])) if len(near) else r.line
                for seg in getattr(g, "geoms", [g]):
                    if isinstance(seg, LineString) and seg.length > 10:
                        roads.append(Road(r.name, r.width, seg, r.kind, r.district))
    # Right-of-way, carriageway and medians.
    whole = unary_union([r.line.buffer(r.width / 2, cap_style=1, join_style=1) for r in roads])
    whole = whole.buffer(4.0, join_style=1).buffer(-4.0, join_style=1)  # kerb returns at corners
    carr = unary_union([r.line.buffer(r.carriage / 2, cap_style=1, join_style=1) for r in roads])
    carr = carr.buffer(3.0, join_style=1).buffer(-3.0, join_style=1)
    med_parts = []
    line_tree = STRtree([r.line for r in roads])
    for i, r in enumerate(roads):
        if r.median <= 0:
            continue
        m = r.line.buffer(r.median / 2, cap_style=2)
        near = [roads[j] for j in line_tree.query(r.line.buffer(1.0)) if roads[j] is not r]
        if near:
            m = m.difference(unary_union([o.line.buffer(o.carriage / 2 + 5.0) for o in near]))
        med_parts.append(m)
    median = unary_union(med_parts) if med_parts else Polygon()
    carr = carr.difference(median)
    land_c = land.buffer(0)
    over_water = unary_union([r.line.buffer(r.width / 2 + 1, cap_style=2) for r in roads if r.name in L.BRIDGE_ROADS])
    keep = land_c.buffer(40).union(over_water)
    whole = whole.intersection(keep)  # only named bridges cross open water
    carr = carr.intersection(whole)
    median = median.intersection(whole)
    plaza = unary_union([Polygon(pp) for pp in L.PLAZAS])
    sidewalk = whole.difference(carr).difference(median).union(plaza)
    return RoadNet(roads, whole.union(plaza), carr, sidewalk, median, plaza)


# --------------------------------------------------------------------------------------------
PARCEL_TARGET = {"center": 330, "business": 3600, "shitamachi": 165, "residential": 290, "bay": 5200,
                 "islet": 6500}


def split_parcels(poly: Polygon, target: float, rng, depth: int = 0) -> list[Polygon]:
    if poly.area <= target * rng.uniform(0.75, 1.35) or depth > 14:
        return [poly]
    rect = poly.minimum_rotated_rectangle
    cs = np.asarray(rect.exterior.coords)[:4]
    e0, e1 = cs[1] - cs[0], cs[2] - cs[1]
    long_axis = e0 if np.linalg.norm(e0) >= np.linalg.norm(e1) else e1
    length = np.linalg.norm(long_axis)
    if length < 6:
        return [poly]
    ax = long_axis / length
    ctr = np.asarray(rect.centroid.coords[0])
    t = rng.uniform(-0.14, 0.14) * length
    p = ctr + ax * t
    nrm = np.array([-ax[1], ax[0]])
    cut = LineString([p - nrm * 1e4, p + nrm * 1e4])
    try:
        pieces = split(poly, cut)
    except Exception:
        return [poly]
    out = []
    for g in pieces.geoms:
        if isinstance(g, Polygon) and g.area > 4:
            out.extend(split_parcels(g, target, rng, depth + 1))
    return out or [poly]


@dataclass
class Parcel:
    poly: Polygon
    district: str
    style: str
    frontage: float  # length of boundary on the street side
    front_dir: tuple  # unit vector towards the street (outward normal of the longest street edge)


def make_parcels(land: Polygon, net: RoadNet, river: River, reserved: Polygon, rng) -> list[Parcel]:
    blocked = unary_union([net.whole.buffer(0.3), river.poly.buffer(3), reserved]
                          + [Polygon(pp).buffer(0) for _, pp in L.PARKS])
    street = net.whole.buffer(0.3)
    shapely.prepare(street)
    out = []
    for name, style, poly in L.DISTRICTS:
        if style not in PARCEL_TARGET:
            continue
        area = Polygon(poly).buffer(0).intersection(land).difference(blocked)
        for block in parts(area):
            if block.area < 40:
                continue
            for pc in split_parcels(block, PARCEL_TARGET[style], rng):
                if pc.area < 25:
                    continue
                mrr = pc.minimum_rotated_rectangle
                cs = np.asarray(mrr.exterior.coords)[:4]
                if min(np.linalg.norm(cs[1] - cs[0]), np.linalg.norm(cs[2] - cs[1])) < 3.5:
                    continue
                # frontage: boundary edges whose outside lies in the street right-of-way
                pc = shapely.geometry.polygon.orient(pc, 1.0)  # CCW: outward normal = right of edge
                coords = np.asarray(pc.exterior.coords)
                a2, c2 = coords[:-1], coords[1:]
                d = c2 - a2
                ln = np.linalg.norm(d, axis=1)
                ok = ln > 0.8
                nrm = np.zeros_like(d)
                nrm[ok] = np.column_stack([d[ok, 1], -d[ok, 0]]) / ln[ok, None]
                m = (a2 + c2) / 2 + nrm * 1.2
                on = ok & shapely.contains_xy(street, m[:, 0], m[:, 1])
                flen = float(ln[on].sum())
                fdir = (0.0, 0.0)
                if on.any():
                    k = int(np.argmax(np.where(on, ln, 0)))
                    fdir = (float(nrm[k, 0]), float(nrm[k, 1]))
                out.append(Parcel(pc, name, style, flen, fdir))
    return out


# --------------------------------------------------------------------------------------------
@dataclass
class Island:
    land: Polygon
    islet: Polygon
    river: River
    terrain: Terrain
    net: RoadNet
    parcels: list[Parcel]
    reserved: Polygon  # stations, landmarks (special buildings)


def smooth_path(pts, it=4, closed=False) -> np.ndarray:
    """Chaikin corner cutting (the rail lines are laid out as smooth curves)."""
    P = np.asarray(pts, float)
    for _ in range(it):
        Q = [] if closed else [P[0]]
        n = len(P)
        for i in (range(n) if closed else range(n - 1)):
            a, b = P[i], P[(i + 1) % n]
            Q += [0.75 * a + 0.25 * b, 0.25 * a + 0.75 * b]
        if not closed:
            Q.append(P[-1])
        P = np.array(Q)
    return P


def rail_corridors() -> Polygon:
    loop = smooth_path(L.RAIL_LOOP[:-1], 4, True)
    # clearance either side of the viaducts (tracks + a service strip / frontage road, as along
    # Tokyo's elevated lines); buildings start beyond it
    parts_ = [LineString(np.vstack([loop, loop[:1]])).buffer(11.0),
              LineString(smooth_path(L.RAIL_BRANCH, 4)).buffer(10.0),
              LineString(smooth_path(L.SHINKANSEN, 4)).buffer(14.0)]
    for name, x, y, hd in list(L.STATIONS) + list(L.SHINKANSEN_STATIONS):
        parts_.append(affinity.rotate(box(x - 24, y - 170, x + 24, y + 170), -hd, origin=(x, y)))
    return unary_union(parts_)


def generate(seed: int = 20260927) -> Island:
    rng = np.random.default_rng(seed)
    land = Polygon(L.COAST).buffer(0).union(Polygon(L.AIRPORT).buffer(0))
    islet = Polygon(L.ISLET).buffer(0)
    river = make_river()
    terrain = Terrain(land, islet, river, seed=seed % 1000)
    all_land = land.union(islet)
    net = build_roads(all_land, river, rng)
    reserved = []
    for name, x, y, hd in L.STATIONS:
        reserved.append(affinity.rotate(box(x - 30, y - 110, x + 30, y + 110), -hd, origin=(x, y)))
    for key, (name, x, y) in L.LANDMARKS.items():
        r = {"temple": 110, "shrine": 45, "tower": 45, "wheel": 60, "stadium": 140}[key]
        reserved.append(Point(x, y).buffer(r))
    reserved.append(rail_corridors())
    reserved_u = unary_union(reserved).difference(net.whole)
    parcels = make_parcels(all_land, net, river, reserved_u, rng)
    return Island(land, islet, river, terrain, net, parcels, reserved_u)
