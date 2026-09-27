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
                        BuildingOut, Geo, box, cap, cylinder, pitched_roof, walls)
from .generate import parts
from realjapan_pipeline.streetdetail import Geo as DGeo

RAIL_DECK = 9.0  # rail level above the ground (elevated loop line)


def chaikin(pts, it=3, closed=False):
    P = np.asarray(pts, float)
    for _ in range(it):
        Q = []
        n = len(P)
        rng = range(n) if closed else range(n - 1)
        if not closed:
            Q.append(P[0])
        for i in rng:
            a, b = P[i], P[(i + 1) % n]
            Q += [0.75 * a + 0.25 * b, 0.25 * a + 0.75 * b]
        if not closed:
            Q.append(P[-1])
        P = np.array(Q)
    return P


def resample(P, step):
    line = LineString(P)
    n = max(2, int(line.length / step) + 1)
    return np.array([line.interpolate(t, normalized=True).coords[0] for t in np.linspace(0, 1, n)])


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


class CellExtra:
    def __init__(self):
        self.decks = []
        self.trees = []


def _named(g: Geo, ring, ground, h, usage, name, kind, storeys=1):
    return BuildingOut(g, Geo(), np.asarray(ring)[:-1] if np.allclose(ring[0], ring[-1]) else np.asarray(ring), ground,
                       h, storeys, usage, name, kind)


def _station(name, x, y, hd, terrain, big, deck=RAIL_DECK):
    ground = float(terrain.sample(x, y))
    rect = affinity.rotate(sbox(x - 16, y - 105, x + 16, y + 105), -hd, origin=(x, y))
    rect = orient(rect, 1.0)
    ring = np.asarray(rect.exterior.coords)
    g = Geo()
    # concourse (ground floor), deck slab, platforms and canopy over the elevated tracks
    walls(g, ring, ground - 1.5, ground + 6.5, ground, PUBLIC, (200, 198, 190))
    cap(g, rect, ground + 6.5, ROOF_FLAT, (150, 150, 146))
    if deck is None:  # Shinkansen: concourse only; platforms / canopy are generated with the line
        return [_named(g, ring, ground, 6.5, 431, name, "station", 2)]
    deck = ground + deck
    th = math.radians(hd)
    fwd = (math.sin(th), math.cos(th))
    # (walkable platforms and their canopies are generated with the line, see cell_detail)
    # the building's solid volume is the concourse below the elevated platforms (which are walkable
    # decks generated with the line): its height must stay below platform level
    out = [_named(g, ring, ground, 6.5, 431, name, "station", 2)]
    if big:  # the main station has a tall station building beside the tracks (department store + offices)
        side = np.array([math.cos(th), -math.sin(th)])
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


