"""Street-level detail for one cell (RJDET v1).

Everything here is derived from a real PLATEAU source object; nothing is placed without one:
  * raised sidewalks + 15 cm curbs from tran LOD2 TrafficArea (歩道部) and traffic islands (島)
  * crisp road-marking meshes from frn 1000-1299 (区画線, 横断歩道, 停止線, 規制標示)
  * street furniture from frn LOD3 geometry, with a material per CityFurniture function code
  * runtime metadata: street-light heads (frn 4200) and traffic-signal heads (frn 4900) with
    their orientation, intersection group and phase (phases are a game-side assumption: two
    orthogonal phases per intersection; real signal timings are not public data)

RJDET v1 = b"RJDET001" + u32 body_len + raw-DEFLATE(body)
body (all positions in the cell's local ENU frame, metres):
  u32 n_chunks, each:
      u32 material, u32 nv, u32 ni, f32 origin[3], f32 scale
      i16 pos[3*nv] (+pad4)  (pos = origin + q * scale)
      i8  nrm[4*nv]
      f16 uv[2*nv]            (material-local UV; tileable materials use world UV in the shader)
      u8  rgba[4*nv]
      u16 idx[ni] (+pad4)
  u32 n_walk_tris  + f32[9n]  extra walkable surfaces (0 today: sidewalk/island chunks are walkable)
  u32 n_lights     + n * (f32 pos[3], f32 range, u32 kind)
  u32 n_signals    + n * (f32 pos[3], f32 axis_yaw, f32 facing_yaw, f32 length, u32 kind, i32 group, u32 phase)
  u32 n_cross_tris + f32[9n]  crosswalk areas (frn 1110)
  u32 ao_png_len + PNG        ground contact-occlusion raster (grey, same UV as the ground texture)
  u32 n_trees + n * (f32 base[3], f32 height, f32 crown_radius, u32 kind)   PLATEAU veg (kind 0 tree, 1 shrub area centroid)
  u32 n_hedge_tris + f32[9n]  PlantCover footprints (low planting)
Yaw angles are compass headings in radians (0 = north, clockwise).
"""

from __future__ import annotations

import math
import struct
import zlib
from dataclasses import dataclass, field

import numpy as np
from shapely.geometry import MultiPolygon, Point, Polygon
from shapely.geometry.polygon import orient
from shapely.ops import unary_union
from shapely.prepared import prep

# Material ids shared with the client shader (client/src/render/materials.hpp).
MAT = {"curb": 1, "sidewalk": 2, "tactile": 3, "marking": 4, "metal": 5, "metal_dark": 6, "sign": 7,
       "manhole": 8, "grating": 9, "lamp": 10, "glass": 11, "concrete": 12, "fence": 13, "bronze": 14,
       "island": 15, "water": 16, "canopy": 17, "asphalt": 18, "ballast": 19}

CURB_H = 0.15      # Japanese standard mountable-kerb height for sidewalks (歩道の縁石) is 0.15 m
MARK_LIFT = 0.018  # paint sits just above the terrain surface
MAX_EDGE = 3.0     # subdivision so raised surfaces follow the terrain
CURB_SEG = 2.0     # kerb segment length (terrain following)

