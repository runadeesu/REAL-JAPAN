"""Special structures of 千景島 (fictional): stations and the elevated loop line, temple with a
five-storey pagoda, summit shrine with torii, broadcast tower, Ferris wheel, stadium, airport,
ferry terminal, bridges, quay walls, river surface, mountain forest canopy and trees.
All designs are generic and invented (no real landmark is reproduced).
"""

from __future__ import annotations

import math
import os
import struct
from dataclasses import dataclass, field

import numpy as np
from shapely import affinity
from shapely.geometry import LineString, Point, Polygon, box as sbox
from shapely.geometry.polygon import orient
from shapely.ops import unary_union

from . import layout as L
from .buildings import (CURTAIN, GLASS, LAMP, METAL, PANEL, PLAIN, PUBLIC, ROOF_FLAT, ROOF_METAL, ROOF_TILE, TEMPLE,
                        WAREHOUSE, WOODEN, BuildingOut, Geo, box, cap, cylinder, pitched_roof, walls)
from .generate import parts
from realjapan_pipeline.streetdetail import Geo as DGeo

RAIL_DECK = 9.0  # rail level above the ground (elevated loop line)


from .railgeom import PLATFORM_LEN, chaikin, resample, stations_aligned, track_piece  # noqa: E402,F401


def beam(g: Geo, p0, p1, t, style, rgb, ground):
    """Square-section member between two 3D points."""
    p0, p1 = np.asarray(p0, float), np.asarray(p1, float)
    d = p1 - p0
    L_ = np.linalg.norm(d)
    if L_ < 1e-3:
        return
    ax = d / L_
    ref = np.array([0, 0, 1.0]) if abs(ax[2]) < 0.9 else np.array([1.0, 0, 0])
    u = np.cross(ax, ref)
    u /= np.linalg.norm(u)
    v = np.cross(ax, u)
    h = t / 2
    cs = [(-1, -1), (1, -1), (1, 1), (-1, 1)]
    for k in range(4):
        a, b = cs[k], cs[(k + 1) % 4]
        o0 = u * a[0] * h + v * a[1] * h
        o1 = u * b[0] * h + v * b[1] * h
        g.quad(p0 + o0, p0 + o1, p1 + o1, p1 + o0, style, rgb, [(1024, 0), (1024 + t, 0), (1024 + t, L_), (1024, L_)])


@dataclass
class Spec:
    buildings: list = field(default_factory=list)
    forest: Polygon = None
    grass: Polygon = None
    fields: Polygon = None
    apron: Polygon = None
    runway: Polygon = None
    marks: Polygon = None
    rails: list = field(default_factory=list)       # (N, 3) polylines, rail level (island z)
    rail_kinds: list = field(default_factory=list)  # "loop" | "branch" | "shinkansen"
    platforms: list = field(default_factory=list)   # (centre x, y, heading deg, length, width, z top)
    piers: list = field(default_factory=list)       # (x, y, heading deg, length)
    bridges: list = field(default_factory=list)     # (deck polygon, line, z0, z1, arch) per bridge
    quays: list = field(default_factory=list)       # LineStrings along seawalls
    trees: list = field(default_factory=list)       # (x, y, h, r, kind)
    canopy: Polygon = None
    tunnel_flags: list = field(default_factory=list)  # per rail line: bool per point (in a tunnel)
    portal_holes: Polygon = None                      # ground cut away at the tunnel mouths
    stations: list = field(default_factory=list)      # walk-in stations (dicts, see station.py)
    road_structs: list = field(default_factory=list)  # generate.RoadStructure: road tunnels and viaducts
    road_struct_area: Polygon = None                  # their footprints (no road painted on the ground there)
    at_grade: list = field(default_factory=list)      # per rail line: bool per point (track on the ground)
    crossings: list = field(default_factory=list)     # level crossings (dicts, see _level_crossings)
    tolls: list = field(default_factory=list)         # expressway toll plazas on the interchange ramps (dicts)
    pas: list = field(default_factory=list)           # parking areas: their shop building (dicts, shop.py plan)
    fuel_lots: list = field(default_factory=list)     # fuel stations on a roadside lot in the towns (dicts)


class CellExtra:
    def __init__(self):
        self.shops = []   # walk-in shops built here outside the building list: (kind, counter xy, stand xy, z)
        self.decks = []
        self.trees = []
        self.walls = []   # collision walls (x0, y0, x1, y1, z low, z high), cell ENU
        self.lights = []  # ((x, y, z), range, kind): kind 3 = indoor light (always on)


def _named(g: Geo, ring, ground, h, usage, name, kind, storeys=1):
    return BuildingOut(g, Geo(), np.asarray(ring)[:-1] if np.allclose(ring[0], ring[-1]) else np.asarray(ring), ground,
                       h, storeys, usage, name, kind)


def _station(st, terrain, big):
    """Station building record: the walk-in concourse under the platforms (its walls, gates and
    stairs are street detail, see station.py; the record carries the name and the roof), and the
    main station's tall station building (department store + offices)."""
    from . import station as S
    name, x, y, hd, ground = st["name"], st["x"], st["y"], st["hd"], st["ground"]
    F = S.Frame(x, y, hd)
    H = S.concourse_height(ground, st["ztop"])
    rect = orient(Polygon([F.xy(S.CU0, -S.CV), F.xy(S.CU1, -S.CV), F.xy(S.CU1, S.CV), F.xy(S.CU0, S.CV)]), 1.0)
    ring = np.asarray(rect.exterior.coords)
    g = Geo()
    cap(g, rect, ground + H + 0.05, ROOF_FLAT, (150, 150, 146))
    b = _named(g, ring, ground, H, 431, name, "station", 1)
    b.walk_in = True
    out = [b]
    side = F.r
    if big:
        c = np.array([x, y]) + side * 44
        trect = orient(affinity.rotate(sbox(c[0] - 22, c[1] - 60, c[0] + 22, c[1] + 60), -hd, origin=tuple(c)), 1.0)
        tring = np.asarray(trect.exterior.coords)
        tg = float(terrain.sample(*c))
        tg_ = Geo()
        walls(tg_, tring, tg - 2, tg + 26, tg, PUBLIC, (210, 206, 196))
        tower = orient(trect.buffer(-7, join_style=2), 1.0)
        walls(tg_, np.asarray(tower.exterior.coords), tg + 26, tg + 148, tg, CURTAIN, (70, 95, 115))
        cap(tg_, trect.difference(tower) if trect.difference(tower).geom_type == "Polygon" else trect, tg + 26, ROOF_FLAT,
            (140, 140, 136))
        cap(tg_, tower, tg + 148, ROOF_FLAT, (150, 150, 146))
        out.append(_named(tg_, tring, tg, 148, 431, name + "ビル", "station_tower", 34))
    return out


def _temple(name, x, y, terrain):
    ground = float(terrain.sample(x, y))
    g = Geo()
    ver, white = (190, 50, 38), (236, 230, 216)
    # main hall on a stone base
    base = orient(sbox(x - 17, y - 13, x + 17, y + 13), 1.0)
    walls(g, np.asarray(base.exterior.coords), ground - 1, ground + 1.2, ground, PLAIN, (150, 146, 138))
    cap(g, base, ground + 1.2, ROOF_FLAT, (160, 156, 148))
    hall = orient(sbox(x - 14, y - 10, x + 14, y + 10), 1.0)
    walls(g, np.asarray(hall.exterior.coords), ground + 1.2, ground + 8.5, ground, TEMPLE, white)
    rect = np.asarray(hall.exterior.coords)[:4]
    pitched_roof(g, rect, ground + 8.5, 36, 3.2, ROOF_TILE, (52, 54, 58), TEMPLE, white, ground, hip=True)
    # five-storey pagoda south-east of the hall
    px, py = x + 42, y - 30
    pg = float(terrain.sample(px, py))
    z = pg
    for tier in range(5):
        s = 4.4 - tier * 0.45
        th = 4.6 if tier == 0 else 3.6
        sq = orient(sbox(px - s, py - s, px + s, py + s), 1.0)
        walls(g, np.asarray(sq.exterior.coords), z - (1 if tier == 0 else 0), z + th, pg, TEMPLE, ver if tier % 2 == 0 else white)
        r = np.asarray(sq.exterior.coords)[:4]
        pitched_roof(g, r, z + th, 24, 2.4, ROOF_TILE, (48, 50, 54), TEMPLE, ver, pg, hip=True)
        z += th + 1.1
    box(g, (px, py, z + 4.0), (0, 1), (0.25, 0.25, 4.5), METAL, (170, 140, 80), pg)  # spire (sōrin)
    # gate on the approach
    gx, gy = x, y - 62
    gg = float(terrain.sample(gx, gy))
    for sx_ in (-5, 5):
        box(g, (gx + sx_, gy, gg + 3.5), (0, 1), (0.5, 0.5, 3.5), PLAIN, ver, gg)
    gate = orient(sbox(gx - 7, gy - 3, gx + 7, gy + 3), 1.0)
    pitched_roof(g, np.asarray(gate.exterior.coords)[:4], gg + 7.0, 30, 1.8, ROOF_TILE, (52, 54, 58), TEMPLE, ver, gg, hip=False)
    fp = np.asarray(orient(sbox(x - 17, y - 13, x + 17, y + 13), 1.0).exterior.coords)
    out = [_named(g, fp, ground, 12, 422, name, "temple")]
    return out


def _torii(g: Geo, x, y, hd, ground, scale=1.0):
    th = math.radians(hd)
    side = np.array([math.cos(th), -math.sin(th)])
    fwd = (math.sin(th), math.cos(th))
    red = (200, 48, 32)
    for s in (-1, 1):
        p = np.array([x, y]) + side * 2.6 * scale * s
        cylinder(g, (p[0], p[1], ground), 0.28 * scale, 5.2 * scale, PLAIN, red, ground, seg=8, top=False)
    c = (x, y, ground + 5.45 * scale)
    box(g, c, fwd, (3.9 * scale, 0.3 * scale, 0.22 * scale), PLAIN, (30, 30, 30), ground)  # kasagi
    box(g, (x, y, ground + 4.6 * scale), fwd, (3.1 * scale, 0.2 * scale, 0.16 * scale), PLAIN, red, ground)


def _shrine(name, x, y, terrain):
    ground = float(terrain.sample(x, y))
    g = Geo()
    hall = orient(sbox(x - 6, y - 5, x + 6, y + 5), 1.0)
    walls(g, np.asarray(hall.exterior.coords), ground - 1, ground + 4.5, ground, TEMPLE, (236, 230, 216))
    pitched_roof(g, np.asarray(hall.exterior.coords)[:4], ground + 4.5, 32, 1.6, ROOF_METAL, (78, 130, 110), TEMPLE,
                 (236, 230, 216), ground, hip=False)
    for k, d in enumerate((16, 30, 44)):
        tx, ty = x, y - d
        _torii(g, tx, ty, 0, float(terrain.sample(tx, ty)), 1.0 + 0.1 * k)
    return [_named(g, np.asarray(hall.exterior.coords), ground, 8, 454, name, "shrine")]


