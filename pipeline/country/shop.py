"""Walk-in ground floors of the fictional country's small shops (generic designs; no real chain or
brand is reproduced, no names or logos).

A shop building's walls start above its ground floor; the ground floor is built here as detail
geometry the player walks into: a glazed shop front on the street side with an open sliding door
and a coloured fascia band above it, solid walls elsewhere, a floor and a lit ceiling, and the fit
out of one of a few generic kinds - a convenience store (shelf gondolas, a till counter, a wall of
chilled cabinets), a cafe (tables and chairs, a counter), a small general shop (wall shelves and a
counter). Walkable floor into the cell's deck triangles, collision walls into its WALL section,
indoor lights (kind 3, always on).
"""
from __future__ import annotations

import math

import numpy as np
from shapely.geometry import Polygon
from shapely.geometry.polygon import orient

FLOOR_H = 3.4          # the building's walls start this high above the ground
KINDS = ("konbini", "cafe", "general")
FASCIA = {"konbini": [(40, 120, 190), (60, 160, 90), (220, 120, 40)], "cafe": [(90, 60, 40), (40, 70, 60)],
          "general": [(170, 40, 40), (60, 60, 120), (200, 170, 60)]}


def plan(fp: Polygon, front, ground: float, rng):
    """A walk-in shop for this footprint (or None): the front edge must face the street and be at
    least 4.5 m long, the floor plate between 30 and 450 m2."""
    if front == (0.0, 0.0) or not (30.0 <= fp.area <= 450.0):
        return None
    fp = orient(fp, 1.0)
    ring = np.asarray(fp.exterior.coords)[:-1]
    fx, fy = front
    best, bi = -1.0, -1
    for i in range(len(ring)):
        a, b = ring[i], ring[(i + 1) % len(ring)]
        d = b - a
        L_ = float(np.hypot(*d))
        if L_ < 4.5:
            continue
        nx, ny = d[1] / L_, -d[0] / L_  # outward normal of a CCW ring
        s = nx * fx + ny * fy
        if s > 0.8 and L_ > best:
            best, bi = L_, i
    if bi < 0:
        return None
    kind = KINDS[int(rng.integers(len(KINDS)))] if fp.area > 60 else "general"
    fascia = FASCIA[kind][int(rng.integers(len(FASCIA[kind])))]
    return dict(ring=ring, ground=ground, front_edge=bi, kind=kind, fascia=fascia, seed=int(rng.integers(1 << 30)))