# frn function -> (material, rgb). Colours follow common Japanese practice for the class;
# the actual sign faces / text are not in the data and are not invented.
FRN_STYLE = {
    2000: ("fence", (214, 214, 208)),      # 柵・壁 (guard pipes / fences, usually white-painted)
    3110: ("sign", (32, 84, 160)),         # 案内標識 (general-road guide signs are blue)
    3130: ("sign", (200, 44, 40)),         # 規制標識
    3140: ("sign", (36, 92, 176)),         # 指示標識
    3150: ("sign", (236, 236, 232)),       # 補助標識
    4020: ("concrete", (170, 168, 162)),   # 地下出入口 (stair enclosures)
    4100: ("sign", (230, 120, 40)),        # 視線誘導標
    4200: ("metal", (150, 152, 156)),      # 照明施設 (heads get the lamp material underneath)
    4203: ("bronze", (96, 72, 48)),        # 立像
    4300: ("metal", (120, 124, 128)),      # 道路情報管理施設
    4800: ("metal", (140, 142, 146)),      # 柱
    4900: ("metal_dark", (52, 54, 58)),    # 交通信号機
    5200: ("metal", (138, 150, 140)),      # 電線共同溝 (ground-level utility boxes)
    5610: ("manhole", (92, 90, 86)),       # マンホール
    7300: ("grating", (70, 70, 70)),       # 側溝
    8010: ("metal", (60, 110, 170)),       # 停留所
    8030: ("sign", (200, 30, 36)),         # 郵便ポスト (Japan Post red)
    8040: ("glass", (120, 150, 130)),      # 電話ボックス
    8150: ("tactile", (230, 190, 40)),     # 点字ブロック
    9000: ("concrete", (160, 158, 152)),   # その他
    9001: ("sign", (230, 230, 226)),       # 看板（自立式）
}
MARKING_CODES = range(1000, 1300)


class TerrainSampler:
    """Heights on the cell terrain grid, identical to the rendered terrain and World::terrainHeight."""

    def __init__(self, hz: np.ndarray, lat0: float, lon0: float, dlat: float, dlon: float, frame):
        self.hz = hz
        self.ny, self.nx = hz.shape
        p00 = np.array(frame.to_local(lat0, lon0, 0.0)[:2])
        self.p00 = p00
        self.ex = np.array(frame.to_local(lat0, lon0 + dlon, 0.0)[:2]) - p00
        self.ey = np.array(frame.to_local(lat0 + dlat, lon0, 0.0)[:2]) - p00
        self.det = self.ex[0] * self.ey[1] - self.ex[1] * self.ey[0]

    def __call__(self, x, y):
        x = np.asarray(x, dtype=np.float64)
        y = np.asarray(y, dtype=np.float64)
        vx, vy = x - self.p00[0], y - self.p00[1]
        fj = np.clip((vx * self.ey[1] - vy * self.ey[0]) / self.det, 0, self.nx - 1)
        fi = np.clip((self.ex[0] * vy - self.ex[1] * vx) / self.det, 0, self.ny - 1)
        i = np.minimum(fi.astype(int), self.ny - 2)
        j = np.minimum(fj.astype(int), self.nx - 2)
        u, w = fi - i, fj - j
        Z = self.hz
        # Same triangulation as the client terrain mesh: quads split along (i,j)-(i+1,j+1).
        a, b, d, e = Z[i, j], Z[i, j + 1], Z[i + 1, j], Z[i + 1, j + 1]
        upper = a + w * (b - a) + u * (e - b)   # triangle (a, b, e): w >= u
        lower = a + u * (d - a) + w * (e - d)   # triangle (a, e, d): u >  w
        return np.where(w >= u, upper, lower)


@dataclass
class Geo:
    pos: list = field(default_factory=list)
    nrm: list = field(default_factory=list)
    uv: list = field(default_factory=list)
    col: list = field(default_factory=list)
    idx: list = field(default_factory=list)
    n: int = 0

    def add(self, pos, nrm, uv, col, tris):
        pos = np.asarray(pos, np.float64)
        self.pos.append(pos)
        self.nrm.append(np.broadcast_to(np.asarray(nrm, np.float64), pos.shape).copy() if np.ndim(nrm) == 1 else np.asarray(nrm))
        self.uv.append(np.zeros((len(pos), 2)) if uv is None else np.asarray(uv, np.float64))
        c = np.asarray(col, np.uint8)
        self.col.append(np.broadcast_to(c, (len(pos), 4)).copy() if c.ndim == 1 else c)
        self.idx.append(np.asarray(tris, np.int64).reshape(-1) + self.n)
        self.n += len(pos)