def _tower(name, x, y, terrain):
    ground = float(terrain.sample(x, y))
    g = Geo()
    H = 236.0
    red, white = (206, 60, 36), (236, 236, 232)
    def half(z):
        t = z / H
        return 22 * (1 - t) ** 1.6 + 1.2
    levels = np.arange(0, H + 0.1, 12.0)
    for i in range(len(levels) - 1):
        z0, z1 = levels[i], levels[i + 1]
        col = red if (i // 2) % 2 == 0 else white
        h0, h1 = half(z0), half(z1)
        corners0 = [(x + sx * h0, y + sy * h0, ground + z0) for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
        corners1 = [(x + sx * h1, y + sy * h1, ground + z1) for sx, sy in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
        for k in range(4):
            beam(g, corners0[k], corners1[k], 1.4 if z0 < 60 else 0.9, METAL, col, ground)
            beam(g, corners1[k], corners1[(k + 1) % 4], 0.6, METAL, col, ground)
            beam(g, corners0[k], corners1[(k + 1) % 4], 0.4, METAL, col, ground)  # diagonal bracing
    for zd, s in ((120, 11), (222, 5)):  # observation decks
        sq = orient(sbox(x - s, y - s, x + s, y + s), 1.0)
        walls(g, np.asarray(sq.exterior.coords), ground + zd, ground + zd + 8, ground, GLASS, (80, 110, 130))
        cap(g, sq, ground + zd + 8, ROOF_FLAT, (200, 200, 196))
        cap(g, sq, ground + zd, ROOF_FLAT, (180, 180, 176), up=False)
    box(g, (x, y, ground + H + 14), (0, 1), (0.5, 0.5, 14), METAL, white, ground)
    for zl in (60, 120, 180, 236):  # aviation / illumination lamps
        box(g, (x + half(zl), y, ground + zl), (0, 1), (0.4, 0.4, 0.4), LAMP, (255, 60, 40), ground)
    fp = np.asarray(orient(sbox(x - 23, y - 23, x + 23, y + 23), 1.0).exterior.coords)
    return [_named(g, fp, ground, H + 28, 454, name, "tower")]


def _wheel(name, x, y, terrain):
    ground = float(terrain.sample(x, y))
    g = Geo()
    R, hub = 45.0, ground + 52.0
    n = 36
    ring = [(x + R * math.cos(2 * math.pi * k / n), y + 0.0, hub + R * math.sin(2 * math.pi * k / n)) for k in range(n)]
    for k in range(n):
        a, b = ring[k], ring[(k + 1) % n]
        for dy in (-2.0, 2.0):
            beam(g, (a[0], y + dy, a[2]), (b[0], y + dy, b[2]), 0.8, METAL, (230, 232, 236), ground)
        if k % 2 == 0:
            beam(g, (x, y, hub), (a[0], y, a[2]), 0.3, METAL, (200, 200, 205), ground)
            box(g, (a[0], y, a[2] - 2.0), (0, 1), (1.2, 1.2, 1.3), GLASS, (120, 170, 210), ground)
            box(g, (a[0], y + 2.2, a[2]), (0, 1), (0.3, 0.1, 0.3), LAMP, (255, 220, 160), ground)
    for s in (-1, 1):  # A-frame legs
        for dy in (-5, 5):
            beam(g, (x + s * 22, y + dy, ground), (x, y + dy * 0.3, hub), 1.6, METAL, (220, 220, 225), ground)
    fp = np.asarray(orient(sbox(x - 25, y - 8, x + 25, y + 8), 1.0).exterior.coords)
    return [_named(g, fp, ground, 100, 454, name, "wheel")]


def _stadium(name, x, y, terrain):
    ground = float(terrain.sample(x, y))
    g = Geo()
    ell = orient(affinity.scale(Point(x, y).buffer(1.0, 48), 125, 95), 1.0)
    walls(g, np.asarray(ell.exterior.coords), ground - 1, ground + 24, ground, PUBLIC, (206, 204, 198))
    inner = orient(affinity.scale(Point(x, y).buffer(1.0, 48), 85, 58), 1.0)
    # stands: sloped ring from the field edge up to the rim (inward-facing surface)
    P_out = np.asarray(ell.exterior.coords)[:-1]
    P_in = np.asarray(inner.exterior.coords)[:-1]
    for k in range(len(P_out)):
        a, b = P_in[k], P_in[(k + 1) % len(P_in)]
        c, d = P_out[(k + 1) % len(P_out)], P_out[k]
        g.quad((a[0], a[1], ground + 1), (b[0], b[1], ground + 1), (c[0], c[1], ground + 22), (d[0], d[1], ground + 22),
               PANEL, (150, 60, 50) if k % 6 < 3 else (60, 90, 150), [(0, 0), (10, 0), (10, 30), (0, 30)])
    rim = ell.difference(orient(affinity.scale(Point(x, y).buffer(1.0, 48), 118, 88), 1.0))
    for part in parts(rim):
        cap(g, part, ground + 24, ROOF_FLAT, (200, 200, 196))
    return [_named(g, np.asarray(ell.exterior.coords), ground, 24, 422, name, "stadium")]


def _airport(terrain, spec: Spec, ap, ground_z):
    (ax, ay), (bx, by), w = ap["runway"]
    rw = LineString([(ax, ay), (bx, by)])
    d = np.array([bx - ax, by - ay])
    Lr = np.linalg.norm(d)
    ux = d / Lr
    nx = np.array([-ux[1], ux[0]])
    tx, ty = ap["terminal"]
    # the terminal side of the runway (the taxiway runs between them)
    if float((np.array([tx, ty]) - np.array([ax, ay])) @ nx) < 0:
        nx = -nx
    big = ap["reclaimed"] is not None
    tw = 190.0 if big else 150.0
    runway = rw.buffer(w / 2, cap_style=2)
    twy = LineString([(ax, ay) + nx * tw, (bx, by) + nx * tw])
    taxi = twy.buffer(12, cap_style=2)
    for t in (0.08, 0.35, 0.65, 0.92):
        p0 = np.array([ax, ay]) + d * t
        taxi = taxi.union(LineString([p0, p0 + nx * tw]).buffer(11.5))
    along_t = float((np.array([tx, ty]) - np.array([ax, ay])) @ ux)
    ap_w = 380.0 if big else 180.0
    for s_ in (along_t - ap_w * 0.53, along_t + ap_w * 0.53):  # taxiway -> apron links
        p0 = np.array([ax, ay]) + ux * s_
        taxi = taxi.union(LineString([p0 + nx * tw, p0 + nx * (tw + 110)]).buffer(11.5))
    ang = math.degrees(math.atan2(ux[1], ux[0]))
    ac = np.array([ax, ay]) + ux * along_t + nx * (tw + 190)
    apron = orient(affinity.rotate(sbox(ac[0] - ap_w, ac[1] - 80, ac[0] + ap_w, ac[1] + 80), ang, origin=tuple(ac)), 1.0)
    spec.runway = runway if spec.runway is None else spec.runway.union(runway)
    a_all = unary_union([apron, taxi]).difference(runway)
    spec.apron = a_all if spec.apron is None else spec.apron.union(a_all)
    field_ = rw.buffer(150, cap_style=2).union(Point(tx, ty).buffer(ap_w + 60))
    if big:
        field_ = field_.union(Polygon(ap["reclaimed"]))
    g_ = field_.difference(a_all).difference(runway.buffer(1))
    spec.grass = g_ if spec.grass is None else spec.grass.union(g_)
    marks = []
    s = 60.0
    while s < Lr - 60:  # centreline dashes
        p = np.array([ax, ay]) + ux * s
        marks.append(LineString([p, p + ux * 30]).buffer(0.45, cap_style=2))
        s += 50
    for end, sgn in ((np.array([ax, ay]), 1), (np.array([bx, by]), -1)):  # threshold piano keys + edge lines
        for k in range(-7, 8):
            if k == 0:
                continue
            p = end + ux * sgn * 8 + nx * k * 2.6
            marks.append(LineString([p, p + ux * sgn * 30]).buffer(0.9, cap_style=2))
    for sn in (-1, 1):
        marks.append(LineString([np.array([ax, ay]) + nx * sn * (w / 2 - 1.5), np.array([bx, by]) + nx * sn * (w / 2 - 1.5)]).buffer(0.45, cap_style=2))
    m_ = unary_union(marks)
    spec.marks = m_ if spec.marks is None else spec.marks.union(m_)
    out = []
    ground = float(terrain.sample(tx, ty))
    g = Geo()
    tc = ac + nx * (80 + (60 if big else 35))
    th = 60 if big else 35
    tl = 160 if big else 70
    term = orient(affinity.rotate(sbox(tc[0] - tl, tc[1] - th / 2, tc[0] + tl, tc[1] + th / 2), ang, origin=tuple(tc)), 1.0)
    walls(g, np.asarray(term.exterior.coords), ground - 1, ground + (22 if big else 12), ground, CURTAIN, (90, 120, 140))
    cap(g, term, ground + (22 if big else 12), ROOF_METAL, (190, 194, 198))
    out.append(_named(g, np.asarray(term.exterior.coords), ground, 22 if big else 12, 431, ap["name"] + "ターミナル", "terminal", 4 if big else 2))
    g2 = Geo()
    ctx, cty = ac + ux * (ap_w * 0.6) + nx * 120
    ht = 52 if big else 26
    cylinder(g2, (ctx, cty, ground - 1), 3.2, ht, PANEL, (220, 220, 216), ground, seg=12, top=False)
    cylinder(g2, (ctx, cty, ground + ht - 1), 6.5, 6, GLASS, (60, 90, 110), ground, seg=12)
    out.append(_named(g2, np.asarray(Point(ctx, cty).buffer(6.5, 8).exterior.coords), ground, ht + 6, 431, "管制塔", "control_tower", 12))
    for k in range(3 if big else 1):  # hangars
        g3 = Geo()
        hx, hy = ac - ux * (ap_w * 0.87 + k * 90) + nx * 40
        hg = orient(affinity.rotate(sbox(hx - 38, hy - 30, hx + 38, hy + 30), ang, origin=(hx, hy)), 1.0)
        walls(g3, np.asarray(hg.exterior.coords), ground - 1, ground + 20, ground, PLAIN, (200, 204, 208))
        cap(g3, hg, ground + 20, ROOF_METAL, (170, 175, 180))
        out.append(_named(g3, np.asarray(hg.exterior.coords), ground, 20, 431, "", "hangar", 1))
    return out


def _ferry_terminal(terrain, name, x, y, hd):
    """Terminal building on the shore behind the pier root (away from the sea)."""
    th = math.radians(hd)
    back = -np.array([math.sin(th), math.cos(th)])
    c = np.array([x, y]) + back * 45
    ground = float(terrain.sample(*c))
    g = Geo()
    b = orient(affinity.rotate(sbox(c[0] - 40, c[1] - 20, c[0] + 40, c[1] + 20), -hd, origin=tuple(c)), 1.0)
    walls(g, np.asarray(b.exterior.coords), ground - 1, ground + 13, ground, PUBLIC, (214, 214, 208))
    cap(g, b, ground + 13, ROOF_METAL, (80, 120, 160))
    return [_named(g, np.asarray(b.exterior.coords), ground, 13, 431, name + "フェリーターミナル", "ferry_terminal", 2)]


def _castle(name, x, y, terrain):
    """Castle keep on a battered stone base: white plastered storeys under dark tiled roofs."""
    ground = float(terrain.sample(x, y))
    g = Geo()
    base_top = ground + 11.0
    s0 = 21.0
    for k in range(6):  # battered stone base (steps)
        s_ = s0 - k * 0.6
        sq = orient(sbox(x - s_, y - s_ * 0.8, x + s_, y + s_ * 0.8), 1.0)
        walls(g, np.asarray(sq.exterior.coords), ground - 1 + k * 2.0 - (1 if k == 0 else 0), ground + 1 + k * 2.0, ground, PLAIN,
              (150, 146, 138))
    cap(g, orient(sbox(x - s0 + 3.6, y - (s0 - 3.6) * 0.8, x + s0 - 3.6, y + (s0 - 3.6) * 0.8), 1.0), base_top, ROOF_FLAT, (160, 156, 148))
    z = base_top
    for tier in range(5):
        hx = 15.0 - tier * 2.4
        hy = hx * 0.8
        sq = orient(sbox(x - hx, y - hy, x + hx, y + hy), 1.0)
        th = 4.6 if tier < 4 else 4.0
        walls(g, np.asarray(sq.exterior.coords), z, z + th, ground, TEMPLE, (240, 238, 232))
        r = np.asarray(sq.exterior.coords)[:4]
        pitched_roof(g, r, z + th, 30, 1.8, ROOF_TILE, (60, 64, 70), TEMPLE, (240, 238, 232), ground, hip=True)
        z += th + 1.6
    fp = np.asarray(orient(sbox(x - s0, y - s0 * 0.8, x + s0, y + s0 * 0.8), 1.0).exterior.coords)
    return [_named(g, fp, ground, z - ground + 4, 454, name, "castle")]


def _torii_path(name, x, y, terrain, hd=0.0, n=60, spacing=2.4):
    """Shrine with a long tunnel of vermilion torii up the hillside behind it (generic design)."""
    out = _shrine(name, x, y, terrain)
    g = Geo()
    th = math.radians(hd)
    fwd = np.array([math.sin(th), math.cos(th)])
    for k in range(n):
        p = np.array([x, y]) + fwd * (20 + k * spacing)
        gz = float(terrain.sample(*p))
        _torii(g, p[0], p[1], hd, gz, 0.72)
    fp = np.asarray(orient(sbox(x - 3, y + 18, x + 3, y + 20 + n * spacing), 1.0).exterior.coords) if hd == 0 else None
    out.append(BuildingOut(g, Geo(), np.asarray([[x - 2.5, y + 18], [x + 2.5, y + 18], [x + 2.5, y + 19], [x - 2.5, y + 19]]),
                           float(terrain.sample(x, y)), 5, 1, 454, "", "torii"))
    return out


def _lighthouse(name, x, y, terrain):
    ground = float(terrain.sample(x, y))
    g = Geo()
    cylinder(g, (x, y, ground - 1), 3.2, 25, PLAIN, (240, 240, 236), ground, seg=16, top=False)
    cylinder(g, (x, y, ground + 24), 3.8, 0.6, PLAIN, (60, 60, 60), ground, seg=16)
    cylinder(g, (x, y, ground + 24.6), 2.2, 3.0, GLASS, (120, 150, 160), ground, seg=12, top=False)
    box(g, (x, y, ground + 26.0), (0, 1), (0.5, 0.5, 0.5), LAMP, (255, 240, 200), ground)
    cylinder(g, (x, y, ground + 27.6), 2.5, 1.2, METAL, (60, 70, 60), ground, seg=12)
    return [_named(g, np.asarray(Point(x, y).buffer(3.2, 8).exterior.coords), ground, 29, 454, name, "lighthouse")]


def _cranes(name, x, y, terrain, hd=80.0, n=4):
    """Container gantry cranes along the quay (generic)."""
    ground = float(terrain.sample(x, y))
    g = Geo()
    th = math.radians(hd)
    fwd = np.array([math.sin(th), math.cos(th)])
    side = np.array([math.cos(th), -math.sin(th)])
    blue, white = (50, 110, 170), (230, 230, 226)
    for k in range(n):
        c = np.array([x, y]) + fwd * (k - (n - 1) / 2) * 70
        for sg in (-1, 1):
            for sf in (-1, 1):
                p = c + side * 9 * sg + fwd * 8 * sf
                box(g, (p[0], p[1], ground + 19), (float(fwd[0]), float(fwd[1])), (0.7, 0.7, 19), METAL, blue, ground)
        for sf in (-1, 1):  # boom girders reaching over the water and back over the yard
            a = c + fwd * 8 * sf - side * 30
            b = c + fwd * 8 * sf + side * 50
            beam(g, (a[0], a[1], ground + 38), (b[0], b[1], ground + 38), 1.6, METAL, white, ground)
        box(g, (c[0], c[1], ground + 41), (float(fwd[0]), float(fwd[1])), (10, 9, 2.5), METAL, white, ground)
    fp = np.asarray(orient(affinity.rotate(sbox(x - 12, y - n * 36, x + 12, y + n * 36), -hd, origin=(x, y)), 1.0).exterior.coords)
    return [_named(g, fp, ground, 44, 441, name, "cranes")]


def _hut(name, x, y, terrain):
    ground = float(terrain.sample(x, y))
    g = Geo()
    b = orient(sbox(x - 14, y - 8, x + 14, y + 8), 1.0)
    walls(g, np.asarray(b.exterior.coords), ground - 1, ground + 4.5, ground, WOODEN, (110, 86, 62))
    pitched_roof(g, np.asarray(b.exterior.coords)[:4], ground + 4.5, 28, 0.9, ROOF_METAL, (130, 60, 50), WOODEN, (110, 86, 62), ground, hip=False)
    return [_named(g, np.asarray(b.exterior.coords), ground, 9, 454, name, "hut")]


def build_all(ctry, terrain, rng) -> Spec:
    from .railgeom import rail_lines2d, rail_profile
    spec = Spec()
    t = L.LANDMARKS
    spec.buildings += _temple(t["temple"][0], t["temple"][1], t["temple"][2], terrain)
    spec.buildings += _shrine(t["shrine"][0], t["shrine"][1], t["shrine"][2], terrain)
    spec.buildings += _tower(t["tower"][0], t["tower"][1], t["tower"][2], terrain)
    spec.buildings += _wheel(t["wheel"][0], t["wheel"][1], t["wheel"][2], terrain)
    spec.buildings += _stadium(t["stadium"][0], t["stadium"][1], t["stadium"][2], terrain)
    spec.buildings += _temple(t["old_temple"][0], t["old_temple"][1], t["old_temple"][2], terrain)
    spec.buildings += _torii_path(t["torii_shrine"][0], t["torii_shrine"][1], t["torii_shrine"][2], terrain, hd=80.0)
    spec.buildings += _castle(t["castle"][0], t["castle"][1], t["castle"][2], terrain)
    spec.buildings += _shrine(t["onsen_shrine"][0], t["onsen_shrine"][1], t["onsen_shrine"][2], terrain)
    spec.buildings += _shrine(t["south_shrine"][0], t["south_shrine"][1], t["south_shrine"][2], terrain)
    spec.buildings += _lighthouse(t["lighthouse"][0], t["lighthouse"][1], t["lighthouse"][2], terrain)
    spec.buildings += _cranes(t["port_cranes"][0], t["port_cranes"][1], t["port_cranes"][2], terrain)
    spec.buildings += _hut(t["volcano_hut"][0], t["volcano_hut"][1], t["volcano_hut"][2], terrain)
    for k, ap in enumerate(L.AIRPORTS):
        spec.buildings += _airport(terrain, spec, ap, ctry.airport_z[k] if k < len(ctry.airport_z) else None)
    for name, (x, y, hd) in L.PIERS.items():
        if name != "台場":
            spec.buildings += _ferry_terminal(terrain, name.replace("港", "港"), x, y, hd)
    spec.fields = Polygon()
    # rail lines (smoothed centre lines, grade-limited profiles with level stations)
    lines = rail_lines2d()
    st_all = stations_aligned(lines)
    # station yards: level ground under each concourse and its entrances, before the rail levels
    from . import station as S
    from .generate import flatten_pad
    yard_z = []
    for name, x, y, hd, li, k in st_all:
        F = S.Frame(x, y, hd)
        yard = Polygon([F.xy(S.CU0 - 4, -S.CV - 6), F.xy(S.CU1 + 4, -S.CV - 6), F.xy(S.CU1 + 4, S.CV + 6), F.xy(S.CU0 - 4, S.CV + 6)])
        yard_z.append(flatten_pad(terrain, yard, z=float(terrain.sample(x, y)), verge=30.0))
    for li, ln in enumerate(L.RAIL_LINES):
        P = lines[li]
        ks = [k for (_, _, _, _, l2, k) in st_all if l2 == li]
        # the platforms stand high enough over the yard for the concourse under them
        zmin = [yard_z[i] + (9.0 if ln["kind"] == "shinkansen" else 6.2) for i, s_ in enumerate(st_all) if s_[4] == li]
        xs = _level_crossings(ctry, P, li, [st_all[i][1:3] for i in range(len(st_all)) if st_all[i][4] == li], terrain) if ln["kind"] == "branch" else []
        z = rail_profile(P, ln["kind"], ln["closed"], terrain, ks, PLATFORM_LEN[li], zmin, [(c["k"], c["z"] - 0.19) for c in xs])
        spec.rails.append(np.column_stack([P, z]))
        spec.rail_kinds.append(ln["kind"])
        spec.tunnel_flags.append(_tunnels(P, z, terrain))
        spec.crossings.extend(xs)
    holes = []
    xing_keep = unary_union([c["area"] for c in spec.crossings]) if spec.crossings else None
    for li, R in enumerate(spec.rails):
        tun = spec.tunnel_flags[li]
        _cut_for_rail(terrain, R, tun, keep=xing_keep)
        if spec.rail_kinds[li] == "branch":
            _fill_for_rail(terrain, R, tun, keep=xing_keep)
        gz = terrain.sample(R[:, 0], R[:, 1])
        urb = np.array([L.urban(x, y) for x, y in R[:, :2]])
        spec.at_grade.append((spec.rail_kinds[li] == "branch") & ~tun & ~urb & (R[:, 2] - gz < 1.0))
        # the ground over the first metres inside each tunnel mouth is cut away (the portal wall
        # stands at the mouth): where the natural surface would cross the tube
        n = len(R)
        for i in range(n):
            if tun[i] and ((i > 0 and not tun[i - 1]) or (i + 1 < n and not tun[i + 1])):
                dirn = 1 if (i > 0 and not tun[i - 1]) else -1
                seg = [R[j, :2] for j in range(i, min(n, max(-1, i + dirn * 8)), dirn) if 0 <= j < n]
                if len(seg) >= 2:
                    holes.append(LineString(seg).buffer(7.5 if spec.rail_kinds[li] == "shinkansen" else 6.5, cap_style=2))
    # road tunnels and viaducts: the ground at the tunnel mouths is cut away like the railway's
    spec.road_structs = list(getattr(ctry, "road_structs", []) or [])
    areas = []
    for rs in spec.road_structs:
        P = rs.pts
        areas.append(LineString(P[:, :2]).buffer(rs.width / 2 + 0.6, cap_style=2))
        if rs.kind == "tunnel" and len(P) >= 3:
            for end, dirn in ((0, 1), (len(P) - 1, -1)):
                seg = [P[j, :2] for j in range(end, min(len(P), max(-1, end + dirn * 3)), dirn) if 0 <= j < len(P)]
                if len(seg) >= 2:
                    holes.append(LineString(seg).buffer(rs.width / 2 + 1.5, cap_style=2))
    spec.road_struct_area = unary_union(areas) if areas else Polygon()
    spec.portal_holes = unary_union(holes) if holes else Polygon()
    spec.tolls = _toll_plazas(ctry, terrain, spec.road_struct_area)
    spec.pas = _parking_areas(ctry, terrain)
    # stations snapped onto the smoothed lines: concourse (and the main station's tower), walkable
    # platform decks level with the line and following its curve
    for si, (name, x, y, hd, li, k) in enumerate(st_all):
        R = spec.rails[li]
        kind = spec.rail_kinds[li]
        ztop = float(R[k, 2]) + 1.1
        st = dict(index=si, name=name, x=x, y=y, hd=hd, line=li, ground=float(terrain.sample(x, y)), ztop=ztop,
                  off=S.platform_offset(kind == "shinkansen"), shink=kind == "shinkansen")
        spec.stations.append(st)
        spec.buildings.extend(_station(st, terrain, big=(name == "千景中央駅")))
        length = PLATFORM_LEN[li]
        path = track_piece(R, k, length / 2, closed=L.RAIL_LINES[li]["closed"])
        spec.platforms.append((x, y, hd, length, 5.0, ztop, li, name, path))
    for name, (x, y, hd) in L.PIERS.items():
        spec.piers.append((x, y, hd, 120.0))
    # bridges: roads crossing rivers (flat decks) and the capital's bay bridge (arched)
    import shapely
    water_all = unary_union([rv.poly for rv in ctry.rivers])
    for r in ctry.net.roads:
        if r.kind in ("expressway", "ramp"):  # (their viaducts are road structures)
            continue
        for arch in (False, True):
            if arch:
                if r.name not in L.BRIDGE_ROADS:
                    continue
                seg = r.line.difference(ctry.land.buffer(-2))
            else:
                if not r.line.intersects(water_all):
                    continue
                seg = r.line.intersection(water_all.buffer(10))
            for part in getattr(seg, "geoms", [seg]):
                if not isinstance(part, LineString) or part.length < 8:
                    continue
                c = np.asarray(part.coords)
                z0 = float(terrain.sample(*c[0]))
                z1 = float(terrain.sample(*c[-1]))
                if arch:
                    z0, z1 = max(z0, 2.5), max(z1, 2.5)
                deck = part.buffer(r.width / 2, cap_style=2)
                spec.bridges.append((deck, part, z0, z1, arch, r.width))
    # seawalls: the capital's shores and the harbours (natural coasts elsewhere)
    beach = Polygon(L.BEACH).buffer(120)
    harbours = unary_union([Point(x, y).buffer(700) for (x, y, hd) in L.PIERS.values()]
                           + [box_(-4300, -3600, 4300, 3900)]
                           + [Polygon(p).buffer(200) for n, s, p in L.DISTRICTS if s == "port"])
    for poly in parts(ctry.land):
        ring = LineString(poly.exterior.coords)
        g = ring.intersection(harbours).difference(beach)
        for part in getattr(g, "geoms", [g]):
            if isinstance(part, LineString) and part.length > 10:
                spec.quays.append(part)
    # trees: street trees on wide roads, parks, temple / shrine groves
    for r in ctry.net.roads:
        if r.sidewalk < 3.5:
            continue
        nb = ctry.net.near(r.line, 30.0)
        carr = unary_union([o.line.buffer(o.carriage / 2 + 0.4) for o in nb])
        shapely.prepare(carr)
        L_ = r.line.length
        for side in (-1, 1):
            try:
                off = r.line.offset_curve(side * (r.width / 2 - r.sidewalk + 1.1))
            except Exception:
                continue
            s = rng.uniform(0, 9)
            while s < off.length:
                p = off.interpolate(s)
                if not carr.contains(p):
                    spec.trees.append((p.x, p.y, rng.uniform(7, 11), rng.uniform(2.2, 3.4), 0))
                s += rng.uniform(8.5, 10.5)
    for name, pp in L.PARKS:
        P = Polygon(pp)
        minx, miny, maxx, maxy = P.bounds
        n = int(P.area / 170)
        for _ in range(n):
            x, y = rng.uniform(minx, maxx), rng.uniform(miny, maxy)
            if P.contains(Point(x, y)):
                spec.trees.append((x, y, rng.uniform(8, 16), rng.uniform(3, 5.5), 0))
    for key, n_, r0, r1 in (("temple", 60, 40, 105), ("old_temple", 90, 45, 130), ("torii_shrine", 50, 25, 90),
                            ("onsen_shrine", 30, 18, 50), ("south_shrine", 30, 18, 50), ("shrine", 25, 14, 40)):
        tx, ty = t[key][1], t[key][2]
        for _ in range(n_):
            a, rr = rng.uniform(0, 2 * math.pi), rng.uniform(r0, r1)
            spec.trees.append((tx + rr * math.cos(a), ty + rr * math.sin(a), rng.uniform(9, 20), rng.uniform(3, 5), 0))
    return spec


def _tunnels(P, z, terrain, cover=9.0, min_run=6):
    """Points in tunnel: the ground is at least `cover` m above the rail (short runs and gaps closed)."""
    gz = terrain.sample(P[:, 0], P[:, 1])
    t = gz > z + cover
    # close short open gaps between tunnels, drop very short tunnels (they become cuttings)
    from scipy.ndimage import binary_closing, binary_opening
    t = binary_closing(t, structure=np.ones(min_run, bool))
    t = binary_opening(t, structure=np.ones(min_run, bool))
    return t


def crossing_sets(c, hw=5.0):
    """The warning post + barrier of each approach of a level crossing: (pivot x, y, facing heading of
    the approaching traffic's view (deg), arm heading (deg), arm length). Traffic keeps left, so each
    set stands at the left edge of its approach and its arm reaches across the approaching lanes."""
    th = math.radians(c["road_hd"])
    u = np.array([math.sin(th), math.cos(th)])
    out = []
    for w in (u, -u):  # direction of travel towards the tracks
        left = np.array([-w[1], w[0]])
        piv = np.array([c["x"], c["y"]]) - w * (hw + 2.4) + left * (c["half"] + 0.7)
        facing = math.degrees(math.atan2(-w[0], -w[1])) % 360   # the lamps look at the oncoming traffic
        arm = math.degrees(math.atan2(-left[0], -left[1])) % 360  # the arm swings down across the lane
        out.append((float(piv[0]), float(piv[1]), facing, arm, c["half"] + 0.4))
    return out


def _level_crossing(geos, ex, xf, c, hw):
    th = math.radians(c["road_hd"])
    u = np.array([math.sin(th), math.cos(th)])
    left = np.array([-u[1], u[0]])
    ctr = np.array([c["x"], c["y"]])
    zt = c["z"]  # road / rail-top level
    L_ = hw + 1.2
    # rubber road panels across the tracks (the cars drive on them)
    q = [ctr - u * L_ - left * c["half"], ctr + u * L_ - left * c["half"], ctr + u * L_ + left * c["half"], ctr - u * L_ + left * c["half"]]
    V = xf.p([[p[0], p[1], zt + 0.01] for p in q])
    _dquad(geos, "asphalt", V, (70, 70, 72, 255))
    ex.decks.append(np.array([[V[0], V[1], V[2]], [V[0], V[2], V[3]]]))
    for sg in (-1, 1):  # yellow and black kerb marks along the panel edges
        e0, e1 = ctr - u * L_ + left * sg * (c["half"] + 0.05), ctr + u * L_ + left * sg * (c["half"] + 0.05)
        E = xf.p([[e0[0], e0[1], zt], [e1[0], e1[1], zt]])
        _dquad(geos, "sign", [E[0], E[1], E[1] + [0, 0, 0.12], E[0] + [0, 0, 0.12]], (230, 190, 30, 255))
    for px, py, facing, arm, alen in crossing_sets(c, hw):
        P0 = xf.p([[px, py, zt]])[0]
        f = math.radians(facing)
        fv = np.array([math.sin(f), math.cos(f), 0.0])
        sv = np.array([math.cos(f), -math.sin(f), 0.0])  # to the right as seen by the oncoming traffic
        # warning post: black and yellow striped pole, crossbuck (X) sign, lamp housings, bell box
        for k in range(7):
            col = (24, 24, 24, 255) if k % 2 == 0 else (235, 190, 20, 255)
            _box_d(geos, "sign", P0 + [0, 0, 0.21 + k * 0.42], (0.06, 0.06, 0.21), col)
        for rot in (1, -1):  # the X: two crossed yellow boards with black edges
            cx = P0 + [0, 0, 3.55] + fv * 0.08
            a1 = cx + sv * 0.55 + np.array([0, 0, 0.32]) * rot
            a2 = cx - sv * 0.55 - np.array([0, 0, 0.32]) * rot
            w_ = np.array([0, 0, 0.1])
            _dquad(geos, "sign", [a1 - w_, a2 - w_, a2 + w_, a1 + w_], (240, 200, 30, 255))
            _dquad(geos, "sign", [a1 - w_ * 1.35 - fv * 0.01, a2 - w_ * 1.35 - fv * 0.01, a2 + w_ * 1.35 - fv * 0.01, a1 + w_ * 1.35 - fv * 0.01],
                   (20, 20, 20, 255))
        _box_d(geos, "metal_dark", P0 + [0, 0, 3.05] + fv * 0.05, (0.18, 0.08, 0.18))       # bell / direction box
        _box_d(geos, "metal_dark", P0 + [0, 0, 2.45] + fv * 0.12, (0.55, 0.05, 0.03))       # lamp bracket
        for sd in (-1, 1):
            _box_d(geos, "metal_dark", P0 + [0, 0, 2.45] + fv * 0.16 + sv * sd * 0.36, (0.17, 0.06, 0.17))  # lamp housings
        # barrier machine (the arm itself is drawn and moved by the client)
        _box_d(geos, "sign", P0 + [0, 0, 0.55] - fv * 0.45, (0.22, 0.22, 0.55), (225, 180, 30, 255))
        ex.walls.append((P0[0] - 0.3, P0[1] - 0.3, P0[0] + 0.3, P0[1] + 0.3, P0[2] - 0.2, P0[2] + 1.2))


def _level_crossings(ctry, P, li, stations_xy, terrain):
    """Level crossings of the at-grade main line: where a road crosses it outside the towns and away
    from the stations. Each: rail index k, position, road level z, headings, road half width, and the
    area kept as the road is (no cutting or bank)."""
    import shapely
    line = LineString(P)
    out = []
    struct = getattr(ctry, "road_structs", []) or []
    s_area = unary_union([LineString(rs.pts[:, :2]).buffer(rs.width / 2 + 2) for rs in struct]) if struct else None
    for r in ctry.net.near(line, 1.0):
        if r.carriage < 3.0:
            continue
        g = r.line.intersection(line)
        for q in getattr(g, "geoms", [g]):
            if not isinstance(q, Point):
                continue
            x, y = q.x, q.y
            if L.urban(x, y) or any(math.hypot(x - sx, y - sy) < 260.0 for sx, sy in stations_xy):
                continue
            if s_area is not None and s_area.contains(q):
                continue
            if any(math.hypot(x - c["x"], y - c["y"]) < 40.0 for c in out):
                continue
            k = int(np.argmin(np.hypot(P[:, 0] - x, P[:, 1] - y)))
            k0, k1 = max(0, k - 1), min(len(P) - 1, k + 1)
            rd = P[k1] - P[k0]
            t = r.line.project(q)
            a = np.asarray(r.line.interpolate(max(0.0, t - 3.0)).coords[0])
            b = np.asarray(r.line.interpolate(min(r.line.length, t + 3.0)).coords[0])
            wd = b - a
            if np.linalg.norm(wd) < 1e-3 or np.linalg.norm(rd) < 1e-3:
                continue
            c_ang = abs(float(np.dot(wd, rd)) / (np.linalg.norm(wd) * np.linalg.norm(rd)))
            if c_ang > 0.87:  # (too oblique: under 30 degrees)
                continue
            half = r.carriage / 2 + 0.5
            seg = LineString([r.line.interpolate(max(0.0, t - 26.0)), r.line.interpolate(min(r.line.length, t + 26.0))])
            out.append(dict(x=x, y=y, k=k, line=li, z=float(terrain.sample(x, y)), half=half,
                            rail_hd=math.degrees(math.atan2(rd[0], rd[1])) % 360, road_hd=math.degrees(math.atan2(wd[0], wd[1])) % 360,
                            area=seg.buffer(half + 1.5, cap_style=2)))
    print(f"    line {li}: {len(out)} level crossings", flush=True)
    return out


def _fill_for_rail(terrain, R, tun, keep=None, half=4.2, slope=1.5, top=0.35, max_fill=7.0):
    """Banks: where the at-grade line runs a little above the ground, the ground is raised under it
    to the formation (side slopes 1 : slope). Higher than max_fill it stays a viaduct."""
    from .terrain import line_distance
    import shapely
    Rs = terrain.RES
    n = len(R)
    gz = terrain.sample(R[:, 0], R[:, 1])
    need = (~tun) & (R[:, 2] - top > gz) & (R[:, 2] - gz < max_fill)
    for a, b in _open_runs(need):
        P = R[max(0, a - 2):min(n, b + 2)]
        pad = half + max_fill * slope + 2 * Rs
        i0 = max(0, int((P[:, 1].min() - pad - terrain.y0) / Rs))
        i1 = min(terrain.ny, int((P[:, 1].max() + pad - terrain.y0) / Rs) + 2)
        j0 = max(0, int((P[:, 0].min() - pad - terrain.x0) / Rs))
        j1 = min(terrain.nx, int((P[:, 0].max() + pad - terrain.x0) / Rs) + 2)
        if i1 <= i0 or j1 <= j0:
            continue
        wx0, wy0 = terrain.x0 + j0 * Rs, terrain.y0 + i0 * Rs
        d, kk = line_distance(P[:, :2], (i1 - i0, j1 - j0), wx0, wy0, Rs)
        kk = np.clip(kk, 0, len(P) - 1)
        fill = P[kk, 2] - top - np.maximum(d - half, 0.0) / slope
        win = terrain.h[i0:i1, j0:j1]
        ok = (d < pad) & (fill - win < max_fill) & (win > 0.5)
        if keep is not None:
            X, Y = np.meshgrid(wx0 + np.arange(j1 - j0) * Rs, wy0 + np.arange(i1 - i0) * Rs)
            ok &= ~shapely.contains_xy(keep, X, Y)
        terrain.h[i0:i1, j0:j1] = np.where(ok, np.maximum(win, fill), win).astype(np.float32)


def _open_runs(mask):
    d = np.diff(np.concatenate([[0], np.asarray(mask, np.int8), [0]]))
    return list(zip(np.nonzero(d == 1)[0], np.nonzero(d == -1)[0]))


def _cut_for_rail(terrain, R, tun, half=6.5, slope=1.4, chunk=160, keep=None):
    """Cuttings: where the line runs below the ground outside the tunnels, the ground is dug down to
    the formation with side slopes (1 : 1/slope); not in `keep` (the roads at level crossings)."""
    import shapely
    from .terrain import line_distance
    Rs = terrain.RES
    n = len(R)
    gz = terrain.sample(R[:, 0], R[:, 1])
    need = (~tun) & (gz > R[:, 2] - 0.8)
    k = 0
    while k < n - 1:
        if not need[k]:
            k += 1
            continue
        k1 = min(n, k + chunk)
        P = R[max(0, k - 2):min(n, k1 + 2)]
        x0b, y0b = P[:, 0].min(), P[:, 1].min()
        x1b, y1b = P[:, 0].max(), P[:, 1].max()
        pad = half + 40.0
        i0 = max(0, int((y0b - pad - terrain.y0) / Rs))
        i1 = min(terrain.ny, int((y1b + pad - terrain.y0) / Rs) + 2)
        j0 = max(0, int((x0b - pad - terrain.x0) / Rs))
        j1 = min(terrain.nx, int((x1b + pad - terrain.x0) / Rs) + 2)
        wx0, wy0 = terrain.x0 + j0 * Rs, terrain.y0 + i0 * Rs
        d, kk = line_distance(P[:, :2], (i1 - i0, j1 - j0), wx0, wy0, Rs)
        kk = np.clip(kk, 0, len(P) - 1)
        zr = P[kk, 2]
        intun = tun[np.clip(kk + max(0, k - 2), 0, n - 1)]
        win = terrain.h[i0:i1, j0:j1]
        cut = zr - 0.3 + np.maximum(d - half, 0.0) * slope
        ok = (d < pad) & ~intun
        if keep is not None:
            X, Y = np.meshgrid(wx0 + np.arange(j1 - j0) * Rs, wy0 + np.arange(i1 - i0) * Rs)
            ok &= ~shapely.contains_xy(keep, X, Y)
        terrain.h[i0:i1, j0:j1] = np.where(ok, np.minimum(win, cut), win).astype(np.float32)
        k = k1


def box_(x0, y0, x1, y1):
    return sbox(x0, y0, x1, y1)


# --------------------------------------------------------------------------------------------
def _dquad(geos, mat, P4, col=(255, 255, 255, 255)):
    P = np.asarray(P4, float)
    n = np.cross(P[1] - P[0], P[3] - P[0])
    ln = np.linalg.norm(n)
    if ln < 1e-9:
        return
    geos.setdefault(mat, DGeo()).add(P, n / ln, None, col, [0, 1, 2, 0, 2, 3])


def _toll_plazas(ctry, terrain, avoid):
    """A toll plaza on each interchange ramp, near its end at the ordinary road (on the ground)."""
    out = []
    for r in ctry.net.roads:
        if r.kind != "ramp" or not r.name.endswith("IC"):
            continue
        L_ = r.line.length
        near = avoid.buffer(25.0) if avoid is not None and not avoid.is_empty else None
        for s_ in np.arange(max(L_ - 110.0, L_ * 0.4), L_ * 0.25, -15.0):
            q = r.line.interpolate(float(s_))
            if near is not None and near.contains(q):
                continue
            a = r.line.interpolate(max(0.0, float(s_) - 4.0))
            b = r.line.interpolate(min(L_, float(s_) + 4.0))
            hd = math.degrees(math.atan2(b.x - a.x, b.y - a.y)) % 360.0  # along the ramp, away from the expressway
            out.append(dict(name=r.name, x=float(q.x), y=float(q.y), z=float(terrain.sample(q.x, q.y)), hd=hd, width=float(r.width)))
            break
        else:
            # a ramp on a viaduct all the way (the port city's): an ETC gantry over its deck instead
            s_ = max(L_ - 150.0, L_ * 0.5)
            q = r.line.interpolate(s_)
            deck = None
            for rs in getattr(ctry, "road_structs", []) or []:
                if rs.kind != "bridge":
                    continue
                d2 = (rs.pts[:, 0] - q.x) ** 2 + (rs.pts[:, 1] - q.y) ** 2
                i = int(np.argmin(d2))
                if d2[i] < (rs.width / 2 + 1.0) ** 2 and (deck is None or d2[i] < deck[0]):
                    deck = (float(d2[i]), float(rs.pts[i, 2]))
            if deck is None:
                continue
            a = r.line.interpolate(max(0.0, s_ - 4.0))
            b = r.line.interpolate(min(L_, s_ + 4.0))
            hd = math.degrees(math.atan2(b.x - a.x, b.y - a.y)) % 360.0
            out.append(dict(name=r.name, x=float(q.x), y=float(q.y), z=deck[1], hd=hd, width=float(r.width), elevated=True))
    return out


def _parking_areas(ctry, terrain):
    """The parking areas' shop building: behind the car park, its glazed front towards it."""
    out = []
    ex = next((r for r in ctry.net.roads if r.kind == "expressway"), None)
    if ex is None:
        return out
    for r in ctry.net.roads:
        if r.kind != "lot":
            continue
        m = r.line.interpolate(0.5, normalized=True)
        a = r.line.interpolate(0.45, normalized=True)
        b = r.line.interpolate(0.55, normalized=True)
        d = np.array([b.x - a.x, b.y - a.y])
        d /= max(np.linalg.norm(d), 1e-9)
        q = ex.line.interpolate(ex.line.project(m))
        n_out = np.array([m.x - q.x, m.y - q.y])
        n_out /= max(np.linalg.norm(n_out), 1e-9)
        c = np.array([m.x, m.y]) + n_out * (r.width / 2 + 3.0 + 6.0)
        ring = np.array([c - d * 13 - n_out * 6, c + d * 13 - n_out * 6, c + d * 13 + n_out * 6, c - d * 13 + n_out * 6])
        area2 = sum(ring[i][0] * ring[(i + 1) % 4][1] - ring[(i + 1) % 4][0] * ring[i][1] for i in range(4))
        if area2 < 0:
            ring = ring[::-1].copy()
        fe = 0
        for i in range(4):  # the edge facing the car park (outward normal of a CCW ring: (dy, -dx))
            e = ring[(i + 1) % 4] - ring[i]
            nrm = np.array([e[1], -e[0]]) / max(np.linalg.norm(e), 1e-9)
            if float(nrm @ -n_out) > 0.9:
                fe = i
        # the fuel station on the car park's outer bays at one end (pumps under a canopy)
        fq = r.line.interpolate(0.84, normalized=True)
        fuel = np.array([fq.x, fq.y]) + n_out * (r.width / 2 - 4.0)
        out.append(dict(name=r.name, ring=ring, ground=float(terrain.sample(c[0], c[1])), front_edge=fe, kind="konbini",
                        fascia=(40, 120, 190), seed=int(abs(c[0] * 7 + c[1] * 3)) % (1 << 30), centre=c, d=d, lot=r.line,
                        n_out=n_out, fuel=fuel, fuel_z=float(terrain.sample(fuel[0], fuel[1]))))
    return out


def pick_fuel_lots(ctry, terrain):
    """A fuel station in each town (generic, no brand): a 26 x 24 m lot beside a main road (8 m or
    wider) 250 m to 1.2 km from the town's centre, clear of other roads, on building land (the
    parcels there get no building) and not too steep; no two within 1.5 km."""
    from shapely.strtree import STRtree
    from . import nation as N
    allr = [r for r in ctry.net.roads if r.kind != "lot"]
    atree = STRtree([r.line for r in allr])
    ptree = STRtree([pc.poly for pc in ctry.parcels])
    out = []
    towns = list(N.CITIES) + [(n_, "", s_, p_) for n_, s_, p_ in L.DISTRICTS if n_ in ("古市", "西ヶ丘", "臨海")]  # (and the capital's)
    for name, _en, style, poly in towns:
        if style in ("center", "airport"):
            continue
        P = Polygon(poly)
        cc = np.array([P.centroid.x, P.centroid.y])
        cands = []
        for k in atree.query(P, predicate="intersects"):
            r = allr[int(k)]
            if r.width < 8.0 or r.kind in ("expressway", "ramp"):
                continue
            for s_ in np.arange(20.0, r.line.length - 20.0, 40.0):
                q = r.line.interpolate(float(s_))
                dist = float(np.hypot(q.x - cc[0], q.y - cc[1]))
                if 250.0 <= dist <= 1200.0 and P.contains(q):
                    cands.append((dist, r, float(s_)))
        cands.sort(key=lambda t: t[0])
        for dist, r, s_ in cands:
            q0, q1 = r.line.interpolate(max(0.0, s_ - 1.0)), r.line.interpolate(min(r.line.length, s_ + 1.0))
            t = np.array([q1.x - q0.x, q1.y - q0.y])
            t /= max(np.linalg.norm(t), 1e-9)
            q = np.array([r.line.interpolate(s_).x, r.line.interpolate(s_).y])
            ok = None
            for sg in (1.0, -1.0):
                n = np.array([-t[1], t[0]]) * sg
                c = q + n * (r.width / 2 + 3.0 + 12.0)
                rect = Polygon([c - t * 13 - n * 12, c + t * 13 - n * 12, c + t * 13 + n * 12, c - t * 13 + n * 12])
                if any(math.hypot(c[0] - o["centre"][0], c[1] - o["centre"][1]) < 1500.0 for o in out):
                    break
                clash = False
                for k in atree.query(rect.buffer(8.0)):
                    o_ = allr[int(k)]
                    if o_.line.distance(rect) < o_.width / 2 + (1.0 if o_ is r else 1.5) and not (o_ is r and o_.line.distance(rect) >= o_.width / 2 + 1.0):
                        clash = True
                        break
                if clash:
                    continue
                pcs = [ctry.parcels[int(k)] for k in ptree.query(rect, predicate="intersects")]
                cover = sum(pc.poly.intersection(rect).area for pc in pcs)
                if cover < 0.75 * rect.area:
                    continue
                zs = terrain.sample(np.array([p_[0] for p_ in rect.exterior.coords]), np.array([p_[1] for p_ in rect.exterior.coords]))
                if float(np.max(zs) - np.min(zs)) > 2.5:
                    continue
                ok = (c, n, rect, pcs)
                break
            if ok is None:
                continue
            c, n, rect, pcs = ok
            for pc in pcs:
                if pc.poly.intersection(rect).area > 0.25 * pc.poly.area:
                    pc.fuel = True
            n_out = -n
            fuel = c + n_out * 2.0
            out.append(dict(name=name, centre=c, d=t, n_out=n_out, depth=24.0, poly=rect, fuel=fuel,
                            fuel_z=float(terrain.sample(fuel[0], fuel[1]))))
            break
    return out


def _town_fuel(geos, ex, xf, lot, ground_c):
    """A roadside fuel station (generic): a concrete apron over the lot, a canopy on four posts over
    two pump islands, a kiosk at the back with a lit window, a blank price board on a pole by the
    road. Colours are generic (no brand)."""
    fu, d, n_out = lot["fuel"], lot["d"], lot["n_out"]
    fz = ground_c(*fu)[2]
    zc = xf.p([[fu[0], fu[1], 0.0]])[0][2]  # (cell-frame height of country z = 0 here)
    fzc = fz - zc  # the ground at the pumps, country frame
    # apron: the lot draped on the ground in 4 m squares (drawn just above it)
    inner = lot["poly"].buffer(-0.4, join_style=2)
    x0, y0, x1, y1 = inner.bounds
    for x in np.arange(x0, x1, 4.0):
        for y in np.arange(y0, y1, 4.0):
            sq = sbox(x, y, x + 4.0, y + 4.0).intersection(inner)
            for part in parts(sq):
                ring = np.asarray(orient(part, 1.0).exterior.coords)[:-1]
                if len(ring) < 3:
                    continue
                V = []
                for q in ring:
                    A = xf.p([[q[0], q[1], 0.0]])[0]
                    A[2] = ground_c(q[0], q[1])[2] + 0.04
                    V.append(A)
                for k in range(1, len(V) - 1):
                    _dquad(geos, "concrete", [V[0], V[k], V[k + 1], V[k + 1]], (178, 178, 172, 255))
    # pump islands and the canopy
    for sa in (-2.6, 2.6):
        p = fu + n_out * sa
        _obox_c(geos, xf, "concrete", p, d, fzc + 0.12, 2.2, 0.55, 0.12, (196, 194, 188, 255))
        for sb in (-0.9, 0.9):
            _obox_c(geos, xf, "metal", p + d * sb, d, fzc + 0.95, 0.3, 0.32, 0.72, (238, 238, 234, 255))
            _obox_c(geos, xf, "sign", p + d * sb, d, fzc + 1.62, 0.31, 0.33, 0.06, (200, 50, 40, 255))
            _obox_c(geos, xf, "lamp", p + d * sb + n_out * 0.33, d, fzc + 1.2, 0.2, 0.005, 0.12, (220, 235, 240, 255))
        A = xf.p([[p[0] - d[0] * 2.2, p[1] - d[1] * 2.2, fzc]])[0]
        B = xf.p([[p[0] + d[0] * 2.2, p[1] + d[1] * 2.2, fzc]])[0]
        ex.walls.append((A[0], A[1], B[0], B[1], A[2] - 0.3, A[2] + 1.7))
    for sa in (-1, 1):
        for sc in (-1, 1):
            q = fu + d * sa * 3.4 + n_out * sc * 2.6
            _obox_c(geos, xf, "metal", q, d, fzc + 2.7, 0.16, 0.16, 2.7, (220, 220, 216, 255))
    _obox_c(geos, xf, "concrete", fu, d, fzc + 5.5, 7.0, 6.0, 0.3, (236, 236, 232, 255))
    for sc in (-1, 1):
        _obox_c(geos, xf, "sign", fu + n_out * sc * 6.02, d, fzc + 5.5, 7.02, 0.03, 0.31, (200, 50, 40, 255))
        _obox_c(geos, xf, "sign", fu + d * sc * 7.02, d, fzc + 5.5, 0.03, 6.02, 0.31, (200, 50, 40, 255))
    _obox_c(geos, xf, "lamp", fu, d, fzc + 5.18, 5.0, 4.0, 0.01, (250, 250, 244, 255))
    L0 = xf.p([[fu[0], fu[1], fzc + 4.6]])[0]
    ex.lights.append(((L0[0], L0[1], L0[2]), 16.0, 3))
    # kiosk at the back of the lot
    kc = lot["centre"] - n_out * (lot["depth"] / 2 - 4.0)
    kz = ground_c(*kc)[2] - zc
    _obox_c(geos, xf, "concrete", kc, d, kz + 1.5, 4.0, 2.6, 1.8, (230, 228, 222, 255))
    _obox_c(geos, xf, "lamp", kc + n_out * 2.61, d, kz + 1.5, 2.6, 0.01, 0.7, (240, 238, 220, 255))
    _obox_c(geos, xf, "sign", kc + n_out * 2.62, d, kz + 2.9, 4.02, 0.02, 0.25, (200, 50, 40, 255))
    for (a1, c1), (a2, c2) in (((-4, -2.6), (4, -2.6)), ((4, -2.6), (4, 2.6)), ((4, 2.6), (-4, 2.6)), ((-4, 2.6), (-4, -2.6))):
        A = xf.p([[kc[0] + d[0] * a1 + n_out[0] * c1, kc[1] + d[1] * a1 + n_out[1] * c1, kz]])[0]
        B = xf.p([[kc[0] + d[0] * a2 + n_out[0] * c2, kc[1] + d[1] * a2 + n_out[1] * c2, kz]])[0]
        ex.walls.append((A[0], A[1], B[0], B[1], A[2] - 0.3, A[2] + 3.3))
    # the price board by the road (blank panels: no prices or names)
    pb = lot["centre"] + n_out * (lot["depth"] / 2 - 1.0) + d * 9.0
    pz = ground_c(*pb)[2] - zc
    _obox_c(geos, xf, "metal", pb, d, pz + 2.2, 0.1, 0.1, 2.2, (200, 200, 196, 255))
    _obox_c(geos, xf, "sign", pb, d, pz + 4.6, 0.9, 0.08, 0.9, (240, 240, 236, 255))
    _obox_c(geos, xf, "sign", pb + n_out * 0.09, d, pz + 5.2, 0.85, 0.01, 0.22, (200, 50, 40, 255))
    for k in range(3):
        _obox_c(geos, xf, "metal_dark", pb + n_out * 0.09, d, pz + 4.7 - k * 0.42, 0.75, 0.01, 0.14, (30, 30, 34, 255))


def _obox_c(geos, xf, mat, p, d, zc, ha, hc, hz, col):
    """A box in the country frame: centre p (2D) at height zc, half sizes along d, across it, up."""
    d = np.asarray(d, float)
    n = np.array([d[1], -d[0]])
    C = []
    for sz in (-1, 1):
        for sc in (-1, 1):
            for sa in (-1, 1):
                q = np.asarray(p, float) + d * sa * ha + n * sc * hc
                C.append([q[0], q[1], zc + sz * hz])
    V = xf.p(np.array(C))
    for f in ((0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)):
        _dquad(geos, mat, [V[f[0]], V[f[1]], V[f[2]], V[f[3]]], col)


def _toll_plaza(geos, ex, xf, t):
    """Booths on islands between the lanes under a canopy, ETC posts at the entries (generic); on a
    viaduct a gantry with the ETC readers over the deck."""
    th = math.radians(t["hd"])
    d = np.array([math.sin(th), math.cos(th)])
    n = np.array([d[1], -d[0]])
    p = np.array([t["x"], t["y"]])
    g = t["z"]
    if t.get("elevated"):
        post = t["width"] / 2 + 0.3
        for sc in (-1, 1):
            _obox_c(geos, xf, "metal", p + n * sc * post, d, g + 2.9, 0.18, 0.18, 2.9, (200, 204, 208, 255))
        _obox_c(geos, xf, "metal", p, d, g + 5.75, 0.25, post + 0.2, 0.2, (200, 204, 208, 255))           # beam
        _obox_c(geos, xf, "sign", p - d * 0.3, d, g + 6.45, 0.05, 2.6, 0.45, (40, 140, 80, 255))          # green panel
        _obox_c(geos, xf, "sign", p - d * 0.3, d, g + 5.3, 0.05, 1.2, 0.18, (235, 190, 30, 255))          # ETC band
        for sc in (-0.45, 0.45):                                                                          # readers
            _obox_c(geos, xf, "metal", p + n * sc * post - d * 0.2, d, g + 5.35, 0.2, 0.25, 0.18, (70, 74, 80, 255))
        L0 = xf.p([[p[0], p[1], g + 5.4]])[0]
        ex.lights.append(((L0[0], L0[1], L0[2]), 12.0, 0))
        return
    edge = t["width"] / 2 + 0.7
    for across in (-edge, 0.0, edge):
        c = p + n * across
        _obox_c(geos, xf, "concrete", c, d, g + 0.13, 4.5, 0.6, 0.13, (196, 196, 190, 255))       # island
        _obox_c(geos, xf, "metal", c, d, g + 1.35, 1.0, 0.5, 1.1, (228, 230, 232, 255))           # booth
        _obox_c(geos, xf, "sign", c, d, g + 2.5, 1.02, 0.52, 0.06, (40, 140, 80, 255))           # booth roof band
        _obox_c(geos, xf, "sign", c - d * 4.1, d, g + 0.75, 0.07, 0.07, 0.5, (235, 190, 30, 255))  # ETC post
        A = xf.p([[c[0] - d[0] * 4.5, c[1] - d[1] * 4.5, g - 0.3]])[0]
        B = xf.p([[c[0] + d[0] * 4.5, c[1] + d[1] * 4.5, g - 0.3]])[0]
        ex.walls.append((A[0], A[1], B[0], B[1], A[2], A[2] + 1.9))
    for sa in (-1, 1):  # canopy on four columns
        for sc in (-1, 1):
            _obox_c(geos, xf, "concrete", p + d * sa * 5.0 + n * sc * edge, d, g + 3.1, 0.25, 0.25, 3.1, (200, 200, 196, 255))
    _obox_c(geos, xf, "concrete", p, d, g + 6.55, 6.0, edge + 2.0, 0.45, (214, 214, 210, 255))
    for sa in (-1, 1):  # the green fascia bands along its front and back
        _obox_c(geos, xf, "sign", p + d * sa * 6.02, d, g + 6.7, 0.04, edge + 2.02, 0.3, (40, 140, 80, 255))
    L0 = xf.p([[p[0], p[1], g + 6.0]])[0]
    ex.lights.append(((L0[0], L0[1], L0[2]), 14.0, 0))


def _parking_area(geos, ex, xf, pa, ground_c):
    """The parking area's shop (a walk-in convenience store) with a flat roof, and bay markings."""
    from . import shop as shop_mod
    till = shop_mod.build(geos, ex, xf, pa)
    if till:
        ex.shops.append((till["kind"], till["counter"], till["stand"], till["z"]))
    c, d = pa["centre"], pa["d"]
    ring = pa["ring"]
    n = np.array([d[1], -d[0]])
    _obox_c(geos, xf, "concrete", c, d, pa["ground"] + shop_mod.FLOOR_H + 0.2, 13.3, 6.3, 0.2, (206, 204, 198, 255))
    # the walls from the ground floor's top to the roof edge are the shop's; a parapet on the roof
    for i in range(4):
        a, b = ring[i], ring[(i + 1) % 4]
        mid = (a + b) / 2
        e = b - a
        L_ = float(np.linalg.norm(e))
        _obox_c(geos, xf, "concrete", mid, e / max(L_, 1e-9), pa["ground"] + shop_mod.FLOOR_H + 0.7, L_ / 2, 0.12, 0.3, (190, 188, 182, 255))
    # the toilets beside the shop (a plain block: pale tiles, a dark band, doorways), vending machines
    # along the shop front, and the fuel station on the outer bays (pumps, canopy, a green band)
    n_out = pa["n_out"]
    gz = pa["ground"]
    wc = c + d * 22.0
    _obox_c(geos, xf, "concrete", wc, d, gz + 1.6, 5.5, 4.0, 1.6, (214, 210, 200, 255))
    _obox_c(geos, xf, "concrete", wc, d, gz + 3.35, 5.8, 4.3, 0.15, (150, 146, 140, 255))
    _obox_c(geos, xf, "sign", wc - n_out * 4.02, d, gz + 2.7, 5.52, 0.03, 0.18, (60, 90, 150, 255))
    for sa in (-2.6, 2.6):
        _obox_c(geos, xf, "metal_dark", wc - n_out * 4.03 + d * sa, d, gz + 1.05, 0.55, 0.03, 1.05, (40, 42, 46, 255))
    A = xf.p([[wc[0] - d[0] * 5.5 - n_out[0] * 4.0, wc[1] - d[1] * 5.5 - n_out[1] * 4.0, gz]])[0]
    B = xf.p([[wc[0] + d[0] * 5.5 - n_out[0] * 4.0, wc[1] + d[1] * 5.5 - n_out[1] * 4.0, gz]])[0]
    ex.walls.append((A[0], A[1], B[0], B[1], A[2] - 0.3, A[2] + 3.2))
    for k in range(3):
        vp = c - n_out * 6.9 + d * (9.5 + k * 1.1)
        _obox_c(geos, xf, "metal", vp, d, gz + 0.92, 0.5, 0.36, 0.92, [(210, 40, 40, 255), (40, 110, 190, 255), (240, 240, 236, 255)][k])
        _obox_c(geos, xf, "lamp", vp - n_out * 0.37, d, gz + 1.2, 0.34, 0.01, 0.5, (235, 240, 245, 255))
    fu, fz = pa["fuel"], pa["fuel_z"]
    for sa in (-2.4, 2.4):
        _obox_c(geos, xf, "concrete", fu + d * sa, d, fz + 0.1, 1.6, 0.5, 0.1, (190, 188, 182, 255))
        _obox_c(geos, xf, "metal", fu + d * sa, d, fz + 0.85, 0.35, 0.3, 0.75, (236, 236, 232, 255))
        _obox_c(geos, xf, "sign", fu + d * sa, d, fz + 1.5, 0.36, 0.31, 0.08, (40, 140, 80, 255))
        A = xf.p([[fu[0] + d[0] * (sa - 1.6), fu[1] + d[1] * (sa - 1.6), fz]])[0]
        B = xf.p([[fu[0] + d[0] * (sa + 1.6), fu[1] + d[1] * (sa + 1.6), fz]])[0]
        ex.walls.append((A[0], A[1], B[0], B[1], A[2] - 0.3, A[2] + 1.6))
    for sa in (-1, 1):
        for sc in (-1, 1):
            _obox_c(geos, xf, "metal", fu + d * sa * 4.2 + n_out * sc * 3.0, d, fz + 2.6, 0.15, 0.15, 2.6, (210, 212, 214, 255))
    _obox_c(geos, xf, "concrete", fu, d, fz + 5.35, 5.0, 4.0, 0.25, (228, 228, 224, 255))
    for sa in (-1, 1):
        _obox_c(geos, xf, "sign", fu + n_out * sa * 4.02, d, fz + 5.35, 5.02, 0.03, 0.26, (40, 140, 80, 255))
    L0 = xf.p([[fu[0], fu[1], fz + 4.8]])[0]
    ex.lights.append(((L0[0], L0[1], L0[2]), 12.0, 3))
    # parking bays: white lines across both sides of the car park
    lot = pa["lot"]
    for s_ in np.arange(6.0, lot.length - 6.0, 2.5):
        q = lot.interpolate(float(s_))
        if math.hypot(q.x - fu[0], q.y - fu[1]) < 12.0:  # (no bays at the fuel station)
            continue
        a = lot.interpolate(max(0.0, float(s_) - 1.0))
        b = lot.interpolate(min(lot.length, float(s_) + 1.0))
        dd = np.array([b.x - a.x, b.y - a.y])
        dd /= max(np.linalg.norm(dd), 1e-9)
        nn = np.array([dd[1], -dd[0]])
        for side in (-1, 1):
            m0 = np.array([q.x, q.y]) + nn * side * 5.5
            m1 = np.array([q.x, q.y]) + nn * side * 10.5
            pts = [m0 - dd * 0.06, m0 + dd * 0.06, m1 + dd * 0.06, m1 - dd * 0.06]
            P = []
            for q2 in pts:  # draped on the ground there
                A = xf.p([[q2[0], q2[1], 0.0]])[0]
                A[2] = ground_c(q2[0], q2[1])[2] + 0.03
                P.append(A)
            _dquad(geos, "marking", P, (235, 235, 230, 255))


def cell_detail(spec: Spec, isl, cpoly: Polygon, xf, ts, rng, geos, rp=None) -> CellExtra:
    ex = CellExtra()
    clip = cpoly.buffer(2)

    def ground_c(x, y):
        c = xf.p([[x, y, 0.0]])[0]
        return c[0], c[1], float(ts(c[0], c[1]))

    # --- trees ---
    for x, y, h, r, k in spec.trees:
        if clip.contains(Point(x, y)):
            cx, cy, cz = ground_c(x, y)
            ex.trees.append(((cx, cy, cz), h, r, k))
    # --- river water surfaces ---
    for rv in isl.rivers:
        if not rv.poly.intersects(clip):
            continue
        P = np.asarray(rv.center.coords)
        W, Z = rv.widths, rv.water
        for i in range(len(P) - 1):
            seg = LineString(P[i:i + 2])
            if not seg.intersects(clip):
                continue
            a, b = P[i], P[i + 1]
            d = (b - a) / max(np.linalg.norm(b - a), 1e-9)
            nrm = np.array([-d[1], d[0]])
            wa, wb = W[i] / 2 - 1, W[i + 1] / 2 - 1
            q = [(a - nrm * wa, Z[i]), (b - nrm * wb, Z[i + 1]), (b + nrm * wb, Z[i + 1]), (a + nrm * wa, Z[i])]
            _dquad(geos, "water", xf.p([[p[0], p[1], z] for p, z in q]))
    # --- lake surfaces (triangulated polygon at the lake level) ---
    for name, lvl, pp in L.LAKES:
        lp = Polygon(pp)
        if not lp.intersects(clip):
            continue
        lvl = getattr(isl.terrain, "lake_levels", {}).get(name, lvl)
        part = lp.intersection(clip)
        for pg in parts(part):
            gg = Geo()
            cap(gg, pg, lvl, 0, (255, 255, 255))
            arr = gg.arrays()
            if arr is None:
                continue
            pos, nrm_, col_, uv_, idx_ = arr
            V = xf.p(pos)
            geos.setdefault("water", DGeo()).add(V, xf.n(nrm_), None, (255, 255, 255, 255), idx_)
    # --- seawalls (vertical concrete + coping) ---
    for ql in spec.quays:
        if not ql.intersects(clip):
            continue
        g = ql.intersection(clip)
        for part in getattr(g, "geoms", [g]):
            if not isinstance(part, LineString):
                continue
            c = np.asarray(part.coords)
            for i in range(len(c) - 1):
                a, b = c[i], c[i + 1]
                L_ = np.linalg.norm(b - a)
                if L_ < 0.5:
                    continue
                n_ = int(math.ceil(L_ / 6))
                for s in range(n_):
                    p0 = a + (b - a) * s / n_
                    p1 = a + (b - a) * (s + 1) / n_
                    d = (p1 - p0) / max(np.linalg.norm(p1 - p0), 1e-9)
                    inward = np.array([-d[1], d[0]])  # land is left of a CCW coast
                    z0 = ground_c(*(p0 + inward * 2.5))
                    z1 = ground_c(*(p1 + inward * 2.5))
                    A, B = xf.p([[p0[0], p0[1], 0]])[0], xf.p([[p1[0], p1[1], 0]])[0]
                    top0, top1 = z0[2] + 0.1, z1[2] + 0.1
                    _dquad(geos, "concrete", [(A[0], A[1], -6.0 + A[2]), (B[0], B[1], -6.0 + B[2]), (B[0], B[1], top1), (A[0], A[1], top0)])
    # --- bridges ---
    for deck, line, z0, z1, arch, width in spec.bridges:
        if not deck.intersects(clip):
            continue
        c = np.asarray(line.coords)
        Lb = line.length
        steps = max(2, int(Lb / 6))
        prof = []
        for s in np.linspace(0, 1, steps + 1):
            p = np.asarray(line.interpolate(s, normalized=True).coords[0])
            z = z0 + (z1 - z0) * s
            if arch:
                z += 22 * math.sin(math.pi * s) ** 0.8
            prof.append((p, z))
        for i in range(len(prof) - 1):
            (pa, za), (pb, zb) = prof[i], prof[i + 1]
            if not LineString([pa, pb]).intersects(clip):
                continue
            d = (pb - pa) / max(np.linalg.norm(pb - pa), 1e-9)
            nrm = np.array([-d[1], d[0]]) * width / 2
            q = [(pa - nrm, za), (pb - nrm, zb), (pb + nrm, zb), (pa + nrm, za)]
            V = xf.p([[p[0], p[1], z] for p, z in q])
            _dquad(geos, "asphalt", V)
            ex.decks.append(np.array([[V[0], V[1], V[2]], [V[0], V[2], V[3]]]))
            for sgn, k0, k1 in ((-1, 0, 1), (1, 3, 2)):  # deck edge (girder face) + railing
                A, B = V[k0], V[k1]
                _dquad(geos, "concrete", [A - [0, 0, 1.6], B - [0, 0, 1.6], B, A] if sgn < 0 else [B - [0, 0, 1.6], A - [0, 0, 1.6], A, B])
                _dquad(geos, "fence", [A, B, B + [0, 0, 1.1], A + [0, 0, 1.1]] if sgn < 0 else [B, A, A + [0, 0, 1.1], B + [0, 0, 1.1]])
            _dquad(geos, "concrete", [V[1] - [0, 0, 1.6], V[0] - [0, 0, 1.6], V[3] - [0, 0, 1.6], V[2] - [0, 0, 1.6]])
            if arch and i % 12 == 6:  # towers + cables of the bay bridge
                for sgn in (-1, 1):
                    base = (pa + pb) / 2 + np.array([-d[1], d[0]]) * sgn * (width / 2 + 1)
                    B0 = xf.p([[base[0], base[1], za - 12]])[0]
                    _box_d(geos, "concrete", B0 + [0, 0, 30], (1.2, 1.2, 30))
    # --- road tunnels and viaducts (country roads through ridges / across valleys) ---
    for rs in spec.road_structs:
        _road_structure(geos, ex, xf, rs, clip, ground_c)
    # --- toll plazas and parking areas ---
    for t in spec.tolls:
        if cpoly.contains(Point(t["x"], t["y"])):
            _toll_plaza(geos, ex, xf, t)
    for pa in spec.pas:
        if cpoly.contains(Point(float(pa["centre"][0]), float(pa["centre"][1]))):
            _parking_area(geos, ex, xf, pa, ground_c)
    for lot in spec.fuel_lots:
        if cpoly.contains(Point(float(lot["centre"][0]), float(lot["centre"][1]))):
            _town_fuel(geos, ex, xf, lot, ground_c)
    # --- the expressway on the ground: median barrier and guard rails (collision walls too), open
    # where an interchange's slip road leaves ---
    slips = [q.line.buffer(q.carriage / 2.0 + 1.5) for q in isl.net.roads if q.kind == "ramp" and q.line.intersects(clip.buffer(300))]
    slip_area = unary_union(slips) if slips else None
    for r in isl.net.roads:
        if r.kind != "expressway" or not r.line.intersects(clip.buffer(20)):
            continue
        c = np.asarray(r.line.coords)
        hw_ = r.width / 2.0
        for i in range(len(c) - 1):
            a, b = c[i], c[i + 1]
            mid = (a + b) / 2
            if not clip.contains(Point(mid)):
                continue
            if spec.road_struct_area is not None and spec.road_struct_area.contains(Point(mid)):
                continue  # (on a structure: its own barriers)
            d2 = b - a
            Ls = float(np.linalg.norm(d2))
            if Ls < 1e-3:
                continue
            nrm = np.array([-d2[1], d2[0]]) / Ls
            for off, h, mat, col in ((0.0, 0.85, "concrete", (200, 200, 196, 255)), (-(hw_ - 0.3), 0.75, "metal", (210, 212, 216, 255)),
                                     (hw_ - 0.3, 0.75, "metal", (210, 212, 216, 255))):
                pa, pb = a + nrm * off, b + nrm * off
                if off != 0.0 and slip_area is not None and slip_area.contains(Point((pa + pb) / 2)):
                    continue
                za, zb = ground_c(*pa)[2], ground_c(*pb)[2]
                A = xf.p([[pa[0], pa[1], 0.0]])[0]
                B = xf.p([[pb[0], pb[1], 0.0]])[0]
                A[2], B[2] = za, zb
                if off == 0.0:
                    _dquad(geos, mat, [A, B, B + [0, 0, h], A + [0, 0, h]], col)
                else:  # guard rail: a beam on posts
                    _dquad(geos, mat, [A + [0, 0, h - 0.3], B + [0, 0, h - 0.3], B + [0, 0, h], A + [0, 0, h]], col)
                    _box_d(geos, "metal", A + [0, 0, h / 2], (0.05, 0.05, h / 2), (150, 152, 156, 255))
                ex.walls.append((A[0], A[1], B[0], B[1], min(za, zb) - 0.2, max(za, zb) + h))
    import shapely
    carr_p = (rp if rp is not None else isl.net.polys(clip)).carriage.intersection(clip.buffer(50))
    shapely.prepare(carr_p)
    # --- station platforms (walkable) with canopies ---
    from . import station as S
    for pi_, (x, y, hd, length, width, ztop, line_i, _name, path) in enumerate(spec.platforms):
        if not cpoly.contains(Point(x, y)):  # a station's platforms and concourse go in the cell of its centre
            continue
        st = spec.stations[pi_]
        kind_i = 2 if L.RAIL_LINES[line_i]["kind"] == "shinkansen" else 0
        th = math.radians(hd)
        fwd = np.array([math.sin(th), math.cos(th)])
        side = np.array([math.cos(th), -math.sin(th)])
        # platform edge 1.6 m (commuter) / 1.76 m (Shinkansen) from the track centre (tracks at 2.5 / 3.15 m)
        off = 6.6 if kind_i < 2 else 7.4
        # the centre line under the platform, with the right-hand normal at each point (curved platforms follow it)
        C = np.asarray(path, float)
        # resampled finely so the platform bands can leave the stair openings out
        cum0 = np.concatenate([[0.0], np.cumsum(np.hypot(*np.diff(C, axis=0).T))])
        tt = np.arange(0.0, cum0[-1] + 1e-6, 0.25)
        C = np.column_stack([np.interp(tt, cum0, C[:, 0]), np.interp(tt, cum0, C[:, 1])])
        T = np.gradient(C, axis=0)
        T /= np.maximum(np.linalg.norm(T, axis=1, keepdims=True), 1e-9)
        if float(T[len(T) // 2] @ fwd) < 0:  # orient along the station heading
            C, T = C[::-1], -T[::-1]
        Nr = np.column_stack([T[:, 1], -T[:, 0]])  # right of the heading
        cum = np.concatenate([[0.0], np.cumsum(np.hypot(*np.diff(C, axis=0).T))])
        u = cum - cum[len(cum) // 2]  # distance along the platform from the station centre

        def strip(o0, o1, z, mat, lo=-1e9, hi=1e9, walk=False, col=None):
            """A band from offset o0 to o1 (right of the centre line) between u = lo and hi (2 m pieces,
            ends at the 0.25 m resolution of the centre line)."""
            sel = np.nonzero((u[1:] > lo) & (u[:-1] < hi))[0]
            if len(sel) == 0:
                return
            a0, a1 = int(sel[0]), int(sel[-1]) + 1
            marks = list(range(a0, a1, 8)) + [a1]
            for a, b in zip(marks[:-1], marks[1:]):
                pa, pb = C[a], C[b]
                q = [pa + Nr[a] * o0, pb + Nr[b] * o0, pb + Nr[b] * o1, pa + Nr[a] * o1]
                V = xf.p([[p[0], p[1], z] for p in q])
                if col is None:
                    _dquad(geos, mat, V if o1 > o0 else V[::-1])
                else:
                    _dquad(geos, mat, V if o1 > o0 else V[::-1], col)
                if walk:
                    ex.decks.append(np.array([[V[0], V[1], V[2]], [V[0], V[2], V[3]]]))
        S.build(geos, ex, xf, st, isl.terrain)
        for sg in (-1, 1):
            o_in, o_out = sg * (off - width / 2), sg * (off + width / 2)
            S.platform_items(geos, ex, xf, st, C, Nr, u, sg, o_in, o_out, length,
                             lambda o0, o1, z, mat, lo, hi, walk: strip(o0, o1, z, mat, lo, hi, walk=walk))
            # platform faces (1.1 m down to the track bed) along both edges and across the ends, facing out
            for o in (o_in, o_out):
                out = 1.0 if o > (o_in + o_out) / 2 else -1.0  # +: the face looks to the right of the heading
                for a in range(len(C) - 1):
                    A = xf.p([[*(C[a] + Nr[a] * o), ztop]])[0]
                    B = xf.p([[*(C[a + 1] + Nr[a + 1] * o), ztop]])[0]
                    Q = [A - [0, 0, 1.1], B - [0, 0, 1.1], B, A]
                    _dquad(geos, "concrete", Q if out > 0 else [Q[1], Q[0], Q[3], Q[2]])
            for e, out in ((0, -1.0), (len(C) - 1, 1.0)):
                A = xf.p([[*(C[e] + Nr[e] * min(o_in, o_out)), ztop]])[0]
                B = xf.p([[*(C[e] + Nr[e] * max(o_in, o_out)), ztop]])[0]
                Q = [A - [0, 0, 1.1], B - [0, 0, 1.1], B, A]  # (faces back along the heading)
                _dquad(geos, "concrete", Q if out < 0 else [Q[1], Q[0], Q[3], Q[2]])
            # tactile strip along the track edge
            e0 = sg * (off - width / 2 + 0.9)
            strip(min(e0 - 0.3, e0 + 0.3), max(e0 - 0.3, e0 + 0.3), ztop + 0.01, "tactile")
            # canopy over the middle 80 % on columns
            c0, c1 = sg * (off - width / 2 - 0.6), sg * (off + width / 2 + 0.6)
            strip(min(c0, c1), max(c0, c1), ztop + 3.4, "metal", -length * 0.4, length * 0.4)
            strip(max(c0, c1), min(c0, c1), ztop + 3.4, "metal_dark", -length * 0.4, length * 0.4)
            for s_ in np.linspace(-0.38, 0.38, 7):
                a = int(np.argmin(np.abs(u - length * s_)))
                pc = C[a] + Nr[a] * sg * off
                P0 = xf.p([[pc[0], pc[1], ztop]])[0]
                _box_d(geos, "metal", P0 + [0, 0, 1.7], (0.12, 0.12, 1.7))
            if kind_i == 2:
                # platform screen doors (typical of Shinkansen stations): 1.3 m fence 0.5 m back from the
                # edge with openings at the car doors of an 8 x 25 m train stopped at the centre
                # (one door at the rear of each car; the reversed rear cab has it at the front end).
                # Shinkansen stations are on straight track: the fence runs along the heading.
                c = np.array([x, y]) + side * off * sg
                fdir = 1.0 if sg < 0 else -1.0  # trains on the left platform run along the heading
                opens = []
                for k in range(8):
                    uu = (3.5 - k) * 25.0 + (11.4 if k == 7 else -11.4)
                    opens.append(uu * fdir)
                opens.sort()
                edge = c - side * sg * (width / 2 - 0.5)
                uu = -length / 2 + 2.0
                cuts = []
                for o in opens:
                    if o - 1.15 > uu:
                        cuts.append((uu, o - 1.15))
                    uu = o + 1.15
                cuts.append((uu, length / 2 - 2.0))
                for ua, ub in cuts:
                    if ub - ua < 0.3:
                        continue
                    pa, pb = edge + fwd * ua, edge + fwd * ub
                    q2 = [pa, pb]
                    Vp = xf.p([[p_[0], p_[1], ztop] for p_ in q2] + [[p_[0], p_[1], ztop + 1.3] for p_ in q2[::-1]])
                    _dquad(geos, "metal", [Vp[0], Vp[1], Vp[2], Vp[3]], (214, 216, 220, 255))
                    _dquad(geos, "metal", [Vp[1], Vp[0], Vp[3], Vp[2]], (214, 216, 220, 255))
                    for ue in (ua, ub):  # door pockets / posts
                        P0 = xf.p([[*(edge + fwd * ue), ztop]])[0]
                        _box_d(geos, "metal_dark", P0 + [0, 0, 0.67], (0.12, 0.12, 0.67))
    # --- ferry piers (deck on piles) ---
    for x, y, hd, length in spec.piers:
        if not clip.buffer(length).contains(Point(x, y)):
            continue
        th = math.radians(hd)
        fwd = np.array([math.sin(th), math.cos(th)])
        side = np.array([math.cos(th), -math.sin(th)])
        base = np.array([x, y])
        q = [base - side * 6, base + fwd * length - side * 6, base + fwd * length + side * 6, base + side * 6]
        V = xf.p([[p[0], p[1], 2.4] for p in q])
        _dquad(geos, "concrete", V)
        ex.decks.append(np.array([[V[0], V[1], V[2]], [V[0], V[2], V[3]]]))
        for s_ in np.linspace(0.1, 1.0, 8):
            for sg in (-1, 1):
                pp = base + fwd * length * s_ + side * 5 * sg
                P0 = xf.p([[pp[0], pp[1], -2.0]])[0]
                _box_d(geos, "concrete", P0, (0.4, 0.4, 4.4))
    # --- elevated rail: deck, barriers, rails, piers (tunnel tubes where the ground is above) ---
    stations_xy = np.array([(p[0], p[1]) for p in spec.platforms], float)
    for ri, R in enumerate(spec.rails):
        shink = spec.rail_kinds[ri] == "shinkansen" if spec.rail_kinds else False
        tun = spec.tunnel_flags[ri] if spec.tunnel_flags else np.zeros(len(R), bool)
        for i in range(len(R) - 1):
            a, b = R[i], R[i + 1]
            if not LineString([a[:2], b[:2]]).intersects(clip):
                continue
            d2 = b[:2] - a[:2]
            Ls = np.linalg.norm(d2)
            if Ls < 1e-3:
                continue
            d = d2 / Ls
            nrm = np.array([-d[1], d[0]])
            hw = 5.8 if shink else 5.0
            ga = ground_c(*a[:2])
            if tun[i]:
                # tunnel: a concrete tube around the track (seen from the train), a portal at each end
                ring_n = 10
                A0 = xf.p([[a[0], a[1], a[2]]])[0]
                B0 = xf.p([[b[0], b[1], b[2]]])[0]
                nn = np.array([nrm[0], nrm[1], 0.0])
                tw, th_ = (6.0, 7.0) if shink else (5.0, 6.2)
                for k in range(ring_n):
                    t0, t1 = math.pi * k / ring_n, math.pi * (k + 1) / ring_n
                    o0 = nn * math.cos(t0) * tw + np.array([0, 0, math.sin(t0) * th_])
                    o1 = nn * math.cos(t1) * tw + np.array([0, 0, math.sin(t1) * th_])
                    _dquad(geos, "concrete", [A0 + o1, B0 + o1, B0 + o0, A0 + o0])
                _dquad(geos, "ballast", [A0 - nn * tw, B0 - nn * tw, B0 + nn * tw, A0 + nn * tw])
                for off in ((-3.9, -2.4, 2.4, 3.9) if shink else (-3.2, -1.8, 1.8, 3.2)):
                    pa, pb = a[:2] + nrm * off, b[:2] + nrm * off
                    Ar = xf.p([[pa[0], pa[1], a[2] + 0.18]])[0]
                    Br = xf.p([[pb[0], pb[1], b[2] + 0.18]])[0]
                    o = np.array([nrm[0], nrm[1], 0]) * 0.04
                    _dquad(geos, "metal", [Ar - o, Br - o, Br + o, Ar + o])
                for end, P0, outward in ((i > 0 and not tun[i - 1], A0, -1.0), (i + 1 < len(tun) and not tun[i + 1], B0, 1.0)):
                    if end:
                        _portal(geos, P0, nn, np.array([d[0], d[1], 0.0]) * outward, tw, th_)
                continue
            ag = spec.at_grade[ri] if ri < len(spec.at_grade) else None
            at_grade = ag is not None and bool(ag[i]) and bool(ag[i + 1])
            q = [(a[:2] - nrm * hw, a[2]), (b[:2] - nrm * hw, b[2]), (b[:2] + nrm * hw, b[2]), (a[:2] + nrm * hw, a[2])]
            V = xf.p([[p[0], p[1], z] for p, z in q])
            _dquad(geos, "ballast", V)
            if at_grade:
                # on the ground: ballast shoulders down to the formation, a lineside fence (not at the crossings)
                near_x = any(c["line"] == ri and math.hypot(c["x"] - a[0], c["y"] - a[1]) < 14.0 for c in spec.crossings)
                for sg in (-1, 1):
                    oa, ob = a[:2] + nrm * sg * (hw + 1.4), b[:2] + nrm * sg * (hw + 1.4)
                    Wo = xf.p([[oa[0], oa[1], a[2] - 0.55], [ob[0], ob[1], b[2] - 0.55]])
                    Vi = (V[0], V[1]) if sg < 0 else (V[3], V[2])
                    _dquad(geos, "ballast", [Vi[0], Vi[1], Wo[1], Wo[0]])
                    if not near_x:
                        fa, fb = a[:2] + nrm * sg * (hw + 2.6), b[:2] + nrm * sg * (hw + 2.6)
                        Fa, Fb = xf.p([[fa[0], fa[1], a[2] - 0.6], [fb[0], fb[1], b[2] - 0.6]])
                        ga, gb = ground_c(*fa)[2], ground_c(*fb)[2]
                        Fa[2], Fb[2] = min(Fa[2], ga), min(Fb[2], gb)
                        _dquad(geos, "fence", [Fa, Fb, Fb + [0, 0, 1.25], Fa + [0, 0, 1.25]], (150, 156, 150, 255))
                        ex.walls.append((Fa[0], Fa[1], Fb[0], Fb[1], min(Fa[2], Fb[2]) - 0.3, max(Fa[2], Fb[2]) + 1.3))
            else:
                _dquad(geos, "concrete", [V[1] - [0, 0, 1.4], V[0] - [0, 0, 1.4], V[3] - [0, 0, 1.4], V[2] - [0, 0, 1.4]])
                for k0, k1 in ((0, 1), (2, 3)):
                    A, B = V[k0], V[k1]
                    _dquad(geos, "concrete", [A - [0, 0, 1.4], B - [0, 0, 1.4], B + [0, 0, 1.2], A + [0, 0, 1.2]])
            for off in ((-3.9, -2.4, 2.4, 3.9) if shink else (-3.2, -1.8, 1.8, 3.2)):  # four rails (double track)
                pa, pb = a[:2] + nrm * off, b[:2] + nrm * off
                A = xf.p([[pa[0], pa[1], a[2] + 0.18]])[0]
                B = xf.p([[pb[0], pb[1], b[2] + 0.18]])[0]
                o = np.array([nrm[0], nrm[1], 0]) * 0.04
                _dquad(geos, "metal", [A - o, B - o, B + o, A + o])
            _catenary(geos, xf, a, b, d, nrm, i, hw, shink, stations_xy)
            if i % 4 == 0 and not at_grade and not shapely.contains_xy(carr_p, a[0], a[1]) and not _in_concourse(spec.stations, a[0], a[1]):
                # pier every ~24 m, never on a carriageway nor inside a station's concourse
                g = ground_c(*a[:2])
                top = xf.p([[a[0], a[1], a[2] - 1.4]])[0]
                if top[2] - g[2] > 1.2:
                    _box_d(geos, "concrete", np.array([top[0], top[1], (top[2] + g[2]) / 2]), (1.1, 1.1, (top[2] - g[2]) / 2))
    # --- level crossings: road panels over the tracks, warning posts and barrier machines ---
    for c in spec.crossings:
        if clip.distance(Point(c["x"], c["y"])) > 20.0:
            continue
        _level_crossing(geos, ex, xf, c, 5.0)
    # --- forest canopy: built by the client from the cell's land-cover map (see cook_country.py) ---
    if False:
        region = spec.canopy.intersection(clip)
        import shapely
        shapely.prepare(region)
        minx, miny, maxx, maxy = region.bounds
        step = 9.0
        xs = np.arange(minx, maxx + step, step)
        ys = np.arange(miny, maxy + step, step)
        X, Y = np.meshgrid(xs, ys)
        inside = shapely.contains_xy(region, X, Y)
        if inside.any():
            from .generate import fbm
            bump = fbm(X, Y, 40, 3, 91)
            H = np.where(inside, 11 + 9 * bump, 0.0)
            # cell-local positions + terrain
            flat = np.column_stack([X.ravel(), Y.ravel(), np.zeros(X.size)])
            C = xf.p(flat)
            tz = ts(C[:, 0], C[:, 1])
            Zc = (tz + H.ravel()).reshape(X.shape)
            Zg = tz.reshape(X.shape)
            ny, nx = X.shape
            Cx, Cy = C[:, 0].reshape(X.shape), C[:, 1].reshape(X.shape)
            verts, tris = [], []
            vid = -np.ones(X.shape, int)
            for i in range(ny):
                for j in range(nx):
                    if inside[max(0, i - 1):i + 2, max(0, j - 1):j + 2].any():
                        vid[i, j] = len(verts)
                        z = Zc[i, j] if inside[i, j] else Zg[i, j] - 0.5
                        verts.append((Cx[i, j], Cy[i, j], z))
            for i in range(ny - 1):
                for j in range(nx - 1):
                    a_, b_, c_, d_ = vid[i, j], vid[i, j + 1], vid[i + 1, j + 1], vid[i + 1, j]
                    if min(a_, b_, c_, d_) >= 0 and (inside[i, j] or inside[i, j + 1] or inside[i + 1, j] or inside[i + 1, j + 1]):
                        tris += [(a_, b_, c_), (a_, c_, d_)]
            if tris:
                V = np.array(verts)
                T = np.array(tris)
                # vertex normals from faces
                N = np.zeros_like(V)
                fn = np.cross(V[T[:, 1]] - V[T[:, 0]], V[T[:, 2]] - V[T[:, 0]])
                for k in range(3):
                    np.add.at(N, T[:, k], fn)
                N /= np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-9)
                shade = (np.clip(80 + 60 * rng.random(len(V)), 0, 255)).astype(np.uint8)
                col = np.column_stack([shade, shade, shade, np.full(len(V), 255, np.uint8)])
                geos.setdefault("canopy", DGeo()).add(V, N, None, col, T)
    return ex


CONTACT_H = 5.0     # contact wire above the rail (Japanese viaduct lines are typically 5.0 m)
MESSENGER_H = 5.9   # messenger (catenary) wire, droppers not modelled
FEEDER_H = 7.2      # feeder wire carried on the mast tops
MAST_EVERY = 8      # rail polyline points are ~6 m apart: a mast portal about every 50 m


def _wire(geos, A, B, w=0.022, col=(52, 50, 48, 255)):
    """A thin cable from A to B as a crossed pair of ribbons (reads from any side)."""
    A, B = np.asarray(A, float), np.asarray(B, float)
    d = B - A
    ln = np.linalg.norm(d)
    if ln < 1e-6:
        return
    d /= ln
    side = np.cross(d, [0.0, 0.0, 1.0])
    sl = np.linalg.norm(side)
    side = side / sl if sl > 1e-6 else np.array([1.0, 0.0, 0.0])
    up = np.cross(side, d)
    for o in (side * w, up * w):
        _dquad(geos, "metal", [A - o, B - o, B + o, A + o], col)
        _dquad(geos, "metal", [A + o, B + o, B - o, A - o], col)


def _catenary(geos, xf, a, b, d, nrm, i, hw, shink, stations_xy):
    """Overhead line equipment over both tracks of a viaduct segment: contact and messenger
    wires (with the contact wire's zig-zag across the pantograph), feeders on the mast tops, and a
    steel portal (two columns and a beam with drop tubes) about every 50 m, except inside stations
    where the wires hang from the platform canopies. Generic, not a surveyed installation."""
    tc = 3.15 if shink else 2.5
    nn = np.array([nrm[0], nrm[1], 0.0])
    def zig(k):  # contact wire stagger (m) at polyline point k
        span = (k % MAST_EVERY) / MAST_EVERY
        sgn = 1.0 if (k // MAST_EVERY) % 2 == 0 else -1.0
        return 0.2 * sgn * (1.0 - 2.0 * span)
    for sg in (-1.0, 1.0):
        for h, stag in ((CONTACT_H, True), (MESSENGER_H, False)):
            za, zb = (zig(i), zig(i + 1)) if stag else (0.0, 0.0)
            pa = a[:2] + nrm * (sg * tc + za)
            pb = b[:2] + nrm * (sg * tc + zb)
            A = xf.p([[pa[0], pa[1], a[2] + h]])[0]
            B = xf.p([[pb[0], pb[1], b[2] + h]])[0]
            _wire(geos, A, B, 0.012 if stag else 0.014)
    for sg in (-1.0, 1.0):  # feeders along the mast tops
        pa, pb = a[:2] + nrm * sg * (hw - 0.35), b[:2] + nrm * sg * (hw - 0.35)
        _wire(geos, xf.p([[pa[0], pa[1], a[2] + FEEDER_H]])[0], xf.p([[pb[0], pb[1], b[2] + FEEDER_H]])[0], 0.016)
    if i % MAST_EVERY != 0:
        return
    # in a station the portal spans the platforms: columns stand at the back of each platform
    in_station = len(stations_xy) and np.min(np.hypot(stations_xy[:, 0] - a[0], stations_xy[:, 1] - a[1])) < (170.0 if shink else 110.0)
    col_off, col_z0 = ((7.4 if shink else 6.6) + 2.3, 1.1) if in_station else (hw - 0.35, 0.0)
    steel = (150, 154, 158, 255)
    cols = []
    for sg in (-1.0, 1.0):
        pc = a[:2] + nrm * sg * col_off
        P = xf.p([[pc[0], pc[1], a[2]]])[0]
        h = FEEDER_H + 0.3 - col_z0
        _box_d(geos, "metal", P + [0, 0, col_z0 + h / 2], (0.13, 0.13, h / 2))
        cols.append(P)
    beam_z = MESSENGER_H + 0.55
    mid = (cols[0] + cols[1]) / 2 + [0, 0, beam_z]
    half = np.linalg.norm(cols[1][:2] - cols[0][:2]) / 2
    # beam across the tracks: a box along the cross-track direction (top, bottom and both faces)
    ang = math.atan2(nrm[1], nrm[0])
    ex = np.array([math.cos(ang), math.sin(ang), 0.0])
    ey = np.array([-math.sin(ang), math.cos(ang), 0.0])
    ez = np.array([0.0, 0.0, 1.0])
    hx, hy, hz = half, 0.1, 0.16
    def C(sx, sy, sz):
        return mid + ex * sx * hx + ey * sy * hy + ez * sz * hz
    for P4 in ([C(-1, -1, 1), C(1, -1, 1), C(1, 1, 1), C(-1, 1, 1)], [C(-1, 1, -1), C(1, 1, -1), C(1, -1, -1), C(-1, -1, -1)],
               [C(-1, -1, -1), C(1, -1, -1), C(1, -1, 1), C(-1, -1, 1)], [C(1, 1, -1), C(-1, 1, -1), C(-1, 1, 1), C(1, 1, 1)]):
        _dquad(geos, "metal", P4, steel)
    # drop tubes and registration arms over each track
    for sg in (-1.0, 1.0):
        pt = a[:2] + nrm * sg * tc
        top = xf.p([[pt[0], pt[1], a[2] + beam_z - hz]])[0]
        bot = xf.p([[pt[0], pt[1], a[2] + CONTACT_H + 0.25]])[0]
        _wire(geos, top, bot, 0.03, steel)
        arm_end = xf.p([[pt[0] + nrm[0] * zig(i), pt[1] + nrm[1] * zig(i), a[2] + CONTACT_H]])[0]
        _wire(geos, bot, arm_end, 0.02, steel)


def _road_structure(geos, ex, xf, rs, clip, ground_c):
    """A road tunnel (concrete tube with walkways, lamps, a portal at each mouth) or a viaduct (deck
    with parapets on piers). The carriageway is a deck the cars drive on; parapets and tunnel walls
    are collision walls."""
    P = rs.pts
    if len(P) < 2 or not LineString(P[:, :2]).intersects(clip.buffer(10)):
        return
    hw = rs.width / 2.0
    up = np.array([0.0, 0.0, 1.0])
    tunnel = rs.kind == "tunnel"
    wide = hw > 7.0  # the expressway: a median barrier (and a dividing wall in its tunnels)
    tw, th_ = hw + 1.6, (8.2 if wide else 6.8)  # tunnel half width at the springing, crown height
    n = len(P)
    for i in range(n - 1):
        a, b = P[i], P[i + 1]
        if not LineString([a[:2], b[:2]]).intersects(clip):
            continue
        d2 = b[:2] - a[:2]
        Ls = float(np.linalg.norm(d2))
        if Ls < 1e-3:
            continue
        dd = d2 / Ls
        nrm = np.array([-dd[1], dd[0]])
        q = [(a[:2] - nrm * hw, a[2]), (b[:2] - nrm * hw, b[2]), (b[:2] + nrm * hw, b[2]), (a[:2] + nrm * hw, a[2])]
        V = xf.p([[p[0], p[1], z] for p, z in q])
        _dquad(geos, "asphalt", V)
        ex.decks.append(np.array([[V[0], V[1], V[2]], [V[0], V[2], V[3]]]))
        A0 = xf.p([[a[0], a[1], a[2]]])[0]
        B0 = xf.p([[b[0], b[1], b[2]]])[0]
        nn = np.array([nrm[0], nrm[1], 0.0])
        if wide:  # median: a concrete barrier on viaducts, a dividing wall between the tunnel's two roads
            mh = th_ if tunnel else 0.9
            for sg in (-1, 1):
                _dquad(geos, "concrete", [A0 + nn * sg * 0.25, B0 + nn * sg * 0.25, B0 + nn * sg * 0.25 + up * mh, A0 + nn * sg * 0.25 + up * mh],
                       (200, 200, 196, 255))
            _dquad(geos, "concrete", [A0 - nn * 0.25 + up * mh, B0 - nn * 0.25 + up * mh, B0 + nn * 0.25 + up * mh, A0 + nn * 0.25 + up * mh],
                   (190, 190, 186, 255))
            ex.walls.append((A0[0], A0[1], B0[0], B0[1], A0[2] - 0.2, A0[2] + mh))
        if tunnel:
            # tube (seen from inside): walls to the springing, then the vault
            for sgn in (-1, 1):
                Aw, Bw = A0 + nn * sgn * tw, B0 + nn * sgn * tw
                _dquad(geos, "concrete", [Aw, Bw, Bw + up * 3.2, Aw + up * 3.2], (206, 204, 198, 255))
                # raised walkway along each wall
                Ai, Bi = A0 + nn * sgn * hw, B0 + nn * sgn * hw
                _dquad(geos, "concrete", [Ai + up * 0.25, Bi + up * 0.25, Bw + up * 0.25, Aw + up * 0.25], (170, 170, 166, 255))
                _dquad(geos, "concrete", [Ai, Bi, Bi + up * 0.25, Ai + up * 0.25], (150, 150, 146, 255))
                ex.walls.append((Ai[0], Ai[1], Bi[0], Bi[1], Ai[2] - 0.2, Ai[2] + 6.0))
            ring_n = 10
            for k in range(ring_n):
                t0, t1 = math.pi * k / ring_n, math.pi * (k + 1) / ring_n
                o0 = nn * math.cos(t0) * tw + up * (3.2 + math.sin(t0) * (th_ - 3.2))
                o1 = nn * math.cos(t1) * tw + up * (3.2 + math.sin(t1) * (th_ - 3.2))
                _dquad(geos, "concrete", [A0 + o1, B0 + o1, B0 + o0, A0 + o0], (196, 194, 188, 255))
            # lamps along the crown's shoulder (sodium-coloured, always lit)
            if i % 3 == 0:
                for sgn in (-1, 1):
                    L0 = A0 + nn * sgn * (tw - 0.8) + up * (th_ - 1.0)
                    _box_d(geos, "lamp", L0, (0.25, 0.25, 0.06))
                ex.lights.append(((A0[0], A0[1], A0[2] + th_ - 1.2), 16.0, 3))
            for end, E0, outward in ((i == 0, A0, -1.0), (i == n - 2, B0, 1.0)):
                if end:
                    _portal(geos, E0, nn, np.array([dd[0], dd[1], 0.0]) * outward, tw, th_)
        else:
            # viaduct: girder faces, soffit, parapets (collision), piers every ~30 m
            for sgn, k0, k1 in ((-1, 0, 1), (1, 3, 2)):
                A, B = V[k0], V[k1]
                _dquad(geos, "concrete", [A - up * 1.8, B - up * 1.8, B + up * 0.9, A + up * 0.9], (196, 196, 192, 255))
                ex.walls.append((A[0], A[1], B[0], B[1], A[2] - 0.2, A[2] + 1.0))
            _dquad(geos, "concrete", [V[1] - up * 1.8, V[0] - up * 1.8, V[3] - up * 1.8, V[2] - up * 1.8], (150, 150, 146, 255))
            if i % 6 == 3:
                g = ground_c(*a[:2])
                top = A0 - up * 1.8
                if top[2] - g[2] > 1.0:
                    _box_d(geos, "concrete", np.array([top[0], top[1], (top[2] + g[2]) / 2]), (1.3, hw * 0.6, (top[2] - g[2]) / 2))


def _portal(geos, P0, nn, out, tw, th_):
    """Tunnel portal: a concrete wall across the track with the arch opening, facing out along the track."""
    W, H = tw + 3.0, th_ + 3.5
    up = np.array([0.0, 0.0, 1.0])
    ring = [nn * math.cos(math.pi * k / 10) * tw + up * math.sin(math.pi * k / 10) * th_ for k in range(11)]

    def rect_pt(v):
        """Where the ray from the rail centre through v meets the wall's outline (sides +-W, top H)."""
        x, z = float(v @ nn), float(v[2])
        t = min(W / abs(x) if abs(x) > 1e-6 else 1e9, H / z if z > 1e-6 else 1e9)
        return nn * x * t + up * z * t, (abs(abs(x) * t - W) < 1e-6)

    def face(Q):
        n_ = np.cross(Q[1] - Q[0], Q[2] - Q[0])
        return Q if float(n_ @ out) > 0 else Q[::-1]

    for k in range(10):
        a, b = ring[k], ring[k + 1]
        (A2, a_side), (B2, b_side) = rect_pt(a), rect_pt(b)
        if a_side != b_side:  # the outline turns a corner between the two rays
            corner = nn * (W if float(a @ nn) + float(b @ nn) > 0 else -W) + up * H
            for tri in ([P0 + a, P0 + b, P0 + corner], [P0 + a, P0 + corner, P0 + A2], [P0 + b, P0 + B2, P0 + corner]):
                T = face(tri)
                _dquad(geos, "concrete", [T[0], T[1], T[2], T[2]])
            continue
        Q = face([P0 + a, P0 + b, P0 + B2, P0 + A2])
        _dquad(geos, "concrete", Q)
    C0, C1 = P0 - nn * (W + 0.3) + up * H, P0 + nn * (W + 0.3) + up * H
    _dquad(geos, "concrete", face([C0, C1, C1 + up * 0.6, C0 + up * 0.6]))


def _in_concourse(stations, x, y, margin=1.5) -> bool:
    """Is (x, y) inside (or within margin of) a station's walk-in concourse (see station.py)?"""
    from .station import CU0, CU1, CV
    for st in stations:
        th = math.radians(st["hd"])
        dx, dy = x - st["x"], y - st["y"]
        u = dx * math.sin(th) + dy * math.cos(th)
        v = dx * math.cos(th) - dy * math.sin(th)
        if CU0 - margin <= u <= CU1 + margin and abs(v) <= CV + margin:
            return True
    return False


def _box_d(geos, mat, c, hs, col=(255, 255, 255, 255)):
    c = np.asarray(c, float)
    hx, hy, hz = hs
    for axis in range(3):
        for s in (-1, 1):
            n = np.zeros(3)
            n[axis] = s
            u = np.zeros(3)
            v = np.zeros(3)
            u[(axis + 1) % 3] = [hx, hy, hz][(axis + 1) % 3]
            v[(axis + 2) % 3] = [hx, hy, hz][(axis + 2) % 3]
            f = c + n * [hx, hy, hz][axis]
            P = [f - u - v, f + u - v, f + u + v, f - u + v]
            if s < 0:
                P = P[::-1]
            geos.setdefault(mat, DGeo()).add(np.array(P), n, None, col, [0, 1, 2, 0, 2, 3])


def airport_layout(ap):
    """Key points of an airport's layout (shared by the geometry and the client's taxi network)."""
    (ax, ay), (bx, by), w = ap["runway"]
    A, B = np.array([ax, ay], float), np.array([bx, by], float)
    dv = B - A
    ux = dv / np.linalg.norm(dv)
    nx = np.array([-ux[1], ux[0]])
    T = np.array(ap["terminal"], float)
    if float((T - A) @ nx) < 0:
        nx = -nx
    big = ap["reclaimed"] is not None
    tw = 190.0 if big else 150.0
    ap_w = 380.0 if big else 180.0
    at = float((T - A) @ ux)
    return dict(A=A, B=B, dv=dv, ux=ux, nx=nx, tw=tw, ap_w=ap_w, at=at, big=big, w=w)


def write_extra(spec: Spec, out: str, fi) -> None:
    """rail.txt: lines (with names) and stations; transport.txt: airports, piers and ferry routes."""
    with open(os.path.join(out, "rail.txt"), "w", encoding="utf-8") as f:
        f.write("# rail polylines (lat lon z) of the fictional country's lines: line <i> <kind> <n> <name>|<english>\n")
        for k, R in enumerate(spec.rails):
            ln = L.RAIL_LINES[k]
            f.write(f"line {k} {spec.rail_kinds[k]} {len(R)} {ln['name']}|{ln['name_en']}\n")
            for x, y, z in R:
                la, lo = fi.to_geodetic(x, y)
                f.write(f"{la:.8f} {lo:.8f} {z:.2f}\n")
        for x, y, hd, length, width, ztop, kind_i, name, _path in spec.platforms:
            la, lo = fi.to_geodetic(x, y)
            f.write(f"station {kind_i} {la:.8f} {lo:.8f} {hd} {ztop:.2f} {name}\n")
        for key, (kana, roman) in L.READINGS.items():
            f.write(f"reading {key} {kana} {roman}\n")
        from . import station as S
        for st in spec.stations:  # ticket gate rows (walk-in concourses)
            f.write(S.gate_record(st, fi) + "\n")
        for i, c in enumerate(spec.crossings):  # level crossings: centre, road level, road heading, half width
            la, lo = fi.to_geodetic(c["x"], c["y"])
            f.write(f"crossing {i} {c['line']} {la:.8f} {lo:.8f} {c['z']:.2f} {c['road_hd']:.1f} {c['half']:.2f}\n")
            for px, py, facing, arm, alen in crossing_sets(c):
                la, lo = fi.to_geodetic(px, py)
                f.write(f"xset {i} {la:.8f} {lo:.8f} {c['z']:.2f} {facing:.1f} {arm:.1f} {alen:.2f}\n")
    g2 = lambda p: "%.8f %.8f" % fi.to_geodetic(float(p[0]), float(p[1]))  # noqa: E731
    with open(os.path.join(out, "transport.txt"), "w", encoding="utf-8") as f:
        for apd in L.AIRPORTS:
            a = airport_layout(apd)
            A, B, dv, ux, nx, tw, ap_w, at = a["A"], a["B"], a["dv"], a["ux"], a["nx"], a["tw"], a["ap_w"], a["at"]
            f.write(f"airport {apd['name']}|{apd['name_en']}\n")
            f.write(f"runway {g2(A)} {g2(B)} {a['w']}\n")
            # taxi network: parallel taxiway, runway connectors, apron links, apron taxilane and the gate
            # stands in front of the terminal (nose towards the building)
            f.write(f"taxiway {g2(A + nx * tw)} {g2(B + nx * tw)}\n")
            for t in (0.08, 0.35, 0.65, 0.92):
                f.write(f"connector {g2(A + dv * t)} {g2(A + dv * t + nx * tw)}\n")
            for s_ in (at - ap_w * 0.53, at + ap_w * 0.53):
                f.write(f"apronlink {g2(A + ux * s_ + nx * tw)} {g2(A + ux * s_ + nx * (tw + 140))}\n")
            f.write(f"apronlane {g2(A + ux * (at - ap_w * 0.95) + nx * (tw + 140))} {g2(A + ux * (at + ap_w * 0.95) + nx * (tw + 140))}\n")
            hd_stand = math.degrees(math.atan2(nx[0], nx[1])) % 360
            offs = (-300, -180, -60, 60, 180, 300) if a["big"] else (-60, 60)
            for s_ in offs:
                f.write(f"stand {g2(A + ux * (at + s_) + nx * (tw + 208))} {hd_stand:.1f}\n")
            tc = A + ux * at + nx * (tw + 190 + 80 + (30 if a["big"] else 17.5))
            f.write(f"terminal {g2(tc)}\n")
        for name, (x, y, hd) in L.PIERS.items():
            la, lo = fi.to_geodetic(x, y)
            f.write(f"pier {la:.8f} {lo:.8f} {hd} {name}\n")
        for k, (a_, b_, kind, route) in enumerate(L.FERRY_ROUTES):
            f.write(f"ferry {k} {kind} {a_}|{b_} " + " ".join("%.8f,%.8f" % fi.to_geodetic(x, y) for x, y in route) + "\n")