def _airport(terrain, spec: Spec):
    (ax, ay), (bx, by), w = L.RUNWAY
    rw = LineString([(ax, ay), (bx, by)])
    d = np.array([bx - ax, by - ay])
    Lr = np.linalg.norm(d)
    ux = d / Lr
    nx = np.array([-ux[1], ux[0]])
    spec.runway = rw.buffer(w / 2, cap_style=2)
    twy = LineString([(ax, ay) + nx * 190, (bx, by) + nx * 190])
    taxi = twy.buffer(12, cap_style=2)
    for t in (0.08, 0.35, 0.65, 0.92):
        p0 = np.array([ax, ay]) + d * t
        taxi = taxi.union(LineString([p0, p0 + nx * 190]).buffer(11.5))
    tx, ty = L.TERMINAL
    along_t = float((np.array([tx, ty]) - np.array([ax, ay])) @ ux)
    for s_ in (along_t - 200, along_t + 200):  # taxiway -> apron links
        p0 = np.array([ax, ay]) + ux * s_
        taxi = taxi.union(LineString([p0 + nx * 190, p0 + nx * 300]).buffer(11.5))
    apron = orient(affinity.rotate(sbox(tx - 380, ty - 90, tx + 380, ty + 70), math.degrees(math.atan2(ux[1], ux[0])), origin=(tx, ty)), 1.0)
    spec.apron = unary_union([apron, taxi]).difference(spec.runway)
    ap = Polygon(L.AIRPORT)
    spec.grass = ap.difference(spec.apron).difference(spec.runway.buffer(1))
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
    spec.marks = unary_union(marks)
    out = []
    ground = float(terrain.sample(tx, ty))
    g = Geo()
    ang = math.degrees(math.atan2(ux[1], ux[0]))
    term = orient(affinity.rotate(sbox(tx - 160, ty + 70, tx + 160, ty + 130), ang, origin=(tx, ty)), 1.0)
    walls(g, np.asarray(term.exterior.coords), ground - 1, ground + 22, ground, CURTAIN, (90, 120, 140))
    cap(g, term, ground + 22, ROOF_METAL, (190, 194, 198))
    out.append(_named(g, np.asarray(term.exterior.coords), ground, 22, 431, "千景空港ターミナル", "terminal", 4))
    g2 = Geo()
    ctx, cty = np.array([tx, ty]) + ux * 230 + nx * 120
    cylinder(g2, (ctx, cty, ground - 1), 3.2, 52, PANEL, (220, 220, 216), ground, seg=12, top=False)
    cylinder(g2, (ctx, cty, ground + 51), 6.5, 6, GLASS, (60, 90, 110), ground, seg=12)
    out.append(_named(g2, np.asarray(Point(ctx, cty).buffer(6.5, 8).exterior.coords), ground, 58, 431, "管制塔", "control_tower", 12))
    for k in range(3):  # hangars
        g3 = Geo()
        hx, hy = np.array([tx, ty]) - ux * (330 + k * 90) + nx * 40
        hg = orient(affinity.rotate(sbox(hx - 38, hy - 30, hx + 38, hy + 30), ang, origin=(hx, hy)), 1.0)
        walls(g3, np.asarray(hg.exterior.coords), ground - 1, ground + 20, ground, PLAIN, (200, 204, 208))
        cap(g3, hg, ground + 20, ROOF_METAL, (170, 175, 180))
        out.append(_named(g3, np.asarray(hg.exterior.coords), ground, 20, 431, "", "hangar", 1))
    return out


def _ferry_terminal(terrain):
    x, y = L.FERRY_TERMINAL
    ground = float(terrain.sample(x, y))
    g = Geo()
    b = orient(sbox(x - 40, y + 10, x + 40, y + 50), 1.0)
    walls(g, np.asarray(b.exterior.coords), ground - 1, ground + 13, ground, PUBLIC, (214, 214, 208))
    cap(g, b, ground + 13, ROOF_METAL, (80, 120, 160))
    return [_named(g, np.asarray(b.exterior.coords), ground, 13, 431, "千景港フェリーターミナル", "ferry_terminal", 2)]