def _earcut2d(poly: Polygon):
    import mapbox_earcut as earcut
    rings = [np.asarray(poly.exterior.coords)[:-1, :2]] + [np.asarray(r.coords)[:-1, :2] for r in poly.interiors]
    verts = np.vstack(rings)
    ends = np.cumsum([len(r) for r in rings]).astype(np.uint32)
    idx = earcut.triangulate_float64(verts, ends)
    return verts, np.asarray(idx, np.int64).reshape(-1, 3)


def _subdivide(verts: np.ndarray, tris: np.ndarray, max_edge: float):
    """Split triangles on their longest edge until every edge <= max_edge (shared midpoints)."""
    verts = [tuple(v) for v in verts]
    mids: dict[tuple[int, int], int] = {}
    out = []
    stack = [tuple(t) for t in tris]
    while stack:
        a, b, c = stack.pop()
        pa, pb, pc = verts[a], verts[b], verts[c]
        e = [(math.dist(pa, pb), a, b, c), (math.dist(pb, pc), b, c, a), (math.dist(pc, pa), c, a, b)]
        L, i, j, k = max(e)
        if L <= max_edge:
            out.append((a, b, c))
            continue
        key = (min(i, j), max(i, j))
        m = mids.get(key)
        if m is None:
            m = len(verts)
            verts.append(((verts[i][0] + verts[j][0]) / 2, (verts[i][1] + verts[j][1]) / 2))
            mids[key] = m
        stack.append((i, m, k))
        stack.append((m, j, k))
    return np.array(verts), np.array(out, np.int64)


def _parts(g):
    if g.is_empty:
        return []
    if isinstance(g, Polygon):
        return [g]
    if isinstance(g, MultiPolygon):
        return list(g.geoms)
    return [p for p in getattr(g, "geoms", []) if isinstance(p, Polygon)]


def _local_poly(frame, to_local, poly) -> Polygon | None:
    ext = to_local(frame, poly.exterior)[:, :2]
    holes = [to_local(frame, h)[:, :2] for h in poly.interiors]
    try:
        p = Polygon(ext, holes).buffer(0)
    except Exception:
        return None
    return p if not p.is_empty and p.area > 0.05 else None


def build_sidewalks(roads, footprints_local, frame, to_local, terrain: TerrainSampler, geos: dict, walk_tris: list):
    """Raised sidewalks/islands. Kerbs (15 cm) only where the raised area meets the carriageway;
    towards plazas and private land the edge ramps down flush (the survey has no kerb there);
    nothing against building walls. Returns the raised-area union (for furniture snapping)."""
    polys = {"sidewalk": [], "median": [], "road": []}
    for r in roads:
        for p in r.detail:
            if p.kind in polys:
                lp = _local_poly(frame, to_local, p)
                if lp is not None:
                    polys[p.kind].append(lp)
    blds = unary_union([Polygon(fp).buffer(0) for fp in footprints_local if len(fp) >= 3]) if footprints_local else Polygon()
    bprep = prep(blds.buffer(0.05)) if not blds.is_empty else None
    road_u = unary_union(polys["road"]).buffer(0.05) if polys["road"] else Polygon()
    rprep = prep(road_u) if not road_u.is_empty else None
    unions = {}
    for kind in ("sidewalk", "median"):
        if polys[kind]:
            U = unary_union(polys[kind]).buffer(0)
            if not blds.is_empty:
                U = U.difference(blds)
            unions[kind] = U
    raised_u = unary_union(list(unions.values())) if unions else Polygon()
    raised_prep = prep(raised_u.buffer(-0.02)) if not raised_u.is_empty else None
    raised_all = []
    for kind, mat in (("sidewalk", "sidewalk"), ("median", "island")):
        if kind not in unions:
            continue
        for part in _parts(unions[kind]):
            part = orient(part.simplify(0.02), 1.0)
            if part.area < 0.3:
                continue
            raised_all.append(part)
            try:
                v2, t = _earcut2d(part)
            except Exception:
                continue
            if len(t) == 0:
                continue
            v2, t = _subdivide(v2, t, globals()["MAX_EDGE"])
            z = terrain(v2[:, 0], v2[:, 1]) + CURB_H
            V = np.column_stack([v2, z])
            geos.setdefault(mat, Geo()).add(V, (0.0, 0.0, 1.0), None, (255, 255, 255, 255), t)
            walk_tris.append(V[t])
            for ring in [part.exterior] + list(part.interiors):
                pts = np.asarray(ring.coords)[:, :2]
                _curb_ring(pts, terrain, bprep, rprep, raised_prep, geos, mat)
    return unary_union(raised_all) if raised_all else Polygon()