def build(geos, ex, xf, shop) -> None:
    from .specials import _box_d, _dquad
    ring, g = shop["ring"], shop["ground"]
    n = len(ring)
    rng = np.random.default_rng(shop["seed"])
    zf, zc = g + 0.06, g + 3.0
    wall_in = (228, 226, 220, 255)
    up = np.array([0, 0, 1.0])

    def P(x, y, z):
        return xf.p([[x, y, z]])[0]

    def quad(mat, pts, col):
        _dquad(geos, mat, xf.p(np.asarray(pts, float)), col)

    def wall(ax, ay, bx, by, zlo, zhi):
        A, B = P(ax, ay, zlo), P(bx, by, zlo)
        ex.walls.append((A[0], A[1], B[0], B[1], A[2], A[2] + (zhi - zlo)))

    # floor (walkable) and ceiling with light panels
    poly = Polygon(ring)
    inner = poly.buffer(-0.12, join_style=2)
    if not isinstance(inner, Polygon) or inner.area < 10:
        return
    import mapbox_earcut as earcut
    verts = np.asarray(orient(inner, 1.0).exterior.coords)[:-1]
    tri = earcut.triangulate_float64(verts, np.array([len(verts)], np.uint32)).reshape(-1, 3)
    for t in tri:
        A, B, C = (P(verts[k][0], verts[k][1], zf) for k in t)
        _dquad(geos, "sidewalk", [A, B, C, C], (196, 194, 188, 255))
        ex.decks.append(np.array([[A, B, C]]))
        A2, B2, C2 = (P(verts[k][0], verts[k][1], zc) for k in t)
        _dquad(geos, "concrete", [A2, C2, B2, B2], (238, 238, 234, 255))
    # slab edge between the ground floor and the walls above
    fe = shop["front_edge"]
    for i in range(n):
        a, b = ring[i], ring[(i + 1) % n]
        d = b - a
        L_ = float(np.hypot(*d))
        if L_ < 0.05:
            continue
        u = d / L_
        nrm = np.array([u[1], -u[0]])  # outward
        ai, bi_ = a - nrm * 0.12, b - nrm * 0.12  # inner face line
        if i == fe:
            # shop front: fascia band, glazing with mullions, the door (open) in the middle
            col = (*shop["fascia"], 255)
            quad("sign", [(a[0], a[1], g + 2.75), (b[0], b[1], g + 2.75), (b[0], b[1], g + FLOOR_H), (a[0], a[1], g + FLOOR_H)], col)
            quad("sign", [(a[0] + nrm[0] * 0.02, a[1] + nrm[1] * 0.02, g + 2.72), (b[0] + nrm[0] * 0.02, b[1] + nrm[1] * 0.02, g + 2.72),
                          (b[0] + nrm[0] * 0.02, b[1] + nrm[1] * 0.02, g + 2.78), (a[0] + nrm[0] * 0.02, a[1] + nrm[1] * 0.02, g + 2.78)],
                 (240, 240, 236, 255))
            dm = L_ / 2
            d0, d1 = dm - 0.85, dm + 0.85
            for s0, s1 in ((0.0, d0), (d1, L_)):
                if s1 - s0 < 0.05:
                    continue
                q0, q1 = a + u * s0, a + u * s1
                # (the panes are left open: the renderer has no see-through glass, and the lit shop
                # seen through the window is what a shop front looks like; the collision wall stays)
                quad("metal", [(q0[0], q0[1], g), (q1[0], q1[1], g), (q1[0], q1[1], g + 0.35), (q0[0], q0[1], g + 0.35)], (120, 124, 130, 255))
                quad("metal", [(q0[0], q0[1], g + 0.35), (q1[0], q1[1], g + 0.35), (q1[0], q1[1], g + 0.40), (q0[0], q0[1], g + 0.40)], (96, 100, 106, 255))
                wall(q0[0], q0[1], q1[0], q1[1], g - 0.3, g + 2.8)
                k = int(max(1, round((s1 - s0) / 1.6)))
                for m in range(k + 1):  # mullions
                    q = a + u * (s0 + (s1 - s0) * m / k)
                    _box_d(geos, "metal", P(q[0], q[1], g + 1.4), (0.04, 0.04, 1.4), (110, 114, 120, 255))
            # the door leaves' frames, slid open behind the glazing, and a mat inside
            for s0 in (d0 - 0.85, d1):
                for e in (0.0, 0.85):
                    q = a + u * (s0 + e) - nrm * 0.05
                    _box_d(geos, "metal", P(q[0], q[1], g + 1.25), (0.03, 0.03, 1.2), (150, 154, 160, 255))
                q0, q1 = a + u * s0 - nrm * 0.05, a + u * (s0 + 0.85) - nrm * 0.05
                quad("metal", [(q0[0], q0[1], g + 0.05), (q1[0], q1[1], g + 0.05), (q1[0], q1[1], g + 0.12), (q0[0], q0[1], g + 0.12)], (150, 154, 160, 255))
                quad("metal", [(q0[0], q0[1], g + 2.38), (q1[0], q1[1], g + 2.38), (q1[0], q1[1], g + 2.45), (q0[0], q0[1], g + 2.45)], (150, 154, 160, 255))
            m0, m1, m2, m3 = a + u * d0 - nrm * 0.2, a + u * d1 - nrm * 0.2, a + u * d1 - nrm * 1.4, a + u * d0 - nrm * 1.4
            quad("sidewalk", [(m[0], m[1], zf + 0.01) for m in (m0, m1, m2, m3)], (60, 62, 66, 255))
            # a low step up at the door (the floor meets the pavement)
            dq0, dq1 = a + u * d0, a + u * d1
            _dquad(geos, "sidewalk", xf.p([[dq0[0] + nrm[0] * 0.4, dq0[1] + nrm[1] * 0.4, g + 0.01], [dq1[0] + nrm[0] * 0.4, dq1[1] + nrm[1] * 0.4, g + 0.01],
                                           [dq1[0], dq1[1], zf], [dq0[0], dq0[1], zf]]), (170, 170, 166, 255))
            ex.decks.append(xf.p([[dq0[0] + nrm[0] * 0.4, dq0[1] + nrm[1] * 0.4, g + 0.01], [dq1[0] + nrm[0] * 0.4, dq1[1] + nrm[1] * 0.4, g + 0.01],
                                  [dq1[0], dq1[1], zf]])[None])
            ex.decks.append(xf.p([[dq0[0] + nrm[0] * 0.4, dq0[1] + nrm[1] * 0.4, g + 0.01], [dq1[0], dq1[1], zf], [dq0[0], dq0[1], zf]])[None])
        else:
            # solid wall: outside (plaster, like the building above) and inside faces
            quad("concrete", [(a[0], a[1], g - 0.3), (b[0], b[1], g - 0.3), (b[0], b[1], g + FLOOR_H), (a[0], a[1], g + FLOOR_H)], (196, 194, 188, 255))
            quad("concrete", [(bi_[0], bi_[1], g), (ai[0], ai[1], g), (ai[0], ai[1], zc), (bi_[0], bi_[1], zc)], wall_in)
            wall(a[0], a[1], b[0], b[1], g - 0.3, g + FLOOR_H)
    # fit-out: the room's frame (u along the front, v inwards)
    a, b = ring[fe], ring[(fe + 1) % n]
    u = (b - a) / max(np.hypot(*(b - a)), 1e-9)
    v = np.array([-u[1], u[0]])  # inward (left of a CCW edge)
    pts = np.asarray(inner.exterior.coords)[:-1]
    us, vs = (pts - a) @ u, (pts - a) @ v
    u0, u1, v0, v1 = float(us.min()) + 0.4, float(us.max()) - 0.4, max(0.0, float(vs.min())) + 1.8, float(vs.max()) - 0.5
    if u1 - u0 < 2.5 or v1 - v0 < 1.5:
        return

    def at(uu, vv, z):
        q = a + u * uu + v * vv
        return P(q[0], q[1], z)

    def obox(mat, uu, vv, z, hu, hv, hz, col):
        # a box aligned with the room: centre, half sizes along u, v, z
        C = np.array([[su, sv, sz] for sz in (-1, 1) for sv in (-1, 1) for su in (-1, 1)], float)
        V = np.array([at(uu + su * hu, vv + sv * hv, z + sz * hz) for su, sv, sz in C])
        for f in ((0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)):
            _dquad(geos, mat, [V[f[0]], V[f[1]], V[f[2]], V[f[3]]], col)

    def owall(ua, va, ub, vb, h):
        A, B = at(ua, va, zf), at(ub, vb, zf)
        ex.walls.append((A[0], A[1], B[0], B[1], A[2] - 0.2, A[2] + h))

    kind = shop["kind"]
    if kind == "konbini":
        # chilled cabinets along the back wall (lit glass doors), gondolas in rows, till counter by the door
        obox("lamp", (u0 + u1) / 2, v1 - 0.35, zf + 1.0, (u1 - u0) / 2, 0.3, 1.0, (215, 235, 245, 255))
        owall(u0, v1 - 0.7, u1, v1 - 0.7, 2.0)
        cu = u0 + 1.4
        obox("metal", cu, v0 - 0.9, zf + 0.5, 1.1, 0.35, 0.5, (200, 200, 196, 255))
        obox("metal_dark", cu + 0.6, v0 - 0.9, zf + 1.1, 0.18, 0.15, 0.1, (40, 44, 50, 255))
        owall(cu - 1.1, v0 - 0.55, cu + 1.1, v0 - 0.55, 1.1)
        # gondolas run inwards from the front, so the aisles lead from the door to the cabinets
        gv0, gv1 = v0 + 0.5, v1 - 1.5
        if gv1 - gv0 >= 1.5:
            for ur in np.arange(u0 + 3.2, u1 - 0.6, 1.95):
                obox("metal", ur, (gv0 + gv1) / 2, zf + 0.68, 0.3, (gv1 - gv0) / 2, 0.68, (226, 228, 230, 255))
                obox("metal", ur, (gv0 + gv1) / 2, zf + 1.38, 0.02, (gv1 - gv0) / 2, 0.02, (200, 202, 206, 255))
                for side in (-1.0, 1.0):
                    for zz in (0.08, 0.5, 0.92):
                        _goods(geos, at, rng, ur + side * 0.31, side, gv0 + 0.05, gv1 - 0.05, zf + zz)
                owall(ur, gv0, ur, gv1, 1.4)
    elif kind == "cafe":
        cu = u1 - 1.2
        obox("metal_dark", cu, (v0 + v1) / 2, zf + 0.55, 0.45, (v1 - v0) / 2 - 0.2, 0.55, (70, 50, 36, 255))  # counter
        owall(cu - 0.45, v0, cu - 0.45, v1, 1.1)
        for uu in np.arange(u0 + 0.9, cu - 1.4, 1.8):
            for vv in np.arange(v0 + 0.3, v1 - 0.6, 1.9):
                obox("metal_dark", uu, vv, zf + 0.72, 0.4, 0.4, 0.03, (120, 90, 60, 255))  # table top
                obox("metal", uu, vv, zf + 0.36, 0.05, 0.05, 0.36, (60, 60, 64, 255))
                for s in (-1, 1):  # chairs
                    obox("metal_dark", uu + s * 0.62, vv, zf + 0.45, 0.2, 0.2, 0.03, (90, 70, 50, 255))
                    obox("metal_dark", uu + s * 0.8, vv, zf + 0.7, 0.02, 0.2, 0.25, (90, 70, 50, 255))
                owall(uu - 0.4, vv, uu + 0.4, vv, 0.8)
    else:
        # wall shelving on the side walls (goods on four shelves), a counter at the back
        for uu, side in ((u0 + 0.25, 1.0), (u1 - 0.25, -1.0)):
            obox("metal", uu - side * 0.04, (v0 + v1) / 2, zf + 0.9, 0.18, (v1 - v0) / 2, 0.9, (190, 170, 140, 255))
            for zz in (0.1, 0.52, 0.94, 1.36):
                obox("metal", uu + side * 0.15, (v0 + v1) / 2, zf + zz - 0.02, 0.07, (v1 - v0) / 2, 0.015, (176, 156, 128, 255))
                _goods(geos, at, rng, uu + side * 0.16, side, v0 + 0.05, v1 - 0.05, zf + zz)
            owall(uu, v0, uu, v1, 1.8)
        obox("metal_dark", (u0 + u1) / 2, v1 - 0.5, zf + 0.5, min(1.4, (u1 - u0) / 3), 0.3, 0.5, (100, 80, 60, 255))
        owall((u0 + u1) / 2 - 1.4, v1 - 0.8, (u0 + u1) / 2 + 1.4, v1 - 0.8, 1.0)
    # ceiling lights
    for uu in np.arange(u0 + 1.0, u1 - 0.5, 2.6):
        for vv in np.arange(0.9, v1, 2.6):
            obox("lamp", uu, vv, zc - 0.03, 0.6, 0.12, 0.02, (255, 255, 250, 255))
    # (the lamp source at mid height: from just under the ceiling it would burn a hot spot into it)
    c = at((u0 + u1) / 2, (v0 + v1) / 2, zf + 1.5)
    ex.lights.append(((c[0], c[1], c[2]), max(6.0, math.hypot(u1 - u0, v1) * 0.6), 3))


