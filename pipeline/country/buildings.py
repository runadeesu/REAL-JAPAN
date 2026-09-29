"""Buildings of 千景島 (fictional): archetypes per district with procedural-facade parameters.

Geometry is emitted in island-local metres. Faces carry what the client's procedural facade
shader (surfaceMode 3, RJCELL page -2) needs:
  colour rgb = base colour (also seeds per-building variation), alpha = STYLE code
  uv = (u, v) metres / (U_SCALE, V_SCALE): walls u along the facade (1024 = facade centre),
       v above the building's ground; roofs: local plan coordinates / slope coordinates.
Keep STYLE codes in sync with client/src/render/shaders.hpp (facade()).
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field

import mapbox_earcut as earcut
import numpy as np
from shapely.geometry import Polygon
from shapely.geometry.polygon import orient

from .shop import FLOOR_H

U_SCALE, V_SCALE = 2048.0, 1024.0

# ---- style codes (walls 0-99, roofs 100-199, special 200+) ----
PLAIN, PANEL = 0, 1
OFFICE_RIBBON, OFFICE_GRID, CURTAIN, CURTAIN_FIN = 10, 11, 12, 13
COMMERCIAL, PENCIL = 20, 21
APT_FRONT, APT_BACK = 30, 31
HOUSE, WOODEN = 40, 41
WAREHOUSE = 50
PUBLIC = 60
TEMPLE = 70
SCHOOL = 80
ROOF_FLAT, ROOF_TILE, ROOF_METAL, ROOF_HELI = 100, 101, 102, 103
NEON, LED, BILLBOARD, GLASS, METAL, LAMP = 200, 201, 202, 210, 220, 230


@dataclass
class Geo:
    pos: list = field(default_factory=list)
    nrm: list = field(default_factory=list)
    col: list = field(default_factory=list)
    uv: list = field(default_factory=list)
    idx: list = field(default_factory=list)
    n: int = 0

    def add(self, P, N, C, UV, T):
        P = np.asarray(P, float)
        self.pos.append(P)
        self.nrm.append(np.broadcast_to(np.asarray(N, float), P.shape).copy() if np.ndim(N) == 1 else np.asarray(N, float))
        self.col.append(np.repeat(np.asarray([C], np.uint8), len(P), axis=0))
        self.uv.append(np.asarray(UV, float))
        self.idx.append(np.asarray(T, np.int64).reshape(-1) + self.n)
        self.n += len(P)

    def quad(self, a, b, c, d, style, rgb, uv4):
        a, b, c, d = map(np.asarray, (a, b, c, d))
        nrm = np.cross(b - a, d - a)
        ln = np.linalg.norm(nrm)
        if ln < 1e-9:
            return
        self.add([a, b, c, d], nrm / ln, (*rgb, style), uv4, [0, 1, 2, 0, 2, 3])

    def arrays(self):
        if not self.pos:
            return None
        uv = np.vstack(self.uv)
        uv = np.column_stack([uv[:, 0] / U_SCALE, uv[:, 1] / V_SCALE]).clip(0, 1)
        return np.vstack(self.pos), np.vstack(self.nrm), np.vstack(self.col), uv, np.concatenate(self.idx)


def walls(g: Geo, ring: np.ndarray, z0: float, z1: float, ground: float, style: int, rgb, v_off: float = 0.0):
    """Vertical walls along a CCW ring (outward normals)."""
    pts = ring[:-1] if np.allclose(ring[0], ring[-1]) else ring
    k = len(pts)
    for i in range(k):
        a, b = pts[i], pts[(i + 1) % k]
        L = float(np.hypot(*(b - a)))
        if L < 0.05:
            continue
        u0, u1 = 1024 - L / 2, 1024 + L / 2
        va, vb = z0 - ground + v_off, z1 - ground + v_off
        g.quad((a[0], a[1], z0), (b[0], b[1], z0), (b[0], b[1], z1), (a[0], a[1], z1), style, rgb,
               [(u0, va), (u1, va), (u1, vb), (u0, vb)])


def cap(g: Geo, poly: Polygon, z: float, style: int, rgb, up: bool = True):
    poly = orient(poly, 1.0)
    rings = [np.asarray(poly.exterior.coords)[:-1]] + [np.asarray(h.coords)[:-1] for h in poly.interiors]
    verts = np.vstack(rings)
    ends = np.cumsum([len(r) for r in rings]).astype(np.uint32)
    try:
        tri = earcut.triangulate_float64(verts, ends).reshape(-1, 3)
    except Exception:
        return
    if len(tri) == 0:
        return
    if not up:
        tri = tri[:, ::-1]
    mn = verts.min(axis=0)
    P = np.column_stack([verts, np.full(len(verts), z)])
    g.add(P, (0, 0, 1 if up else -1), (*rgb, style), verts - mn + 1.0, tri)


def box(g: Geo, c, f, hs, style, rgb, ground: float):
    """Oriented box: centre c (x, y, z), forward unit f (x, y), half sizes (right, forward, up)."""
    fx, fy = f
    r = np.array([fy, -fx, 0.0])
    fw = np.array([fx, fy, 0.0])
    up = np.array([0.0, 0.0, 1.0])
    c = np.asarray(c, float)
    hr, hf, hu = hs
    corners = {}
    for sr in (-1, 1):
        for sf in (-1, 1):
            for su in (-1, 1):
                corners[(sr, sf, su)] = c + r * hr * sr + fw * hf * sf + up * hu * su
    faces = [((1, -1, -1), (1, 1, -1), (1, 1, 1), (1, -1, 1), 2 * hf),     # right
             ((-1, 1, -1), (-1, -1, -1), (-1, -1, 1), (-1, 1, 1), 2 * hf),  # left
             ((-1, -1, -1), (1, -1, -1), (1, -1, 1), (-1, -1, 1), 2 * hr),  # back
             ((1, 1, -1), (-1, 1, -1), (-1, 1, 1), (1, 1, 1), 2 * hr),      # front
             ((-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1), 2 * hr)]      # top
    for a, b, cc, d, w in faces:
        z0 = corners[a][2] - ground
        z1 = corners[d][2] - ground
        g.quad(corners[a], corners[b], corners[cc], corners[d], style, rgb,
               [(1024 - w / 2, z0), (1024 + w / 2, z0), (1024 + w / 2, z1), (1024 - w / 2, z1)])


def cylinder(g: Geo, c, r, h, style, rgb, ground: float, seg: int = 10, top: bool = True):
    cx, cy, cz = c
    for i in range(seg):
        a0, a1 = 2 * math.pi * i / seg, 2 * math.pi * (i + 1) / seg
        p0 = (cx + r * math.cos(a0), cy + r * math.sin(a0))
        p1 = (cx + r * math.cos(a1), cy + r * math.sin(a1))
        L = r * (a1 - a0)
        g.quad((*p0, cz), (*p1, cz), (*p1, cz + h), (*p0, cz + h), style, rgb,
               [(1024, cz - ground), (1024 + L, cz - ground), (1024 + L, cz + h - ground), (1024, cz + h - ground)])
    if top:
        ring = [(cx + r * math.cos(2 * math.pi * i / seg), cy + r * math.sin(2 * math.pi * i / seg)) for i in range(seg)]
        cap(g, Polygon(ring), cz + h, ROOF_FLAT, rgb)


def pitched_roof(g: Geo, rect: np.ndarray, z_eave: float, pitch_deg: float, overhang: float, style: int,
                 rgb, wall_style: int, wall_rgb, ground: float, hip: bool):
    """Gable or hip roof over an oriented rectangle (4 CCW corners). Ridge along the long axis."""
    p = rect
    e0, e1 = p[1] - p[0], p[2] - p[1]
    if np.linalg.norm(e0) < np.linalg.norm(e1):
        p = np.roll(p, -1, axis=0)
        e0, e1 = p[1] - p[0], p[2] - p[1]
    L, W = np.linalg.norm(e0), np.linalg.norm(e1)
    ax, ay = e0 / L, e1 / W
    c = p.mean(axis=0)
    t = math.tan(math.radians(pitch_deg))
    rise = (W / 2) * t
    zr = z_eave + rise
    Lo, Wo = L / 2 + overhang, W / 2 + overhang
    zo = z_eave - overhang * t  # eave line lowered by the overhang
    q = lambda a, b, z: (c[0] + ax[0] * a + ay[0] * b, c[1] + ax[1] * a + ay[1] * b, z)
    ridge_half = (L / 2 - W / 2) if hip else Lo
    ridge_half = max(ridge_half, 0.3)
    sl = math.hypot(Wo, rise + overhang * t)
    # two long slopes
    g.quad(q(-Lo, -Wo, zo), q(Lo, -Wo, zo), q(ridge_half, 0, zr), q(-ridge_half, 0, zr), style, rgb,
           [(0, 0), (2 * Lo, 0), (Lo + ridge_half, sl), (Lo - ridge_half, sl)])
    g.quad(q(Lo, Wo, zo), q(-Lo, Wo, zo), q(-ridge_half, 0, zr), q(ridge_half, 0, zr), style, rgb,
           [(0, 0), (2 * Lo, 0), (Lo + ridge_half, sl), (Lo - ridge_half, sl)])
    if hip:
        for s in (-1, 1):
            a, b = q(s * Lo, -s * Wo, zo), q(s * Lo, s * Wo, zo)
            g.add([a, b, q(s * ridge_half, 0, zr)], _tri_n(a, b, q(s * ridge_half, 0, zr)), (*rgb, style),
                  [(0, 0), (2 * Wo, 0), (Wo, sl)], [0, 1, 2])
    else:
        for s in (-1, 1):  # gable end walls (triangle) + verge
            a, b = q(s * L / 2, -s * W / 2, z_eave), q(s * L / 2, s * W / 2, z_eave)
            tip = q(s * L / 2, 0, zr)
            g.add([a, b, tip], _tri_n(a, b, tip), (*wall_rgb, wall_style),
                  [(1024 - W / 2, z_eave - ground), (1024 + W / 2, z_eave - ground), (1024, zr - ground)], [0, 1, 2])
    # soffit under the overhang so the eaves have thickness when seen from below
    for s in (-1, 1):
        g.quad(q(Lo, s * Wo, zo - 0.12), q(-Lo, s * Wo, zo - 0.12), q(-Lo, s * Wo, zo), q(Lo, s * Wo, zo), PLAIN,
               tuple(int(v * 0.8) for v in wall_rgb), [(0, 0), (1, 0), (1, 1), (0, 1)])


def _tri_n(a, b, c):
    n = np.cross(np.asarray(b) - a, np.asarray(c) - a)
    ln = np.linalg.norm(n)
    return n / ln if ln > 0 else np.array([0, 0, 1.0])


# --------------------------------------------------------------------------------------------
PALETTE_WALL = {
    "office": [(150, 160, 170), (120, 130, 140), (185, 185, 180), (95, 110, 125), (170, 175, 185)],
    "commercial": [(205, 196, 180), (150, 138, 124), (225, 222, 214), (120, 110, 100), (190, 176, 158), (150, 160, 168),
                   (176, 120, 96), (98, 102, 110)],
    "pencil": [(210, 200, 185), (120, 110, 100), (200, 190, 175), (230, 228, 220), (150, 150, 155), (180, 120, 90)],
    "apt": [(225, 220, 208), (210, 200, 185), (190, 185, 178), (200, 175, 150), (235, 232, 225)],
    "house": [(232, 226, 210), (215, 200, 175), (190, 180, 165), (240, 238, 232), (170, 160, 150), (205, 190, 160),
              (150, 145, 140)],
    "wooden": [(92, 70, 52), (110, 86, 62), (75, 60, 48), (130, 104, 78)],
    "glass": [(60, 90, 110), (70, 100, 90), (80, 90, 110), (95, 115, 130), (50, 70, 90)],
    "warehouse": [(180, 185, 190), (160, 170, 175), (190, 175, 150), (140, 150, 160)],
}
ROOF_TILE_COL = [(70, 72, 78), (60, 62, 66), (85, 80, 76), (95, 70, 55), (70, 80, 95)]
ROOF_METAL_COL = [(90, 105, 120), (130, 60, 50), (60, 80, 70), (110, 110, 110), (150, 120, 90), (70, 70, 80)]
NEON_COL = [(255, 60, 60), (255, 200, 40), (60, 200, 255), (255, 90, 200), (120, 255, 120), (255, 255, 255),
            (255, 140, 30), (180, 90, 255)]


@dataclass
class BuildingOut:
    geo: Geo
    gear: Geo                   # vertex-coloured rooftop equipment (page -1)
    footprint: np.ndarray       # CCW ring (x, y)
    ground: float
    height: float
    storeys: int
    usage: int
    name: str = ""
    kind: str = ""
    walk_in: bool = False       # no footprint collision (the player walks in: stations, shops)
    shop: dict | None = None    # walk-in ground floor (shop.py): built with the cell's street detail


def _shop_floor(fp, front, ground, rng, p=0.45):
    """A walk-in shop on the ground floor (shop.py), for about half the small shop buildings."""
    if rng.random() > p:
        return None
    from .shop import plan
    return plan(fp, front, ground, rng)


def _with_shop(out: "BuildingOut", shop):
    out.shop = shop
    out.walk_in = shop is not None
    return out


def _jit(rgb, rng, amt=10):
    return tuple(int(np.clip(v + rng.integers(-amt, amt + 1), 0, 255)) for v in rgb)


def _rect_of(poly: Polygon):
    r = orient(poly.minimum_rotated_rectangle, 1.0)
    return np.asarray(r.exterior.coords)[:4]


def _rooftop_gear(gear: Geo, top: Polygon, z: float, rng, ground: float, big: bool):
    inner = top.buffer(-1.6)
    if inner.is_empty or inner.area < 6:
        return
    minx, miny, maxx, maxy = inner.bounds
    rect = _rect_of(top)
    e = rect[1] - rect[0]
    f = (e / (np.linalg.norm(e) + 1e-9))
    fdir = (float(f[0]), float(f[1]))
    from shapely.geometry import Point
    n = int(min(10, 1 + inner.area / (60 if big else 90)))
    for _ in range(n):
        for _try in range(6):
            x, y = rng.uniform(minx, maxx), rng.uniform(miny, maxy)
            if inner.contains(Point(x, y)):
                break
        else:
            continue
        kind = rng.random()
        if kind < 0.45:  # AC outdoor units
            box(gear, (x, y, z + 0.45), fdir, (0.45, 0.35, 0.45), PLAIN, _jit((200, 200, 196), rng, 6), ground)
        elif kind < 0.65:  # water tank
            cylinder(gear, (x, y, z), 1.1, 2.0, PLAIN, _jit((190, 192, 196), rng, 6), ground, seg=10)
        elif kind < 0.85:  # elevator / stair housing
            box(gear, (x, y, z + 1.5), fdir, (1.6, 2.0, 1.5), PLAIN, _jit((175, 172, 165), rng, 8), ground)
        else:  # vent / antenna mast
            box(gear, (x, y, z + 2.5), fdir, (0.08, 0.08, 2.5), PLAIN, (150, 150, 150), ground)


ROOF_FLAT_COL = [(150, 150, 146), (172, 172, 168), (124, 124, 122), (112, 140, 112), (118, 132, 150), (158, 150, 136),
                 (96, 100, 104)]


def _flat_top(g: Geo, gear: Geo, poly: Polygon, z: float, ground: float, wall_style, rgb, rng, big=False, heli=False):
    """Flat roof with a parapet and rooftop gear (small buildings: plain roof slab, no parapet)."""
    roof_col = _jit(ROOF_FLAT_COL[rng.integers(len(ROOF_FLAT_COL))], rng, 6)
    if not big and poly.area < 160:
        cap(g, poly, z, ROOF_FLAT, roof_col)
        if rng.random() < 0.3:
            _rooftop_gear(gear, poly, z, rng, ground, False)
        return
    ring = np.asarray(orient(poly, 1.0).exterior.coords)
    walls(g, ring, z, z + 0.9, ground, PLAIN, tuple(int(v * 0.92) for v in rgb))
    inner = poly.buffer(-0.25)
    if not inner.is_empty and isinstance(inner, Polygon):
        iring = np.asarray(orient(inner, 1.0).exterior.coords)[::-1]  # inward-facing parapet side
        walls(g, iring, z, z + 0.9, ground, PLAIN, tuple(int(v * 0.85) for v in rgb))
        rim = poly.difference(inner)
        for part in getattr(rim, "geoms", [rim]):
            if isinstance(part, Polygon) and part.area > 0.05:
                cap(g, part, z + 0.9, ROOF_FLAT, (180, 180, 176))
        cap(g, inner, z + 0.05, ROOF_HELI if heli else ROOF_FLAT, roof_col)
        _rooftop_gear(gear, inner, z + 0.05, rng, ground, big)
    else:
        cap(g, poly, z, ROOF_FLAT, (150, 150, 146))


def _neon_sign(g: Geo, poly: Polygon, front, z0, z1, ground, rng):
    """Vertical projecting sign (袖看板) at a front corner, perpendicular to the facade."""
    if front == (0.0, 0.0):
        return
    fx, fy = front
    ring = np.asarray(orient(poly, 1.0).exterior.coords)[:-1]
    # front-most vertex: max dot with the front direction; offset slightly along the facade
    dots = ring @ np.array([fx, fy])
    k = int(np.argmax(dots))
    p = ring[k]
    side = np.array([fy, -fx])
    if rng.random() < 0.5:
        side = -side
    c = p + np.array([fx, fy]) * 0.55 - side * 0.6
    h = min(z1 - z0, rng.uniform(4.0, 11.0))
    zc = z0 + h / 2
    col = NEON_COL[rng.integers(len(NEON_COL))]
    # thin panel (0.9 wide along the front direction, 0.25 thick), faces point along +-side
    box(g, (c[0], c[1], zc), (float(side[0]), float(side[1])), (0.45, 0.13, h / 2), NEON, col, zc - h / 2)


def _balconies(g: Geo, poly: Polygon, front, ground, z0, z1, storey, rgb, rng):
    """Balcony slabs + railing bands along the front facade of an apartment block."""
    if front == (0.0, 0.0):
        return
    ring = np.asarray(orient(poly, 1.0).exterior.coords)
    fx, fy = front
    for i in range(len(ring) - 1):
        a, b = ring[i], ring[i + 1]
        d = b - a
        L = float(np.hypot(*d))
        if L < 5:
            continue
        n = np.array([d[1], -d[0]]) / L
        if n @ np.array([fx, fy]) < 0.7:
            continue
        a2, b2 = a + n * 1.1, b + n * 1.1
        z = z0 + storey
        while z < z1 - 0.5:
            # slab
            g.quad((a[0], a[1], z), (b[0], b[1], z), (b2[0], b2[1], z), (a2[0], a2[1], z), PLAIN, (205, 205, 200),
                   [(0, 0), (1, 0), (1, 1), (0, 1)])
            g.quad((a2[0], a2[1], z - 0.18), (b2[0], b2[1], z - 0.18), (b2[0], b2[1], z), (a2[0], a2[1], z), PLAIN,
                   (215, 215, 210), [(0, 0), (1, 0), (1, 1), (0, 1)])
            # railing band (frosted panel)
            g.quad((a2[0], a2[1], z), (b2[0], b2[1], z), (b2[0], b2[1], z + 1.1), (a2[0], a2[1], z + 1.1), APT_FRONT + 2,
                   rgb, [(1024 - L / 2, z - ground), (1024 + L / 2, z - ground), (1024 + L / 2, z + 1.1 - ground),
                         (1024 - L / 2, z + 1.1 - ground)])
            z += storey


def make_building(pc, terrain, rng, near_scramble: float, near_station: float) -> BuildingOut | None:
    """One building on a parcel, by district style. Returns None for an empty lot."""
    style = pc.style
    poly = pc.poly
    setback = {"center": 0.25, "business": 3.0, "shitamachi": 0.35, "residential": 1.2, "bay": 6.0, "islet": 8.0,
               "kyoto": 0.15, "kyoto_center": 0.3, "port": 5.0, "onsen": 0.6, "snowtown": 1.8, "village": 4.0}[style]
    if style == "residential" and pc.poly.area > 180:
        setback = 1.6
    fp = poly.buffer(-setback, join_style=2).simplify(0.25)
    if fp.is_empty or not isinstance(fp, Polygon) or fp.area < 20:
        return None
    fp = orient(fp, 1.0)
    ring = np.asarray(fp.exterior.coords)
    tz = terrain.sample(ring[:, 0], ring[:, 1])
    ground = float(tz.min())
    g, gear = Geo(), Geo()
    area = fp.area
    front = pc.front_dir
    r = rng.random()

    def result(h, storeys, usage, kind):
        return BuildingOut(g, gear, ring[:-1], ground, h, storeys, usage, "", kind)

    if style == "center":
        if r < 0.07:
            return None  # coin parking
        rect = _rect_of(fp)
        w = min(np.linalg.norm(rect[1] - rect[0]), np.linalg.norm(rect[2] - rect[1]))
        boost = 1.0 + 0.9 * math.exp(-near_scramble / 260.0)
        if w < 9.5 or area < 130:  # pencil building
            fl = int(np.clip(rng.normal(7, 2) * boost, 4, 16))
            h = 4.5 + (fl - 1) * 3.4
            rgb = _jit(PALETTE_WALL["pencil"][rng.integers(6)], rng)
            walls(g, ring, ground - 2, ground + h, ground, PENCIL, rgb)
            _flat_top(g, gear, fp, ground + h, ground, PENCIL, rgb, rng)
            if rng.random() < 0.75:
                _neon_sign(g, fp, front, ground + 3.5, ground + h, ground, rng)
            if rng.random() < 0.12:
                _billboard(g, fp, front, ground + h + 0.9, ground, rng)
            return result(h, fl, 402, "pencil")
        fl = int(np.clip(rng.normal(8, 2.5) * boost, 3, 24))
        h = 4.8 + (fl - 1) * 3.7
        rgb = _jit(PALETTE_WALL["commercial"][rng.integers(8)], rng)
        st = COMMERCIAL if rng.random() < 0.7 else (OFFICE_GRID if rng.random() < 0.6 else CURTAIN)
        if fl >= 12 and area > 500 and rng.random() < 0.6:  # podium + setback
            pod = 4.8 + 3 * 3.7
            walls(g, ring, ground - 2, ground + pod, ground, COMMERCIAL, rgb)
            top = fp.buffer(-rng.uniform(3, 6), join_style=2)
            if isinstance(top, Polygon) and top.area > 80:
                cap(g, fp.difference(top) if fp.difference(top).geom_type == "Polygon" else fp, ground + pod, ROOF_FLAT,
                    (150, 150, 146))
                tring = np.asarray(orient(top, 1.0).exterior.coords)
                grgb = _jit(PALETTE_WALL["glass"][rng.integers(5)], rng)
                walls(g, tring, ground + pod, ground + h, ground, CURTAIN, grgb)
                _flat_top(g, gear, top, ground + h, ground, CURTAIN, grgb, rng, big=True)
            else:
                walls(g, ring, ground + pod, ground + h, ground, st, rgb)
                _flat_top(g, gear, fp, ground + h, ground, st, rgb, rng, big=True)
        else:
            walls(g, ring, ground - 2, ground + h, ground, st, rgb)
            _flat_top(g, gear, fp, ground + h, ground, st, rgb, rng, big=True)
        if rng.random() < 0.45:
            _neon_sign(g, fp, front, ground + 4.0, ground + min(h, 30), ground, rng)
        if near_scramble < 170 and rng.random() < 0.5:
            _led_screen(g, fp, front, ground, h, rng)
        elif rng.random() < 0.18:
            _billboard(g, fp, front, ground + h + 0.9, ground, rng)
        return result(h, fl, 402 if rng.random() < 0.7 else 404, "commercial")

    if style in ("business", "bay", "islet"):
        big = area > 2200
        if style == "bay" and near_station > 700 and rng.random() < 0.5:  # port warehouses
            h = rng.uniform(9, 16)
            rgb = _jit(PALETTE_WALL["warehouse"][rng.integers(4)], rng)
            walls(g, ring, ground - 2, ground + h, ground, WAREHOUSE, rgb)
            cap(g, fp, ground + h, ROOF_METAL, _jit((150, 155, 160), rng, 8))
            return result(h, 2, 431, "warehouse")
        residential = style == "bay" and rng.random() < 0.65
        if big:
            fl = int(rng.uniform(28, 58) if style == "business" else rng.uniform(26, 50))
            h = 5.0 + (fl - 1) * (4.0 if not residential else 3.15)
            pod_fl = int(rng.uniform(3, 6))
            pod = 5.0 + (pod_fl - 1) * 4.2
            prgb = _jit(PALETTE_WALL["commercial"][rng.integers(6)], rng)
            walls(g, ring, ground - 2, ground + pod, ground, COMMERCIAL, prgb)
            # tower footprint: rectangle inside the parcel, rotated with it
            rect = _rect_of(fp)
            c = rect.mean(axis=0)
            e0, e1 = rect[1] - rect[0], rect[2] - rect[1]
            s = rng.uniform(0.45, 0.68)
            tw = [c + (-e0 / 2 - e1 / 2) * s, c + (e0 / 2 - e1 / 2) * s, c + (e0 / 2 + e1 / 2) * s, c + (-e0 / 2 + e1 / 2) * s]
            tower = Polygon(tw)
            if not fp.buffer(0.1).contains(tower):
                tower = fp.buffer(-6, join_style=2)
            if not isinstance(tower, Polygon) or tower.area < 150:
                tower = fp
            podroof = fp.difference(tower)
            for part in getattr(podroof, "geoms", [podroof]):
                if isinstance(part, Polygon) and part.area > 1:
                    cap(g, part, ground + pod, ROOF_FLAT, (140, 145, 140))
            tring = np.asarray(orient(tower, 1.0).exterior.coords)
            if residential:
                trgb = _jit(PALETTE_WALL["apt"][rng.integers(5)], rng)
                walls(g, tring, ground + pod, ground + h, ground, APT_FRONT if rng.random() < 0.5 else CURTAIN, trgb)
                usage = 412
            else:
                trgb = _jit(PALETTE_WALL["glass"][rng.integers(5)], rng)
                walls(g, tring, ground + pod, ground + h, ground, CURTAIN if rng.random() < 0.6 else CURTAIN_FIN, trgb)
                usage = 401 if style == "business" else 403
            # crown: a slightly inset top storey band
            top = tower.buffer(-1.2, join_style=2)
            if isinstance(top, Polygon) and top.area > 60:
                cap(g, tower.difference(top) if tower.difference(top).geom_type == "Polygon" else tower, ground + h,
                    ROOF_FLAT, (170, 170, 166))
                cring = np.asarray(orient(top, 1.0).exterior.coords)
                walls(g, cring, ground + h, ground + h + 6, ground, PANEL, _jit((190, 192, 195), rng, 6))
                _flat_top(g, gear, top, ground + h + 6, ground, PANEL, (190, 192, 195), rng, big=True, heli=fl > 40)
            else:
                _flat_top(g, gear, tower, ground + h, ground, CURTAIN, trgb, rng, big=True, heli=fl > 40)
            return result(h + 6, fl, usage, "tower")
        fl = int(rng.uniform(7, 20))
        h = 4.6 + (fl - 1) * 3.9
        rgb = _jit(PALETTE_WALL["office"][rng.integers(5)], rng)
        st = [OFFICE_RIBBON, OFFICE_GRID, CURTAIN][rng.integers(3)]
        walls(g, ring, ground - 2, ground + h, ground, st, rgb)
        _flat_top(g, gear, fp, ground + h, ground, st, rgb, rng, big=True)
        return result(h, fl, 401 if style == "business" else 404, "office")

    if style in ("kyoto", "kyoto_center", "port", "onsen", "snowtown", "village"):
        return _regional(style, pc, fp, ring, ground, g, gear, rng, near_station, result)

    if style in ("shitamachi", "residential"):
        if r < (0.04 if style == "shitamachi" else 0.06):
            return None  # parking / garden / field
        p_house = 0.62 if style == "shitamachi" else 0.66
        p_shop = 0.2 if style == "shitamachi" else 0.04
        p_apt = 0.13 if style == "shitamachi" else 0.2
        near_boost = math.exp(-near_station / 350.0)
        rr = rng.random()
        rect = _rect_of(fp)
        rect_area = Polygon(rect).area
        rectish = rect_area > 0 and area / rect_area > 0.84
        if rr < p_house - 0.25 * near_boost or (area < 70):
            # detached / row house, 2 storeys, pitched roof when the footprint is a clean rectangle
            fl = 2 if rng.random() < 0.85 else 3
            eave = ground + 0.4 + fl * 2.85
            wooden = style == "shitamachi" and rng.random() < 0.55
            wrgb = _jit(PALETTE_WALL["wooden" if wooden else "house"][rng.integers(4 if wooden else 7)], rng, 8)
            wst = WOODEN if wooden else HOUSE
            if rectish and area < 260:
                rp = Polygon(rect)
                rring = np.asarray(orient(rp, 1.0).exterior.coords)
                walls(g, rring, ground - 1.5, eave, ground, wst, wrgb)
                tile = wooden or rng.random() < 0.5
                rgb_r = ROOF_TILE_COL[rng.integers(5)] if tile else ROOF_METAL_COL[rng.integers(6)]
                pitched_roof(g, rect, eave, rng.uniform(22, 32), 0.55, ROOF_TILE if tile else ROOF_METAL, _jit(rgb_r, rng, 6),
                             wst, wrgb, ground, hip=rng.random() < 0.4)
                fp_out = rring[:-1]
                return BuildingOut(g, gear, fp_out, ground, eave - ground + 2.5, fl, 411, "", "house")
            walls(g, ring, ground - 1.5, eave, ground, wst, wrgb)
            _flat_top(g, gear, fp, eave, ground, wst, wrgb, rng)
            return result(eave - ground, fl, 411, "house")
        if rr < p_house + p_shop:
            fl = int(rng.integers(2, 5))
            h = 4.2 + (fl - 1) * 3.0
            wrgb = _jit(PALETTE_WALL["pencil" if style == "shitamachi" else "house"][rng.integers(6)], rng)
            shop = _shop_floor(fp, front, ground, rng)
            walls(g, ring, ground + FLOOR_H if shop else ground - 1.5, ground + h, ground, COMMERCIAL if fl > 2 else HOUSE, wrgb)
            if rectish and fl <= 2 and rng.random() < 0.5:
                pitched_roof(g, rect, ground + h, 25, 0.5, ROOF_TILE, _jit(ROOF_TILE_COL[rng.integers(5)], rng, 6), HOUSE,
                             wrgb, ground, hip=True)
            else:
                _flat_top(g, gear, fp, ground + h, ground, HOUSE, wrgb, rng)
            if rng.random() < 0.3:
                _neon_sign(g, fp, front, ground + 3.6, ground + h, ground, rng)
            return _with_shop(result(h, fl, 413, "shop"), shop)
        if rr < p_house + p_shop + p_apt + 0.2 * near_boost:
            fl = int(rng.integers(2, 5)) if rng.random() < 0.6 else int(rng.integers(5, 11))
            h = 3.4 + (fl - 1) * 3.0
            wrgb = _jit(PALETTE_WALL["apt"][rng.integers(5)], rng)
            walls(g, ring, ground - 1.5, ground + h, ground, APT_BACK, wrgb)
            _flat_top(g, gear, fp, ground + h, ground, APT_BACK, wrgb, rng)
            if fl >= 3:
                _balconies(g, fp, front, ground, ground, ground + h, 3.0, wrgb, rng)
            return result(h, fl, 412, "apartment")
        fl = int(rng.integers(5, 12))
        h = 4.0 + (fl - 1) * 3.05
        wrgb = _jit(PALETTE_WALL["apt"][rng.integers(5)], rng)
        walls(g, ring, ground - 1.5, ground + h, ground, APT_FRONT, wrgb)
        _flat_top(g, gear, fp, ground + h, ground, APT_FRONT, wrgb, rng, big=True)
        _balconies(g, fp, front, ground, ground, ground + h, 3.05, wrgb, rng)
        return result(h, fl, 412, "mansion")
    return None


def _billboard(g: Geo, poly: Polygon, front, z, ground, rng):
    if front == (0.0, 0.0):
        return
    c = np.asarray(poly.centroid.coords[0])
    fx, fy = front
    rgb = NEON_COL[rng.integers(len(NEON_COL))]
    w, h = rng.uniform(6, 12), rng.uniform(3, 6)
    base = c + np.array([fx, fy]) * 1.0
    # frame legs
    for s in (-1, 1):
        p = base + np.array([fy, -fx]) * (w / 2 - 0.3) * s
        box(g, (p[0], p[1], z + 1.0), (fx, fy), (0.1, 0.1, 1.0), METAL, (90, 90, 90), ground)
    box(g, (base[0], base[1], z + 2.0 + h / 2), (fx, fy), (w / 2, 0.15, h / 2), BILLBOARD, rgb, ground)


def _led_screen(g: Geo, poly: Polygon, front, ground, h, rng):
    """Large LED screen on the front facade (content is abstract, never real advertising)."""
    if front == (0.0, 0.0) or h < 18:
        return
    ring = np.asarray(orient(poly, 1.0).exterior.coords)
    fx, fy = front
    best, seg = 0, None
    for i in range(len(ring) - 1):
        a, b = ring[i], ring[i + 1]
        d = b - a
        L = float(np.hypot(*d))
        n = np.array([d[1], -d[0]]) / max(L, 1e-9)
        if n @ np.array([fx, fy]) > 0.7 and L > best:
            best, seg = L, (a, b, n)
    if seg is None or best < 10:
        return
    a, b, n = seg
    w = min(best * 0.8, rng.uniform(12, 22))
    sh = min(h * 0.45, w * 0.6)
    c = (a + b) / 2 + n * 0.35
    z0 = ground + rng.uniform(8, max(9, h - sh - 3))
    dd = (b - a) / best
    p0, p1 = c - dd * w / 2, c + dd * w / 2
    g.quad((p0[0], p0[1], z0), (p1[0], p1[1], z0), (p1[0], p1[1], z0 + sh), (p0[0], p0[1], z0 + sh), LED,
           NEON_COL[rng.integers(len(NEON_COL))], [(1024 - w / 2, 0), (1024 + w / 2, 0), (1024 + w / 2, sh), (1024 - w / 2, sh)])


# --------------------------------------------------------------------------------------------
# Regional towns (all generic designs): the old capital's machiya streets, the port's sheds, the
# hot-spring inns, the snow country's steep roofs and the farm villages.
def _gable_house(g, rect, ground, fl, storey, wall_style, wall_rgb, roof_style, roof_rgb, pitch, overhang, hip, sink=1.5):
    rp = Polygon(rect)
    rring = np.asarray(orient(rp, 1.0).exterior.coords)
    eave = ground + 0.4 + fl * storey
    walls(g, rring, ground - sink, eave, ground, wall_style, wall_rgb)
    pitched_roof(g, rect, eave, pitch, overhang, roof_style, roof_rgb, wall_style, wall_rgb, ground, hip=hip)
    return rring[:-1], eave


def _street_parallel(rect, front):
    """Rotate the rectangle's corner order so its first edge runs along the street (ridge parallel
    to the street, as on machiya), when the frontage direction is known."""
    if front == (0.0, 0.0):
        return rect
    f = np.array(front)
    best, k = -1.0, 0
    for i in range(4):
        e = rect[(i + 1) % 4] - rect[i]
        e = e / (np.linalg.norm(e) + 1e-9)
        par = 1.0 - abs(float(e @ f))
        if par > best:
            best, k = par, i
    return np.roll(rect, -k, axis=0)


def _regional(style, pc, fp, ring, ground, g, gear, rng, near_station, result):
    area = fp.area
    front = pc.front_dir
    rect = _rect_of(fp)
    rect_area = Polygon(rect).area
    rectish = rect_area > 0 and area / rect_area > 0.8
    r = rng.random()
    if style == "kyoto":
        if r < 0.05:
            return None
        if r < 0.66 and rectish:
            # machiya: two low storeys, dark timber, latticed front, tiled gable roof along the street
            wrgb = _jit(PALETTE_WALL["wooden"][rng.integers(4)], rng, 6)
            rr = _street_parallel(rect, front)
            e0 = np.linalg.norm(rr[1] - rr[0])
            e1 = np.linalg.norm(rr[2] - rr[1])
            if e0 < e1:  # ridge along the longer side facing the street is typical; keep the long axis
                pass
            fpo, eave = _gable_house(g, rr, ground, 2, 2.55, WOODEN, wrgb, ROOF_TILE, _jit((64, 66, 70), rng, 5),
                                     rng.uniform(21, 26), 0.6, hip=False)
            return BuildingOut(g, gear, fpo, ground, eave - ground + 2.2, 2, 402 if rng.random() < 0.35 else 411, "", "machiya")
        if r < 0.82:
            fl = int(rng.integers(3, 6))
            h = 4.2 + (fl - 1) * 3.1
            wrgb = _jit(PALETTE_WALL["commercial"][rng.integers(8)], rng)
            shop = _shop_floor(fp, front, ground, rng)
            walls(g, ring, ground + FLOOR_H if shop else ground - 1.5, ground + h, ground, COMMERCIAL, wrgb)
            _flat_top(g, gear, fp, ground + h, ground, COMMERCIAL, wrgb, rng)
            return _with_shop(result(h, fl, 402, "shop"), shop)
        if r < 0.95 or not rectish:
            fl = 2
            wrgb = _jit(PALETTE_WALL["house"][rng.integers(7)], rng, 8)
            if rectish:
                fpo, eave = _gable_house(g, rect, ground, fl, 2.85, HOUSE, wrgb, ROOF_TILE,
                                         _jit(ROOF_TILE_COL[rng.integers(5)], rng, 6), rng.uniform(22, 28), 0.5, hip=rng.random() < 0.5)
                return BuildingOut(g, gear, fpo, ground, eave - ground + 2.4, fl, 411, "", "house")
            eave = ground + 0.4 + fl * 2.85
            walls(g, ring, ground - 1.5, eave, ground, HOUSE, wrgb)
            _flat_top(g, gear, fp, eave, ground, HOUSE, wrgb, rng)
            return result(eave - ground, fl, 411, "house")
        # small temple / sub-temple: white walls between vermilion posts, deep hip roof
        fpo, eave = _gable_house(g, rect, ground, 1, 4.2, TEMPLE, (236, 230, 216), ROOF_TILE, (56, 58, 62),
                                 30, 1.6, hip=True, sink=1.0)
        return BuildingOut(g, gear, fpo, ground, eave - ground + 5.0, 1, 422, "", "subtemple")
    if style == "kyoto_center":
        if r < 0.05:
            return None
        fl = int(np.clip(rng.normal(6, 1.6), 3, 9))  # height limits in the old capital
        h = 4.6 + (fl - 1) * 3.6
        wrgb = _jit(PALETTE_WALL["commercial"][rng.integers(8)], rng)
        st = COMMERCIAL if rng.random() < 0.7 else OFFICE_GRID
        walls(g, ring, ground - 2, ground + h, ground, st, wrgb)
        _flat_top(g, gear, fp, ground + h, ground, st, wrgb, rng, big=True)
        if rng.random() < 0.35:
            _neon_sign(g, fp, front, ground + 4.0, ground + min(h, 22), ground, rng)
        return result(h, fl, 402 if rng.random() < 0.7 else 401, "commercial")
    if style == "port":
        if r < 0.1:
            return None
        if r < 0.72:  # warehouse / shed
            h = rng.uniform(9, 17)
            rgb = _jit(PALETTE_WALL["warehouse"][rng.integers(4)], rng)
            walls(g, ring, ground - 2, ground + h, ground, WAREHOUSE, rgb)
            if rectish and rng.random() < 0.5:
                pitched_roof(g, rect, ground + h, 8, 0.4, ROOF_METAL, _jit((150, 155, 160), rng, 8), WAREHOUSE, rgb, ground, hip=False)
            else:
                cap(g, fp, ground + h, ROOF_METAL, _jit((150, 155, 160), rng, 8))
            return result(h, 2, 441, "warehouse")
        if r < 0.86:  # container stacks on the yard (vertex-coloured boxes)
            c = np.asarray(fp.centroid.coords[0])
            e = rect[1] - rect[0]
            f = e / (np.linalg.norm(e) + 1e-9)
            side = np.array([f[1], -f[0]])
            cols = [(40, 90, 150), (170, 40, 40), (200, 120, 40), (60, 120, 70), (120, 120, 125), (210, 200, 60)]
            for i in range(-3, 4):
                for j in range(-1, 2):
                    for lev in range(int(rng.integers(1, 5))):
                        p = c + f * i * 12.6 + side * j * 2.6
                        box(gear, (p[0], p[1], ground + 1.3 + lev * 2.6), (float(f[0]), float(f[1])), (6.0, 1.2, 1.3), PLAIN,
                            cols[rng.integers(len(cols))], ground)
            fpo = np.array([c + f * 45 + side * 4.5, c - f * 45 + side * 4.5, c - f * 45 - side * 4.5, c + f * 45 - side * 4.5])
            return BuildingOut(g, gear, fpo, ground, 10.4, 1, 441, "", "containers")
        fl = int(rng.integers(3, 7))
        h = 4.2 + (fl - 1) * 3.6
        rgb = _jit(PALETTE_WALL["office"][rng.integers(5)], rng)
        walls(g, ring, ground - 2, ground + h, ground, OFFICE_RIBBON, rgb)
        _flat_top(g, gear, fp, ground + h, ground, OFFICE_RIBBON, rgb, rng, big=True)
        return result(h, fl, 401, "office")
    if style == "onsen":
        if r < 0.05:
            return None
        if r < 0.22 and area > 380:
            # hot-spring hotel: concrete block with balconies (rooms face the valley)
            fl = int(rng.integers(4, 10))
            h = 4.4 + (fl - 1) * 3.1
            wrgb = _jit(PALETTE_WALL["apt"][rng.integers(5)], rng)
            walls(g, ring, ground - 1.5, ground + h, ground, APT_FRONT, wrgb)
            _flat_top(g, gear, fp, ground + h, ground, APT_FRONT, wrgb, rng, big=True)
            _balconies(g, fp, front, ground, ground + 4.4, ground + h, 3.1, wrgb, rng)
            return result(h, fl, 403, "hotel")
        if r < 0.72 and rectish:
            # traditional inn: timber, two or three storeys, tiled hip roof
            fl = 2 if rng.random() < 0.6 else 3
            wrgb = _jit(PALETTE_WALL["wooden"][rng.integers(4)], rng, 6)
            fpo, eave = _gable_house(g, rect, ground, fl, 2.9, WOODEN, wrgb, ROOF_TILE, _jit((60, 62, 66), rng, 5),
                                     rng.uniform(24, 30), 0.9, hip=True)
            return BuildingOut(g, gear, fpo, ground, eave - ground + 3.0, fl, 403, "", "ryokan")
        fl = 2
        h = 4.0 + 3.0
        wrgb = _jit(PALETTE_WALL["pencil"][rng.integers(6)], rng)
        shop = _shop_floor(fp, front, ground, rng)
        walls(g, ring, ground + FLOOR_H if shop else ground - 1.5, ground + h, ground, COMMERCIAL, wrgb)
        _flat_top(g, gear, fp, ground + h, ground, COMMERCIAL, wrgb, rng)
        if rng.random() < 0.5:
            _neon_sign(g, fp, front, ground + 3.6, ground + h, ground, rng)
        return _with_shop(result(h, fl, 402, "shop"), shop)
    if style == "snowtown":
        if r < 0.06:
            return None
        if r < 0.8 and rectish and area < 320:
            # snow-country house: steep metal gable roof (snow slides off), raised ground floor
            fl = 2
            wrgb = _jit(PALETTE_WALL["house"][rng.integers(7)], rng, 8)
            rgb_r = _jit(ROOF_METAL_COL[rng.integers(6)], rng, 6)
            fpo, eave = _gable_house(g, rect, ground + 0.6, fl, 2.9, HOUSE, wrgb, ROOF_METAL, rgb_r, rng.uniform(30, 38), 0.7,
                                     hip=False, sink=2.1)
            return BuildingOut(g, gear, fpo, ground, eave - ground + 3.5, fl, 411, "", "house")
        if r < 0.9:
            fl = int(rng.integers(2, 4))
            h = 4.2 + (fl - 1) * 3.0
            wrgb = _jit(PALETTE_WALL["house"][rng.integers(7)], rng)
            shop = _shop_floor(fp, front, ground, rng)
            walls(g, ring, ground + FLOOR_H if shop else ground - 1.5, ground + h, ground, COMMERCIAL, wrgb)
            _flat_top(g, gear, fp, ground + h, ground, COMMERCIAL, wrgb, rng)
            return _with_shop(result(h, fl, 402, "shop"), shop)
        fl = int(rng.integers(3, 6))
        h = 3.6 + (fl - 1) * 3.0
        wrgb = _jit(PALETTE_WALL["apt"][rng.integers(5)], rng)
        walls(g, ring, ground - 1.5, ground + h, ground, APT_BACK, wrgb)
        _flat_top(g, gear, fp, ground + h, ground, APT_BACK, wrgb, rng)
        return result(h, fl, 412, "apartment")
    if style == "village":
        if r < 0.22:
            return None  # garden / field / empty lot
        # farmhouse: a big two-storey house with a heavy tiled hip roof, often with a shed beside it
        c = np.asarray(fp.centroid.coords[0])
        e = rect[1] - rect[0]
        e2 = rect[2] - rect[1]
        f = e / (np.linalg.norm(e) + 1e-9)
        sd = e2 / (np.linalg.norm(e2) + 1e-9)
        hl, hw = min(np.linalg.norm(e) / 2 - 1.0, rng.uniform(6.5, 9.5)), min(np.linalg.norm(e2) / 2 - 1.0, rng.uniform(5.0, 7.0))
        if hl < 3.5 or hw < 3.0:
            return None
        hc = c - sd * (np.linalg.norm(e2) / 2 - hw - 1.0) * 0.5
        hrect = np.array([hc - f * hl - sd * hw, hc + f * hl - sd * hw, hc + f * hl + sd * hw, hc - f * hl + sd * hw])
        wrgb = _jit(PALETTE_WALL["house" if rng.random() < 0.6 else "wooden"][rng.integers(4)], rng, 8)
        tile = rng.random() < 0.7
        fpo, eave = _gable_house(g, hrect, ground, 2 if rng.random() < 0.7 else 1, 2.9,
                                 HOUSE if wrgb[0] > 140 else WOODEN, wrgb, ROOF_TILE if tile else ROOF_METAL,
                                 _jit(ROOF_TILE_COL[rng.integers(5)] if tile else ROOF_METAL_COL[rng.integers(6)], rng, 6),
                                 rng.uniform(24, 31), 0.8, hip=rng.random() < 0.7)
        # shed / barn (metal) at the side
        room = np.linalg.norm(e) / 2 - hl
        if room > 7 and rng.random() < 0.7:
            sc = c + f * (hl + room / 2) * (1 if rng.random() < 0.5 else -1)
            sl, sw = min(room / 2 - 0.8, 4.5), min(hw, 4.0)
            srect = np.array([sc - f * sl - sd * sw, sc + f * sl - sd * sw, sc + f * sl + sd * sw, sc - f * sl + sd * sw])
            _gable_house(g, srect, ground, 1, 3.2, WAREHOUSE, _jit((150, 150, 145), rng, 10), ROOF_METAL,
                         _jit(ROOF_METAL_COL[rng.integers(6)], rng, 6), 16, 0.3, hip=False, sink=0.5)
        return BuildingOut(g, gear, fpo, ground, eave - ground + 3.5, 2, 411, "", "farmhouse")
    return None