def _curb_ring(pts: np.ndarray, terrain, bprep, rprep, raised_prep, geos, surface_mat: str):
    g = geos.setdefault("curb", Geo())
    gs = geos.setdefault(surface_mat, Geo())
    for k in range(len(pts) - 1):
        a, b = pts[k], pts[k + 1]
        L = float(np.hypot(*(b - a)))
        if L < 0.05:
            continue
        d = (b - a) / L
        out = np.array([d[1], -d[0]])  # right-hand side = outside for CCW exteriors / CW holes
        mid = (a + b) / 2
        probe = Point(*(mid + out * 0.35))
        if bprep is not None and bprep.contains(probe):
            continue  # against a wall
        if raised_prep is not None and raised_prep.contains(probe):
            continue  # another raised area (island next to sidewalk etc.)
        n = max(1, int(math.ceil(L / CURB_SEG)))
        s = np.linspace(0.0, 1.0, n + 1)
        P = a[None, :] + (b - a)[None, :] * s[:, None]
        tz = terrain(P[:, 0], P[:, 1])
        if rprep is None or not rprep.contains(probe):
            # Flush transition towards plazas / private ground: short sloped apron, no kerb.
            Po = P + out[None, :] * 0.6
            V = np.vstack([np.column_stack([P, tz + CURB_H]), np.column_stack([Po, terrain(Po[:, 0], Po[:, 1]) + 0.004])])
            T = []
            for i in range(n):
                T += [(i, n + 1 + i + 1, i + 1), (i, n + 1 + i, n + 1 + i + 1)]
            nz = np.array([out[0] * 0.24, out[1] * 0.24, 0.97])
            gs.add(V, nz / np.linalg.norm(nz), None, (255, 255, 255, 255), T)
            continue
        # Kerb: vertical face (terrain - 5 cm .. kerb top) + 15 cm kerb-stone top band.
        V = np.vstack([np.column_stack([P, tz - 0.05]), np.column_stack([P, tz + CURB_H])])
        T = []
        for i in range(n):
            T += [(i, i + 1, n + 1 + i + 1), (i, n + 1 + i + 1, n + 1 + i)]
        uv = np.column_stack([np.concatenate([s * L, s * L]), np.concatenate([np.zeros(n + 1), np.full(n + 1, CURB_H)])])
        g.add(V, (out[0], out[1], 0.0), uv, (255, 255, 255, 255), T)
        Pi = P - out[None, :] * 0.15
        V2 = np.vstack([np.column_stack([P, tz + CURB_H + 0.003]), np.column_stack([Pi, terrain(Pi[:, 0], Pi[:, 1]) + CURB_H + 0.003])])
        T2 = []
        for i in range(n):
            T2 += [(i, n + 1 + i + 1, i + 1), (i, n + 1 + i, n + 1 + i + 1)]
        uv2 = np.column_stack([np.concatenate([s * L, s * L]), np.concatenate([np.zeros(n + 1), np.full(n + 1, 0.15)])])
        g.add(V2, (0.0, 0.0, 1.0), uv2, (255, 255, 255, 255), T2)


def build_markings(markings, frame, to_local, triangulate, terrain, raised, geos, cross_tris: list, codes: list):
    """Road markings as thin decal meshes following the terrain (crisp at eye level)."""
    rprep = prep(raised) if not raised.is_empty else None
    g = geos.setdefault("marking", Geo())
    for poly, code in zip(markings, codes):
        ext = to_local(frame, poly.exterior)
        holes = [to_local(frame, h) for h in poly.interiors]
        v, t, _ = triangulate(ext, holes)
        if len(t) == 0:
            continue
        v = v.copy()
        lift = MARK_LIFT
        c = v[:, :2].mean(axis=0)
        if rprep is not None and rprep.contains(Point(*c)):
            lift += CURB_H
        v[:, 2] = terrain(v[:, 0], v[:, 1]) + lift
        g.add(v, (0.0, 0.0, 1.0), None, (238, 238, 232, 255), t)
        if code == 1110:
            cross_tris.append(v[t])