def build_all(isl, terrain, rng) -> Spec:
    spec = Spec()
    for k, (name, x, y, hd) in enumerate(L.STATIONS):
        spec.buildings.extend(_station(name, x, y, hd, terrain, big=(k == 0)))
    t = L.LANDMARKS
    spec.buildings += _temple(t["temple"][0], t["temple"][1], t["temple"][2], terrain)
    spec.buildings += _shrine(t["shrine"][0], t["shrine"][1], t["shrine"][2], terrain)
    spec.buildings += _tower(t["tower"][0], t["tower"][1], t["tower"][2], terrain)
    spec.buildings += _wheel(t["wheel"][0], t["wheel"][1], t["wheel"][2], terrain)
    spec.buildings += _stadium(t["stadium"][0], t["stadium"][1], t["stadium"][2], terrain)
    spec.buildings += _airport(terrain, spec)
    spec.buildings += _ferry_terminal(terrain)
    # forest: the mountain district away from roads and the summit shrine
    mnt = unary_union([Polygon(p).buffer(0) for n, s, p in L.DISTRICTS if s == "mountain"]).intersection(isl.land)
    road_clear = unary_union([r.line.buffer(r.width / 2 + 4) for r in isl.net.roads if r.kind in ("mountain", "arterial")])
    spec.forest = mnt.difference(road_clear).difference(Point(*t["shrine"][1:]).buffer(60)).difference(isl.river.poly.buffer(6))
    spec.canopy = spec.forest.buffer(-6)
    spec.fields = Polygon()
    # elevated rail (loop + branch + Shinkansen), smoothed
    from scipy.ndimage import gaussian_filter1d
    for pts, closed, kind, deck in ((L.RAIL_LOOP[:-1], True, "loop", RAIL_DECK), (L.RAIL_BRANCH, False, "branch", RAIL_DECK),
                                    (L.SHINKANSEN, False, "shinkansen", L.SHINKANSEN_DECK)):
        P = resample(chaikin(pts, 4, closed), 6.0)
        if closed:
            P = np.vstack([P, P[:1]])
        gz = np.maximum(terrain.sample(P[:, 0], P[:, 1]), 2.0)
        if kind == "shinkansen":
            # grade-limited profile: bridges over low ground, tunnels through the mountain (max 2.5 %)
            z = gz + deck
            z = gaussian_filter1d(np.minimum(z, 80.0), 30, mode="nearest")
            for k in range(1, len(z)):
                z[k] = np.clip(z[k], z[k - 1] - 0.15, z[k - 1] + 0.15)
            for k in range(len(z) - 2, -1, -1):
                z[k] = np.clip(z[k], z[k + 1] - 0.15, z[k + 1] + 0.15)
        else:
            z = gaussian_filter1d(gz, 25, mode="wrap" if closed else "nearest") + deck
        spec.rails.append(np.column_stack([P, z]))
        spec.rail_kinds.append(kind)
    for name, x, y, hd in L.SHINKANSEN_STATIONS:
        spec.buildings.extend(_station(name, x, y, hd, terrain, big=False, deck=None))
    # platform decks (walkable) for every station: level with the rail line passing through
    for (name, x, y, hd), kind_i in [(s_, 0) for s_ in L.STATIONS[:4]] + [(s_, 1) for s_ in L.STATIONS[4:]] + \
            [(s_, 2) for s_ in L.SHINKANSEN_STATIONS]:
        R = spec.rails[kind_i]
        k = int(np.argmin(np.hypot(R[:, 0] - x, R[:, 1] - y)))
        spec.platforms.append((x, y, hd, 200.0 if kind_i < 2 else 320.0, 5.0, float(R[k, 2]) + 1.1, kind_i))
    for x, y, hd in L.FERRY_PIERS:
        spec.piers.append((x, y, hd, 120.0))
    # bridges: roads crossing the river (flat decks) and the bay bridge (arched)
    for r in isl.net.roads:
        for water, arch in ((isl.river.poly, False), (None, True)):
            if arch:
                if r.name not in L.BRIDGE_ROADS:
                    continue
                seg = r.line.difference(isl.land.union(isl.islet).buffer(-2))
            else:
                if not r.line.intersects(water):
                    continue
                seg = r.line.intersection(water.buffer(10))
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
    # seawalls: every coast except the beach
    beach = Polygon(L.BEACH).buffer(120)
    for poly in (isl.land, isl.islet):
        for p in parts(poly):
            ring = LineString(p.exterior.coords)
            g = ring.difference(beach)
            for part in getattr(g, "geoms", [g]):
                if isinstance(part, LineString) and part.length > 10:
                    spec.quays.append(part)
    # trees: street trees on wide roads, parks, temple, mountain road edges
    carr = isl.net.carriage.buffer(0.4)
    import shapely
    shapely.prepare(carr)
    for r in isl.net.roads:
        if r.sidewalk < 3.5:
            continue
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
    tx, ty = t["temple"][1], t["temple"][2]
    for _ in range(60):
        a, rr = rng.uniform(0, 2 * math.pi), rng.uniform(40, 105)
        spec.trees.append((tx + rr * math.cos(a), ty + rr * math.sin(a), rng.uniform(9, 17), rng.uniform(3, 5), 0))
    return spec