# packaging colours (generic: no brands or labels)
_PACK = [(222, 72, 56), (52, 120, 206), (238, 196, 58), (84, 168, 86), (236, 236, 228), (240, 140, 50),
         (150, 90, 170), (40, 46, 60), (200, 60, 110), (120, 190, 220), (170, 120, 70), (250, 220, 170)]


def _goods(geos, at, rng, uf, side, va, vb, z):
    """A shelf of goods facing `side` (+/-u) at u = uf from v = va to vb, standing on height z: packs
    of random width, height and colour (their fronts and tops)."""
    from .specials import _dquad
    v = va
    while v < vb - 0.08:
        w = float(min(rng.uniform(0.07, 0.3), vb - v))
        h = float(rng.uniform(0.12, 0.34))
        c = _PACK[int(rng.integers(len(_PACK)))]
        c = (*(min(255, int(k * rng.uniform(0.85, 1.05))) for k in c), 255)
        dep = 0.24
        ub = uf - side * dep
        A, B = at(uf, v + 0.005, z), at(uf, v + w - 0.005, z)
        if side < 0:  # (fronts face the aisle: +u for side +1)
            A, B = B, A
        _dquad(geos, "sign", [A, B, B + [0, 0, h], A + [0, 0, h]], c)
        T0, T1 = at(uf, v + 0.005, z + h), at(uf, v + w - 0.005, z + h)
        T2, T3 = at(ub, v + w - 0.005, z + h), at(ub, v + 0.005, z + h)
        tq = [T0, T1, T2, T3] if side > 0 else [T3, T2, T1, T0]
        _dquad(geos, "sign", tq, (int(c[0] * 0.8), int(c[1] * 0.8), int(c[2] * 0.8), 255))
        v += w