def _snap_offsets(furn_local, terrain, raised):
    """Survey heights vs our terrain: grounded objects snap their base to the (raised) surface;
    floating parts (lamp heads, signal heads, arms) reuse the offset of the nearest grounded object."""
    rprep = prep(raised) if not raised.is_empty else None
    grounded = []
    offs = [None] * len(furn_local)
    for i, (fn, V) in enumerate(furn_local):
        c = V[:, :2].mean(axis=0)
        zmin = V[:, 2].min()
        tz = float(terrain(c[0], c[1]))
        surf = tz + (CURB_H if rprep is not None and rprep.contains(Point(*c)) else 0.0)
        if abs(zmin - surf) < 1.2:
            offs[i] = surf - zmin
            grounded.append((c, offs[i]))
    if grounded:
        G = np.array([g[0] for g in grounded])
        O = np.array([g[1] for g in grounded])
        med = float(np.median(O))
        for i, (fn, V) in enumerate(furn_local):
            if offs[i] is None:
                c = V[:, :2].mean(axis=0)
                d = np.hypot(G[:, 0] - c[0], G[:, 1] - c[1])
                j = int(np.argmin(d))
                offs[i] = float(O[j]) if d[j] < 12.0 else med
    return [o or 0.0 for o in offs]


def _pca_yaw(xy: np.ndarray) -> tuple[float, float, float]:
    """(axis yaw of the longest horizontal extent, extent along it, extent across it)."""
    c = xy - xy.mean(axis=0)
    w, v = np.linalg.eigh(np.cov(c.T) + np.eye(2) * 1e-9)
    ax = v[:, 1]
    along = c @ ax
    across = c @ np.array([-ax[1], ax[0]])
    yaw = math.atan2(ax[0], ax[1])  # compass: x = east, y = north
    return yaw, float(along.max() - along.min()), float(across.max() - across.min())


def build_furniture(furniture, frame, to_local, triangulate, terrain, raised, geos, lights: list, signals: list):
    items = []
    for f in furniture:
        if f.function in MARKING_CODES or not f.polys:
            continue
        rings = [(to_local(frame, p.exterior), [to_local(frame, h) for h in p.interiors]) for p in f.polys]
        V = np.vstack([r[0] for r in rings])
        items.append((f.function, V, rings))
    offs = _snap_offsets([(fn, V) for fn, V, _ in items], terrain, raised)
    for (fn, V, rings), off in zip(items, offs):
        mat, rgb = FRN_STYLE.get(fn, ("concrete", (150, 150, 146)))
        zmin = V[:, 2].min() + off
        zmax = V[:, 2].max() + off
        c2 = V[:, :2].mean(axis=0)
        ground = float(terrain(c2[0], c2[1]))
        is_head = (zmax - zmin) < 0.9 and (zmin - ground) > 2.2
        for ext, holes in rings:
            v, t, n = triangulate(ext, holes)
            if len(t) == 0:
                continue
            v = v.copy()
            v[:, 2] += off
            m = mat
            if fn == 4200 and is_head and n[2] < -0.3:
                m = "lamp"  # emissive underside of a street-light head
            uv = None
            if m == "manhole":
                bb0, bb1 = V[:, :2].min(axis=0), V[:, :2].max(axis=0)
                uv = (v[:, :2] - bb0) / np.maximum(bb1 - bb0, 1e-3)
                v[:, 2] = np.maximum(v[:, 2], terrain(v[:, 0], v[:, 1]) + 0.012)
            geos.setdefault(m, Geo()).add(v, n, uv, rgb + (255,), t)
        if fn == 4200 and is_head:
            lights.append((np.array([c2[0], c2[1], zmin - 0.15]), 16.0, 0))
        if fn == 4900 and (zmin - ground) > 1.5:
            yaw, along, across = _pca_yaw(V[:, :2])
            height = zmax - zmin
            kind = 0 if (along > 0.9 and height < 0.62) else 1  # 0 vehicle (horizontal 3-lamp), 1 pedestrian
            ax = np.array([math.sin(yaw), math.cos(yaw)])
            perp = np.array([ax[1], -ax[0]])
            proj = (V[:, :2] - c2) @ perp
            # Hoods (visors) stick out of the lamp face: the vertex mass leans to the front.
            facing_v = perp if (proj.mean() - (proj.max() + proj.min()) / 2) >= 0 else -perp
            if kind == 1:
                # Pedestrian heads are tall boxes: face along the shorter horizontal extent's normal.
                facing_v = facing_v
            facing = math.atan2(facing_v[0], facing_v[1])
            signals.append([np.array([c2[0], c2[1], (zmin + zmax) / 2]), yaw, facing, along if kind == 0 else height, kind])