# --------------------------------------------------------------------------------------------
def _dquad(geos, mat, P4, col=(255, 255, 255, 255)):
    P = np.asarray(P4, float)
    n = np.cross(P[1] - P[0], P[3] - P[0])
    ln = np.linalg.norm(n)
    if ln < 1e-9:
        return
    geos.setdefault(mat, DGeo()).add(P, n / ln, None, col, [0, 1, 2, 0, 2, 3])


def cell_detail(spec: Spec, isl, cpoly: Polygon, xf, ts, rng, geos) -> CellExtra:
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
    # --- river water surface ---
    P = np.asarray(isl.river.center.coords)
    W, Z = isl.river.widths, isl.river.water
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
    import shapely
    carr_p = isl.net.carriage.intersection(clip.buffer(50))
    shapely.prepare(carr_p)
    # --- station platforms (walkable) with canopies ---
    for x, y, hd, length, width, ztop, kind_i in spec.platforms:
        if not clip.buffer(length).contains(Point(x, y)):
            continue
        th = math.radians(hd)
        fwd = np.array([math.sin(th), math.cos(th)])
        side = np.array([math.cos(th), -math.sin(th)])
        # platform edge 1.6 m (commuter) / 1.76 m (Shinkansen) from the track centre (tracks at 2.5 / 3.15 m)
        off = 6.6 if kind_i < 2 else 7.4
        for sg in (-1, 1):
            c = np.array([x, y]) + side * off * sg
            q = [c - fwd * length / 2 - side * width / 2, c + fwd * length / 2 - side * width / 2,
                 c + fwd * length / 2 + side * width / 2, c - fwd * length / 2 + side * width / 2]
            V = xf.p([[p[0], p[1], ztop] for p in q])
            _dquad(geos, "sidewalk", V)
            ex.decks.append(np.array([[V[0], V[1], V[2]], [V[0], V[2], V[3]]]))
            for k0, k1 in ((0, 1), (1, 2), (2, 3), (3, 0)):
                A, B = V[k0], V[k1]
                _dquad(geos, "concrete", [A - [0, 0, 1.1], B - [0, 0, 1.1], B, A])
            if kind_i == 2:
                # platform screen doors (typical of Shinkansen stations): 1.3 m fence 0.5 m back from the
                # edge with openings at the car doors of an 8 x 25 m train stopped at the centre
                # (one door at the rear of each car; the reversed rear cab has it at the front end)
                fdir = 1.0 if sg < 0 else -1.0  # trains on the left platform run along the heading
                opens = []
                for k in range(8):
                    u = (3.5 - k) * 25.0 + (11.4 if k == 7 else -11.4)
                    opens.append(u * fdir)
                opens.sort()
                edge = c - side * sg * (width / 2 - 0.5)
                u = -length / 2 + 2.0
                cuts = []
                for o in opens:
                    if o - 1.15 > u:
                        cuts.append((u, o - 1.15))
                    u = o + 1.15
                cuts.append((u, length / 2 - 2.0))
                for ua, ub in cuts:
                    if ub - ua < 0.3:
                        continue
                    pa, pb = edge + fwd * ua, edge + fwd * ub
                    q2 = [pa, pb]
                    Vp = xf.p([[p_[0], p_[1], ztop] for p_ in q2] + [[p_[0], p_[1], ztop + 1.3] for p_ in q2[::-1]])
                    _dquad(geos, "metal", [Vp[0], Vp[1], Vp[2], Vp[3]], (214, 216, 220, 255))
                    _dquad(geos, "metal", [Vp[1], Vp[0], Vp[3], Vp[2]], (214, 216, 220, 255))
                    for uu in (ua, ub):  # door pockets / posts
                        P0 = xf.p([[*(edge + fwd * uu), ztop]])[0]
                        _box_d(geos, "metal_dark", P0 + [0, 0, 0.67], (0.12, 0.12, 0.67))
            # tactile strip along the track edge + canopy
            e0 = c - side * sg * (width / 2 - 0.9)
            t = [e0 - fwd * length / 2 - side * 0.3, e0 + fwd * length / 2 - side * 0.3, e0 + fwd * length / 2 + side * 0.3,
                 e0 - fwd * length / 2 + side * 0.3]
            _dquad(geos, "tactile", xf.p([[p[0], p[1], ztop + 0.01] for p in t]))
            cq = [c - fwd * length * 0.4 - side * (width / 2 + 0.6), c + fwd * length * 0.4 - side * (width / 2 + 0.6),
                  c + fwd * length * 0.4 + side * (width / 2 + 0.6), c - fwd * length * 0.4 + side * (width / 2 + 0.6)]
            Vc = xf.p([[p[0], p[1], ztop + 3.4] for p in cq])
            _dquad(geos, "metal_dark", Vc[::-1])
            _dquad(geos, "metal", Vc)
            for s_ in np.linspace(-0.38, 0.38, 7):
                pc = c + fwd * length * s_
                P0 = xf.p([[pc[0], pc[1], ztop]])[0]
                _box_d(geos, "metal", P0 + [0, 0, 1.7], (0.12, 0.12, 1.7))
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
    stations_xy = np.array([(x, y) for _, x, y, _ in list(L.STATIONS) + list(L.SHINKANSEN_STATIONS)], float)
    for ri, R in enumerate(spec.rails):
        shink = spec.rail_kinds[ri] == "shinkansen" if spec.rail_kinds else False
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
            if shink and ga[2] > xf.p([[a[0], a[1], a[2]]])[0][2] + 7.0:
                # tunnel: a concrete tube around the track (seen from the train)
                ring_n = 10
                A0 = xf.p([[a[0], a[1], a[2]]])[0]
                B0 = xf.p([[b[0], b[1], b[2]]])[0]
                nn = np.array([nrm[0], nrm[1], 0.0])
                for k in range(ring_n):
                    t0, t1 = math.pi * k / ring_n, math.pi * (k + 1) / ring_n
                    o0 = nn * math.cos(t0) * 6.0 + np.array([0, 0, math.sin(t0) * 7.0])
                    o1 = nn * math.cos(t1) * 6.0 + np.array([0, 0, math.sin(t1) * 7.0])
                    _dquad(geos, "concrete", [A0 + o1, B0 + o1, B0 + o0, A0 + o0])
                _dquad(geos, "ballast", [A0 - nn * 6, B0 - nn * 6, B0 + nn * 6, A0 + nn * 6])
                continue
            q = [(a[:2] - nrm * hw, a[2]), (b[:2] - nrm * hw, b[2]), (b[:2] + nrm * hw, b[2]), (a[:2] + nrm * hw, a[2])]
            V = xf.p([[p[0], p[1], z] for p, z in q])
            _dquad(geos, "ballast", V)
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
            if i % 4 == 0 and not shapely.contains_xy(carr_p, a[0], a[1]):  # pier every ~24 m, never on a carriageway
                g = ground_c(*a[:2])
                top = xf.p([[a[0], a[1], a[2] - 1.4]])[0]
                _box_d(geos, "concrete", np.array([top[0], top[1], (top[2] + g[2]) / 2]), (1.1, 1.1, max(0.5, (top[2] - g[2]) / 2)))
    # --- mountain forest canopy (bumpy crown surface over the forest) ---
    if spec.canopy is not None and not spec.canopy.is_empty and spec.canopy.intersects(clip):
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
    if len(stations_xy) and np.min(np.hypot(stations_xy[:, 0] - a[0], stations_xy[:, 1] - a[1])) < 125.0:
        return
    steel = (150, 154, 158, 255)
    cols = []
    for sg in (-1.0, 1.0):
        pc = a[:2] + nrm * sg * (hw - 0.35)
        P = xf.p([[pc[0], pc[1], a[2]]])[0]
        _box_d(geos, "metal", P + [0, 0, (FEEDER_H + 0.3) / 2], (0.13, 0.13, (FEEDER_H + 0.3) / 2))
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


