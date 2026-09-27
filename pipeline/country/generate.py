"""Procedural generation of 秋津国 (fictional) from the hand-made layout (layout.py).

Deterministic for a given seed. Produces, in country-local metres (x east, y north, z up):
terrain heights (terrain.py), water (sea, rivers, a lake), road centerlines and polygons
(carriageway / sidewalk / median / plaza), town blocks split into parcels, land cover. Buildings
are made in buildings.py, special structures in specials.py.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np
import shapely
import shapely.geometry.polygon
from scipy import ndimage
from shapely import affinity
from shapely.geometry import LineString, MultiPolygon, Point, Polygon, box
from shapely.ops import split, unary_union
from shapely.strtree import STRtree

from . import layout as L
from .terrain import BOUNDS, CountryTerrain, fbm, land_polygons, line_distance, make_rivers, smoothstep, value_noise


def parts(g):
    if g is None or g.is_empty:
        return []
    if isinstance(g, Polygon):
        return [g]
    return [p for p in getattr(g, "geoms", []) if isinstance(p, Polygon) and not p.is_empty]


# --------------------------------------------------------------------------------------------
@dataclass
class Road:
    name: str
    width: float
    line: LineString
    kind: str  # arterial | street | alley | mountain | rural | lane
    district: str = ""

    @property
    def sidewalk(self) -> float:
        if self.kind in ("rural", "mountain", "lane"):
            return 0.0
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
    # the old capital: a strict north-south grid of narrow streets (machiya blocks)
    "kyoto": (0, 118, 118, 1.5, [(7, 0.45), (10, 0.25), (5, 0.3)], 0.02, 0),
    "kyoto_center": (0, 118, 118, 1.5, [(12, 0.5), (16, 0.3), (8, 0.2)], 0.0, 0),
    "port": (-4, 170, 125, 0, [(16, 1.0)], 0.12, 0),
    "onsen": (20, 70, 52, 8, [(6, 0.6), (4.5, 0.4)], 0.15, 10),
    "snowtown": (8, 95, 80, 6, [(9, 0.5), (6.5, 0.5)], 0.08, 8),
    "village": (15, 125, 105, 18, [(5, 0.65), (4, 0.35)], 0.42, 26),
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
                kind = "lane" if style == "village" else ("alley" if w < 5.5 else "street")
                out.append(Road("", float(w), part, kind, district))
    return out


@dataclass
class RoadPolys:
    whole: Polygon          # full right-of-way (carriageway + sidewalks + medians)
    carriage: Polygon
    sidewalk: Polygon
    median: Polygon
    plaza: Polygon


class RoadNet:
    """Road centre lines, and their surface polygons built locally on demand (a country-wide union
    of every road would be far too slow): polys(area) covers `area` exactly as a global build would,
    since every road within reach of it is included and the corner rounding is local."""

    def __init__(self, roads, land, plazas):
        self.roads = roads
        self.tree = STRtree([r.line for r in roads])
        self.land = land
        self.plaza = unary_union([Polygon(pp) for pp in plazas]) if plazas else Polygon()

    def near(self, geom, dist=0.0):
        return [self.roads[i] for i in self.tree.query(geom.buffer(dist) if dist else geom)]

    def polys(self, area) -> RoadPolys:
        reach = area.buffer(90)
        roads = self.near(reach)
        if not roads:
            e = Polygon()
            return RoadPolys(e, e, e, e, self.plaza.intersection(area) if not self.plaza.is_empty else e)
        whole = unary_union([r.line.buffer(r.width / 2, cap_style=1, join_style=1) for r in roads])
        whole = whole.buffer(4.0, join_style=1).buffer(-4.0, join_style=1)  # kerb returns at corners
        carr = unary_union([r.line.buffer(r.carriage / 2, cap_style=1, join_style=1) for r in roads])
        carr = carr.buffer(3.0, join_style=1).buffer(-3.0, join_style=1)
        med_parts = []
        for r in roads:
            if r.median <= 0:
                continue
            m = r.line.buffer(r.median / 2, cap_style=2)
            nb = [o for o in self.near(r.line, 1.0) if o is not r]
            if nb:
                m = m.difference(unary_union([o.line.buffer(o.carriage / 2 + 5.0) for o in nb]))
            med_parts.append(m)
        median = unary_union(med_parts) if med_parts else Polygon()
        carr = carr.difference(median)
        over_water = unary_union([r.line.buffer(r.width / 2 + 1, cap_style=2) for r in roads if r.name in L.BRIDGE_ROADS])
        keep = self.land.intersection(reach.buffer(50)).buffer(40).union(over_water)
        whole = whole.intersection(keep).intersection(area.buffer(30))  # only named bridges cross open water
        carr = carr.intersection(whole)
        median = median.intersection(whole)
        plaza = self.plaza.intersection(area.buffer(30)) if not self.plaza.is_empty else Polygon()
        sidewalk = whole.difference(carr).difference(median).union(plaza)
        return RoadPolys(whole.union(plaza), carr, sidewalk, median, plaza)


def _towns():
    return unary_union([Polygon(p).buffer(120) for n, s, p in L.DISTRICTS if s not in ("airport",)])


def build_roads(land, rivers, lakes, rng) -> RoadNet:
    roads = [Road(n, float(w), LineString(p), "arterial") for n, w, p in L.ARTERIALS]
    n, w, p = L.MOUNTAIN_ROAD
    roads.append(Road(n, float(w), LineString(p), "mountain"))
    towns = _towns()
    shapely.prepare(towns)
    # national / local roads: sidewalks inside the towns, a plain two-lane road with paved
    # shoulders outside (no sidewalks in the countryside)
    for name, w, pts in L.NATIONAL_ROADS:
        line = LineString(pts)
        r = Road(name, float(w), line, "arterial")
        inside = line.intersection(towns)
        outside = line.difference(towns)
        for seg in getattr(inside, "geoms", [inside]):
            if isinstance(seg, LineString) and seg.length > 5:
                roads.append(Road(name, float(w), seg, "arterial"))
        for seg in getattr(outside, "geoms", [outside]):
            if isinstance(seg, LineString) and seg.length > 5:
                roads.append(Road(name, max(6.5, r.carriage), seg, "rural"))
    art_bufs = [r.line.buffer(max(1.0, r.width / 2 - 1.0)) for r in roads]
    art_tree = STRtree(art_bufs)
    blocked = unary_union([Polygon(pp).buffer(0) for _, pp in L.PARKS] + [Polygon(pp) for pp in L.PLAZAS]
                          + [rv.poly.buffer(4) for rv in rivers] + [Polygon(pp).buffer(10) for _, _, pp in lakes])
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
    return RoadNet(roads, land, L.PLAZAS)


# --------------------------------------------------------------------------------------------
PARCEL_TARGET = {"center": 330, "business": 3600, "shitamachi": 165, "residential": 290, "bay": 5200,
                 "islet": 6500, "kyoto": 380, "kyoto_center": 420, "port": 4200, "onsen": 650, "snowtown": 420,
                 "village": 900}


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


def make_parcels(land: Polygon, net: RoadNet, rivers, reserved: Polygon, rng) -> list[Parcel]:
    other = unary_union([reserved] + [rv.poly.buffer(3) for rv in rivers] + [Polygon(pp).buffer(0) for _, pp in L.PARKS])
    out = []
    import time
    for name, style, poly in L.DISTRICTS:
        if style not in PARCEL_TARGET:
            continue
        t_d = time.time()
        n_before = len(out)
        dpoly = Polygon(poly).buffer(0)
        rp = net.polys(dpoly)
        street = rp.whole.buffer(0.3)
        shapely.prepare(street)
        blocked = unary_union([street, other.intersection(dpoly.buffer(50))])
        area = dpoly.intersection(land).difference(blocked)
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
                if style == "village" and flen <= 0:
                    continue  # farm land behind the houses stays open
                out.append(Parcel(pc, name, style, flen, fdir))
        print(f"    {name} ({style}): {len(out) - n_before} parcels, {time.time() - t_d:.0f}s", flush=True)
    return out


# --------------------------------------------------------------------------------------------
def flatten_for_roads(terrain: CountryTerrain, roads, rivers, skip_box=(-4300, -3600, 4300, 3900)):
    """Road beds: across each road the ground is set to a smoothed, grade-limited profile along it
    (cuts into hillsides, fills over dips), blending back to the natural ground over the verges.
    Not over rivers (the bridges have their own decks) and not in the capital (already gentle)."""
    R = terrain.RES
    water = unary_union([rv.poly.buffer(12) for rv in rivers])
    shapely.prepare(water)
    for r in roads:
        if r.kind not in ("rural", "mountain", "arterial", "lane", "street", "alley"):
            continue
        x0b, y0b, x1b, y1b = r.line.bounds
        if skip_box[0] < x0b and x1b < skip_box[2] and skip_box[1] < y0b and y1b < skip_box[3]:
            continue
        Lr = r.line.length
        n = max(2, int(Lr / 5.0) + 1)
        P = np.array([r.line.interpolate(t, normalized=True).coords[0] for t in np.linspace(0, 1, n)])
        z = terrain.sample(P[:, 0], P[:, 1]).astype(float)
        over = shapely.contains_xy(water, P[:, 0], P[:, 1])
        if over.all():
            continue
        if over.any():  # bridge spans: straight between the banks
            idx = np.arange(n)
            z[over] = np.interp(idx[over], idx[~over], z[~over])
        z = ndimage.gaussian_filter1d(z, 5, mode="nearest")
        g = (0.10 if r.kind in ("mountain", "rural", "lane") else 0.08) * (Lr / max(n - 1, 1))
        for _ in range(2):
            for k in range(1, n):
                z[k] = min(max(z[k], z[k - 1] - g), z[k - 1] + g)
            for k in range(n - 2, -1, -1):
                z[k] = min(max(z[k], z[k + 1] - g), z[k + 1] + g)
        half = r.width / 2.0 + 1.0
        verge = 14.0
        pad = half + verge + 2 * R
        i0 = max(0, int((y0b - pad - terrain.y0) / R))
        i1 = min(terrain.ny, int((y1b + pad - terrain.y0) / R) + 2)
        j0 = max(0, int((x0b - pad - terrain.x0) / R))
        j1 = min(terrain.nx, int((x1b + pad - terrain.x0) / R) + 2)
        if i1 <= i0 or j1 <= j0:
            continue
        wx0, wy0 = terrain.x0 + j0 * R, terrain.y0 + i0 * R
        d, k = line_distance(P, (i1 - i0, j1 - j0), wx0, wy0, R)
        # profile height at the nearest point: interpolate along the segment
        zk = z[np.clip(k, 0, n - 1)]
        zk1 = z[np.clip(k + 1, 0, n - 1)]
        prof = 0.5 * (zk + zk1)
        t = smoothstep(half, half + verge, d)
        win = terrain.h[i0:i1, j0:j1]
        ys = wy0 + np.arange(i1 - i0) * R
        xs = wx0 + np.arange(j1 - j0) * R
        Xw, Yw = np.meshgrid(xs, ys)
        wet = shapely.contains_xy(water, Xw, Yw)
        # Cuts and fills stay within what a road bed does (about 10 m down, 6 m up): where the
        # grade-limited profile cannot follow the ground (a slope steeper than the road may climb) the
        # ground is left as it is instead of being filled into a ridge hundreds of metres high.
        prof = win + np.clip(prof - win, -10.0, 6.0)
        new = (prof * (1.0 - t) + win * t).astype(np.float32)
        terrain.h[i0:i1, j0:j1] = np.where((d < half + verge) & ~wet & (win > 0.5), new, win)


def flatten_pad(terrain: CountryTerrain, poly: Polygon, z: float | None = None, verge: float = 60.0):
    """Level ground under an airport / station yard (to its mean height unless given)."""
    R = terrain.RES
    x0b, y0b, x1b, y1b = poly.bounds
    i0 = max(0, int((y0b - verge - terrain.y0) / R))
    i1 = min(terrain.ny, int((y1b + verge - terrain.y0) / R) + 2)
    j0 = max(0, int((x0b - verge - terrain.x0) / R))
    j1 = min(terrain.nx, int((x1b + verge - terrain.x0) / R) + 2)
    ys = terrain.y0 + np.arange(i0, i1) * R
    xs = terrain.x0 + np.arange(j0, j1) * R
    X, Y = np.meshgrid(xs, ys)
    inside = shapely.contains_xy(poly, X, Y)
    win = terrain.h[i0:i1, j0:j1]
    if z is None:
        z = float(np.mean(win[inside])) if inside.any() else float(np.mean(win))
    dist = ndimage.distance_transform_edt(~inside) * R
    t = smoothstep(0.0, verge, dist)
    terrain.h[i0:i1, j0:j1] = np.where(win > 0.2, (z * (1 - t) + win * t), win).astype(np.float32)
    return z


# --------------------------------------------------------------------------------------------
@dataclass
class Country:
    land: Polygon           # every island incl. reclaimed land (MultiPolygon)
    main: Polygon
    south: Polygon
    islet: Polygon
    rivers: list
    terrain: CountryTerrain
    net: RoadNet
    parcels: list[Parcel]
    reserved: Polygon       # stations, landmarks, rail corridors (special buildings)
    airport_z: list = field(default_factory=list)
    towns: Polygon = None   # built-up districts (yards between the buildings)


def rail_corridors(lines) -> Polygon:
    from .railgeom import stations_aligned, track_piece
    parts_ = []
    for li, P in enumerate(lines):
        kind = L.RAIL_LINES[li]["kind"]
        parts_.append(LineString(P).buffer(14.0 if kind == "shinkansen" else 11.0))
    for name, x, y, hd, li, k in stations_aligned(lines):
        parts_.append(LineString(track_piece(lines[li], k, 170.0, closed=L.RAIL_LINES[li]["closed"])).buffer(24.0))
    return unary_union(parts_)


def generate(seed: int = 20260927, preview: bool = False) -> Country:
    import time
    t0 = time.time()
    rng = np.random.default_rng(seed)
    main, south, islet = land_polygons(seed % 1000)
    rivers = make_rivers()
    terrain = CountryTerrain([main, south, islet], rivers, seed=seed % 1000, res=40.0 if preview else 10.0)
    print(f"  terrain {terrain.nx}x{terrain.ny} ({time.time() - t0:.0f}s)", flush=True)
    land = unary_union([main, south, islet])
    net = build_roads(land, rivers, L.LAKES, rng)
    print(f"  roads: {len(net.roads)} ({time.time() - t0:.0f}s)", flush=True)
    # level ground for the airports (runway strip + apron) before the road beds
    airport_z = []
    for ap in L.AIRPORTS:
        (ax, ay), (bx, by), w = ap["runway"]
        strip = LineString([(ax, ay), (bx, by)]).buffer(150, cap_style=2).union(Point(*ap["terminal"]).buffer(420))
        airport_z.append(flatten_pad(terrain, strip, z=None if ap["reclaimed"] is None else 4.0))
    flatten_for_roads(terrain, net.roads, rivers)
    print(f"  road beds ({time.time() - t0:.0f}s)", flush=True)
    reserved = []
    from .railgeom import rail_lines2d, stations_aligned, track_piece
    lines = rail_lines2d()
    for name, x, y, hd, li, k in stations_aligned(lines):
        if L.RAIL_LINES[li]["kind"] != "shinkansen":
            reserved.append(LineString(track_piece(lines[li], k, 110.0, closed=L.RAIL_LINES[li]["closed"])).buffer(30.0))
    radius = {"temple": 110, "shrine": 45, "tower": 45, "wheel": 60, "stadium": 140, "old_temple": 120,
              "torii_shrine": 60, "castle": 90, "onsen_shrine": 35, "lighthouse": 20, "port_cranes": 160,
              "volcano_hut": 40, "south_shrine": 35}
    for key, (name, x, y) in L.LANDMARKS.items():
        reserved.append(Point(x, y).buffer(radius.get(key, 50)))
    for ap in L.AIRPORTS:
        (ax, ay), (bx, by), w = ap["runway"]
        reserved.append(LineString([(ax, ay), (bx, by)]).buffer(120, cap_style=2))
    reserved.append(rail_corridors(lines))
    reserved_u = unary_union(reserved)
    parcels = make_parcels(land, net, rivers, reserved_u, rng)
    print(f"  parcels: {len(parcels)} ({time.time() - t0:.0f}s)", flush=True)
    towns = unary_union([Polygon(p).buffer(0) for n, s, p in L.DISTRICTS if s not in ("airport", "village")])
    return Country(land, main, south, islet, rivers, terrain, net, parcels, reserved_u, airport_z, towns)