def group_signals(signals: list) -> None:
    """Cluster signal heads into intersections and assign two orthogonal phases (game assumption)."""
    n = len(signals)
    group = [-1] * n
    g = 0
    for i in range(n):
        if group[i] >= 0:
            continue
        stack = [i]
        group[i] = g
        while stack:
            a = stack.pop()
            for b in range(n):
                if group[b] < 0 and np.hypot(*(signals[a][0][:2] - signals[b][0][:2])) < 35.0:
                    group[b] = g
                    stack.append(b)
        g += 1
    for gi in range(g):
        members = [i for i in range(n) if group[i] == gi]
        veh = [i for i in members if signals[i][4] == 0]
        ref = signals[veh[0]][2] if veh else signals[members[0]][2]
        for i in members:
            d = abs(((signals[i][2] - ref + math.pi / 2) % math.pi) - math.pi / 2)  # angle between axes mod 180
            ph = 0 if d < math.pi / 4 else 1
            signals[i] += [gi, ph]


VEG = "{http://www.opengis.net/citygml/vegetation/2.0}"
GML = "{http://www.opengis.net/gml}"


def parse_vegetation(path: str, frame, to_local, terrain) -> tuple[list, list]:
    """PLATEAU veg: SolitaryVegetationObject -> (base, height, crown radius); PlantCover -> footprint tris."""
    import xml.etree.ElementTree as ET
    from .citygml import _parse_poslist
    trees, hedges = [], []
    for ev, el in ET.iterparse(path, events=("end",)):
        tag = el.tag
        if tag == VEG + "SolitaryVegetationObject":
            pts = []
            for pl in el.iter(GML + "posList"):
                pts.extend(_parse_poslist(pl.text))
            if pts:
                L = to_local(frame, pts)
                c = L[:, :2].mean(axis=0)
                zmin, zmax = L[:, 2].min(), L[:, 2].max()
                r = float(np.max(np.hypot(L[:, 0] - c[0], L[:, 1] - c[1])))
                base = np.array([c[0], c[1], float(terrain(c[0], c[1]))])
                trees.append((base, float(max(zmax - zmin, 2.0)), max(r, 0.8), 0))
            el.clear()
        elif tag == VEG + "PlantCover":
            for poly in el.iter(GML + "Polygon"):
                ext = poly.find(f"{GML}exterior/{GML}LinearRing/{GML}posList")
                ring = _parse_poslist(ext.text if ext is not None else None)
                if len(ring) < 3:
                    continue
                L = to_local(frame, ring)
                if np.ptp(L[:, 2]) > 0.3:  # keep the horizontal footprint faces only
                    continue
                try:
                    part = Polygon(L[:, :2]).buffer(0)
                except Exception:
                    continue
                for pp in _parts(part):
                    v2, t = _earcut2d(orient(pp, 1.0))
                    if len(t):
                        V = np.column_stack([v2, terrain(v2[:, 0], v2[:, 1])])
                        hedges.append(V[t])
            el.clear()
    return trees, hedges