def _box_d(geos, mat, c, hs):
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
            geos.setdefault(mat, DGeo()).add(np.array(P), n, None, (255, 255, 255, 255), [0, 1, 2, 0, 2, 3])


def write_extra(spec: Spec, out: str, fi) -> None:
    """rail.txt: the loop line and branch (for trains), stations; transport.txt: airport / ferry."""
    with open(os.path.join(out, "rail.txt"), "w", encoding="utf-8") as f:
        f.write("# rail polylines (lat lon z) of the fictional island's lines\n")
        for k, R in enumerate(spec.rails):
            f.write(f"line {k} {spec.rail_kinds[k]} {len(R)}\n")
            for x, y, z in R:
                la, lo = fi.to_geodetic(x, y)
                f.write(f"{la:.8f} {lo:.8f} {z:.2f}\n")
        for x, y, hd, length, width, ztop, kind_i in spec.platforms:
            la, lo = fi.to_geodetic(x, y)
            names = [n for n, sx, sy, _ in list(L.STATIONS) + list(L.SHINKANSEN_STATIONS) if abs(sx - x) < 1 and abs(sy - y) < 1]
            f.write(f"station {kind_i} {la:.8f} {lo:.8f} {hd} {ztop:.2f} {names[0] if names else '?'}\n")
    with open(os.path.join(out, "transport.txt"), "w", encoding="utf-8") as f:
        (ax, ay), (bx, by), w = L.RUNWAY
        la0, lo0 = fi.to_geodetic(ax, ay)
        la1, lo1 = fi.to_geodetic(bx, by)
        f.write(f"runway {la0:.8f} {lo0:.8f} {la1:.8f} {lo1:.8f} {w}\n")
        # taxi network (island metres -> geodetic): parallel taxiway, runway connectors, apron links,
        # apron taxilane and the gate stands in front of the terminal (nose towards the building)
        A, B = np.array([ax, ay], float), np.array([bx, by], float)
        dv = B - A
        ux_ = dv / np.linalg.norm(dv)
        nx_ = np.array([-ux_[1], ux_[0]])
        T = np.array(L.TERMINAL, float)
        at = float((T - A) @ ux_)
        g2 = lambda p: "%.8f %.8f" % fi.to_geodetic(float(p[0]), float(p[1]))
        f.write(f"taxiway {g2(A + nx_ * 190)} {g2(B + nx_ * 190)}\n")
        for t in (0.08, 0.35, 0.65, 0.92):
            f.write(f"connector {g2(A + dv * t)} {g2(A + dv * t + nx_ * 190)}\n")
        for s_ in (at - 200, at + 200):
            f.write(f"apronlink {g2(A + ux_ * s_ + nx_ * 190)} {g2(A + ux_ * s_ + nx_ * 330)}\n")
        f.write(f"apronlane {g2(A + ux_ * (at - 360) + nx_ * 330)} {g2(A + ux_ * (at + 360) + nx_ * 330)}\n")
        hd_stand = math.degrees(math.atan2(nx_[0], nx_[1])) % 360
        for s_ in (-300, -180, -60, 60, 180, 300):
            f.write(f"stand {g2(A + ux_ * (at + s_) + nx_ * 398)} {hd_stand:.1f}\n")
        f.write(f"terminal {g2(T)}\n")
        for x, y, hd in L.FERRY_PIERS:
            la, lo = fi.to_geodetic(x, y)
            f.write(f"pier {la:.8f} {lo:.8f} {hd}\n")
        for k, route in enumerate(L.FERRY_ROUTES):
            f.write(f"ferry {k} " + " ".join("%.8f,%.8f" % fi.to_geodetic(x, y) for x, y in route) + "\n")