def encode(geos: dict, walk_tris: list, lights: list, signals: list, cross_tris: list, ao_png: bytes = b"",
           trees: list = (), hedges: list = ()) -> tuple[bytes, dict]:
    body = bytearray()
    chunks = []
    for name, g in geos.items():
        if g.n == 0:
            continue
        P = np.vstack(g.pos)
        N = np.vstack(g.nrm)
        UV = np.vstack(g.uv)
        C = np.vstack(g.col)
        I = np.concatenate(g.idx)
        tris = I.reshape(-1, 3)
        # Split into spatial tiles (256 m) and <= 65535 vertices per chunk.
        cen = P[tris].mean(axis=1)
        tile = np.floor(cen[:, :2] / 256.0).astype(np.int64)
        keys = tile[:, 0] * 100003 + tile[:, 1]
        for k in np.unique(keys):
            tsel = tris[keys == k]
            start = 0
            while start < len(tsel):
                sub = tsel[start:start + 20000]
                start += 20000
                used, inv = np.unique(sub.reshape(-1), return_inverse=True)
                chunks.append((MAT[name], P[used], N[used], UV[used], C[used], inv.astype(np.uint16)))
    body += struct.pack("<I", len(chunks))
    nv_total = 0
    for mat, P, N, UV, C, I in chunks:
        o = P.min(axis=0)
        ext = float((P - o).max()) if len(P) else 1.0
        scale = max(ext / 65000.0, 1e-5)
        q = np.clip(np.round((P - o) / scale) - 32500, -32767, 32767).astype(np.int16)
        o = o + 32500 * scale
        body += struct.pack("<3I", mat, len(P), len(I)) + struct.pack("<4f", *o, scale)
        qb = q.tobytes()
        body += qb + b"\0" * ((4 - len(qb) % 4) % 4)
        qn = np.zeros((len(N), 4), np.int8)
        ln = np.linalg.norm(N, axis=1, keepdims=True)
        qn[:, :3] = np.clip(np.round(N / np.maximum(ln, 1e-9) * 127), -127, 127).astype(np.int8)
        body += qn.tobytes()
        body += UV.astype(np.float16).tobytes()
        body += C.astype(np.uint8).tobytes()
        ib = I.tobytes()
        body += ib + b"\0" * ((4 - len(ib) % 4) % 4)
        nv_total += len(P)
    W = np.concatenate(walk_tris).astype(np.float32) if walk_tris else np.zeros((0, 3, 3), np.float32)
    body += struct.pack("<I", len(W)) + W.tobytes()
    body += struct.pack("<I", len(lights))
    for p, r, k in lights:
        body += struct.pack("<4fI", *p, r, k)
    body += struct.pack("<I", len(signals))
    for p, yaw, facing, length, kind, group, phase in signals:
        body += struct.pack("<6fIiI", *p, yaw, facing, length, kind, group, phase)
    X = np.concatenate(cross_tris).astype(np.float32) if cross_tris else np.zeros((0, 3, 3), np.float32)
    body += struct.pack("<I", len(X)) + X.tobytes()
    body += struct.pack("<I", len(ao_png)) + ao_png
    body += struct.pack("<I", len(trees))
    for base, h, r, k in trees:
        body += struct.pack("<5fI", *base, h, r, k)
    H = np.concatenate(hedges).astype(np.float32) if hedges else np.zeros((0, 3, 3), np.float32)
    body += struct.pack("<I", len(H)) + H.tobytes()
    comp = zlib.compressobj(9, zlib.DEFLATED, -15)
    data = comp.compress(bytes(body)) + comp.flush()
    stats = {"chunks": len(chunks), "vertices": nv_total, "walk_tris": len(W), "lights": len(lights), "trees": len(trees),
             "signals": len(signals), "crosswalk_tris": len(X)}
    return b"RJDET001" + struct.pack("<I", len(body)) + data, stats
