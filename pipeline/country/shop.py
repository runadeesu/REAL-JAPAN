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
from shapely.geometry import Point, Polygon
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
    # (mostly small shops; a convenience store now and then)
    r = rng.random()
    kind = ("konbini" if r < 0.14 else "cafe" if r < 0.45 else "general") if fp.area > 60 else "general"
    fascia = FASCIA[kind][int(rng.integers(len(FASCIA[kind])))]
    return dict(ring=ring, ground=ground, front_edge=bi, kind=kind, fascia=fascia, seed=int(rng.integers(1 << 30)))


def build(geos, ex, xf, shop, ts=None):
    """Builds the shop floor; returns where its counter is (country frame: kind, counter x/y, the
    customer's spot x/y, floor height), or None when the room is too small to fit out."""
    from .specials import _box_d, _dquad
    ring, g = shop["ring"], shop["ground"]
    n = len(ring)
    rng = np.random.default_rng(shop["seed"])
    zf, zc = g + 0.06, g + 3.0

    def ground_at(x, y):  # the rendered terrain's height here (ts: the cell's grid), country frame
        if ts is None:
            return g
        c = xf.p([[x, y, g]])[0]
        return g + float(ts(c[0], c[1])) - float(c[2])

    if ts is not None:
        # the building stands at the lowest ground under it: lift the floor clear of the terrain
        # under the rest of the footprint (the step at the door climbs it)
        fpp = Polygon(ring)
        x0_, y0_, x1_, y1_ = fpp.bounds
        pts_ = [tuple(q) for q in ring] + [(x, y) for x in np.arange(x0_ + 1.0, x1_, 2.0) for y in np.arange(y0_ + 1.0, y1_, 2.0)
                                           if fpp.contains(Point(x, y))]
        lift = max(ground_at(x, y) - g for x, y in pts_)
        zf = g + float(np.clip(lift + 0.05, 0.06, 0.5))
        zc = min(g + FLOOR_H - 0.02, zf + 2.94)
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
    home = shop["kind"] == "home"
    if home:
        wall_in = (236, 230, 214, 255)
    for t in tri:
        A, B, C = (P(verts[k][0], verts[k][1], zf) for k in t)
        if home:  # (a plain wooden floor at home)
            _dquad(geos, "concrete:in", [A, B, C, C], (164, 122, 84, 255))
        else:
            _dquad(geos, "sidewalk:in", [A, B, C, C], (196, 194, 188, 255))
        ex.decks.append(np.array([[A, B, C]]))
        A2, B2, C2 = (P(verts[k][0], verts[k][1], zc) for k in t)
        _dquad(geos, "concrete:in", [A2, C2, B2, B2], (238, 238, 234, 255))
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
        if i == fe and shop["kind"] == "home":
            door = _home_front(geos, ex, xf, quad, wall, a, b, u, nrm, L_, g, zf, wall_in, ground_at)
        elif i == fe:
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
                # see-through panes (drawn blended after the opaque scene: the lit shop shows through)
                quad("glass_clear", [(q0[0], q0[1], g + 0.40), (q1[0], q1[1], g + 0.40), (q1[0], q1[1], g + 2.72), (q0[0], q0[1], g + 2.72)],
                     (200, 215, 225, 255))
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
                quad("glass_clear", [(q0[0], q0[1], g + 0.12), (q1[0], q1[1], g + 0.12), (q1[0], q1[1], g + 2.38), (q0[0], q0[1], g + 2.38)],
                     (200, 215, 225, 255))
                quad("metal", [(q0[0], q0[1], g + 0.05), (q1[0], q1[1], g + 0.05), (q1[0], q1[1], g + 0.12), (q0[0], q0[1], g + 0.12)], (150, 154, 160, 255))
                quad("metal", [(q0[0], q0[1], g + 2.38), (q1[0], q1[1], g + 2.38), (q1[0], q1[1], g + 2.45), (q0[0], q0[1], g + 2.45)], (150, 154, 160, 255))
            m0, m1, m2, m3 = a + u * d0 - nrm * 0.2, a + u * d1 - nrm * 0.2, a + u * d1 - nrm * 1.4, a + u * d0 - nrm * 1.4
            quad("sidewalk:in", [(m[0], m[1], zf + 0.01) for m in (m0, m1, m2, m3)], (60, 62, 66, 255))
            # a low step up at the door (the floor meets the pavement)
            dq0, dq1 = a + u * d0, a + u * d1
            so = _door_step(dq0, dq1, nrm, zf, ground_at)
            _dquad(geos, "sidewalk", xf.p(so), (170, 170, 166, 255))
            ex.decks.append(xf.p([so[0], so[1], so[2]])[None])
            ex.decks.append(xf.p([so[0], so[2], so[3]])[None])
        else:
            # solid wall: outside (plaster, like the building above) and inside faces
            quad("concrete", [(a[0], a[1], g - 0.3), (b[0], b[1], g - 0.3), (b[0], b[1], g + FLOOR_H), (a[0], a[1], g + FLOOR_H)], (196, 194, 188, 255))
            quad("concrete:in", [(bi_[0], bi_[1], g), (ai[0], ai[1], g), (ai[0], ai[1], zc), (bi_[0], bi_[1], zc)], wall_in)
            wall(a[0], a[1], b[0], b[1], g - 0.3, g + FLOOR_H)
    # fit-out: the room's frame (u along the front, v inwards)
    a, b = ring[fe], ring[(fe + 1) % n]
    u = (b - a) / max(np.hypot(*(b - a)), 1e-9)
    v = np.array([-u[1], u[0]])  # inward (left of a CCW edge)
    pts = np.asarray(inner.exterior.coords)[:-1]
    us, vs = (pts - a) @ u, (pts - a) @ v
    u0, u1, v0, v1 = float(us.min()) + 0.4, float(us.max()) - 0.4, max(0.0, float(vs.min())) + 1.8, float(vs.max()) - 0.5
    if u1 - u0 < 2.5 or v1 - v0 < 1.5:
        return None
    if home:
        return _home_fitout(geos, ex, at_fn=lambda uu, vv, z: P(*(a + u * uu + v * vv), z), a=a, u=u, v=v,
                            u0=float(us.min()) + 0.12, u1=float(us.max()) - 0.12, v1=float(vs.max()) - 0.12, zf=zf, zc=zc, door=door)

    def cpt(uu, vv):  # (country frame, 2D)
        q = a + u * uu + v * vv
        return float(q[0]), float(q[1])

    def at(uu, vv, z):
        q = a + u * uu + v * vv
        return P(q[0], q[1], z)

    def obox(mat, uu, vv, z, hu, hv, hz, col):
        # a box aligned with the room (all of it indoors): centre, half sizes along u, v, z
        mat = mat + ":in"
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
        till = (cpt(cu, v0 - 0.9), cpt(cu, v0 + 0.15))
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
                        _goods(geos, at, rng, ur + side * 0.31, side, gv0 + 0.05, gv1 - 0.05, zf + zz, tops=zz > 0.9)
                owall(ur, gv0, ur, gv1, 1.4)
    elif kind == "cafe":
        cu = u1 - 1.2
        till = (cpt(cu, (v0 + v1) / 2), cpt(cu - 1.05, (v0 + v1) / 2))
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
            for zz in (0.3, 0.8, 1.3):
                obox("metal", uu + side * 0.15, (v0 + v1) / 2, zf + zz - 0.02, 0.07, (v1 - v0) / 2, 0.015, (176, 156, 128, 255))
                _goods(geos, at, rng, uu + side * 0.16, side, v0 + 0.05, v1 - 0.05, zf + zz)
            owall(uu, v0, uu, v1, 1.8)
        obox("metal_dark", (u0 + u1) / 2, v1 - 0.5, zf + 0.5, min(1.4, (u1 - u0) / 3), 0.3, 0.5, (100, 80, 60, 255))
        till = (cpt((u0 + u1) / 2, v1 - 0.5), cpt((u0 + u1) / 2, v1 - 1.4))
        owall((u0 + u1) / 2 - 1.4, v1 - 0.8, (u0 + u1) / 2 + 1.4, v1 - 0.8, 1.0)
    # ceiling lights
    for uu in np.arange(u0 + 1.0, u1 - 0.5, 2.6):
        for vv in np.arange(0.9, v1, 2.6):
            obox("lamp", uu, vv, zc - 0.03, 0.6, 0.12, 0.02, (255, 255, 250, 255))
    # (the lamp source at mid height: from just under the ceiling it would burn a hot spot into it)
    c = at((u0 + u1) / 2, (v0 + v1) / 2, zf + 1.5)
    ex.lights.append(((c[0], c[1], c[2]), max(6.0, math.hypot(u1 - u0, v1) * 0.6), 3))
    return dict(kind=kind, counter=till[0], stand=till[1], z=float(zf))


def _door_step(q0, q1, nrm, zf, ground_at):
    """The step up from the pavement to a walk-in floor across a doorway q0..q1: from the ground out
    in front (longer when the floor stands higher) to the floor at the wall line."""
    out = max(0.4, (zf - min(ground_at(*(q0 + nrm * 0.4)), ground_at(*(q1 + nrm * 0.4)))) * 2.0)
    o0, o1 = q0 + nrm * out, q1 + nrm * out
    return [[o0[0], o0[1], ground_at(*o0) + 0.01], [o1[0], o1[1], ground_at(*o1) + 0.01], [q1[0], q1[1], zf], [q0[0], q0[1], zf]]


def _home_front(geos, ex, xf, quad, wall, a, b, u, nrm, L_, g, zf, wall_in, ground_at):
    """The front of the player's flat: a plastered wall with the front door (its leaf is drawn by the
    game - shut and locked until the flat is rented) and a window, a door lamp, a name plate and a
    step up. Returns the doorway's two ends (country frame, on the wall line)."""
    from .specials import _box_d, _dquad

    def P(x, y, z):
        return xf.p([[x, y, z]])[0]

    d0 = min(1.0, max(0.35, L_ * 0.15))
    d1 = d0 + 0.95
    w0, w1 = d1 + 0.9, min(L_ - 0.6, d1 + 0.9 + 1.6)
    inn = -nrm * 0.12
    top = g + FLOOR_H

    def span(s0, s1, z0, z1):  # a piece of the wall: outside and inside faces
        q0, q1 = a + u * s0, a + u * s1
        quad("concrete", [(q0[0], q0[1], z0), (q1[0], q1[1], z0), (q1[0], q1[1], z1), (q0[0], q0[1], z1)], (206, 200, 186, 255))
        i0, i1 = q0 + inn, q1 + inn
        lo, hi = max(z0, g), min(z1, g + 3.0)
        if hi > lo:
            quad("concrete:in", [(i1[0], i1[1], lo), (i0[0], i0[1], lo), (i0[0], i0[1], hi), (i1[0], i1[1], hi)], wall_in)

    span(0.0, d0, g - 0.3, top)
    span(d0, d1, g + 2.1, top)  # over the door
    if w1 - w0 > 0.8:
        span(d1, w0, g - 0.3, top)
        span(w0, w1, g - 0.3, g + 0.9)
        span(w0, w1, g + 2.0, top)
        span(w1, L_, g - 0.3, top)
        q0, q1 = a + u * w0, a + u * w1
        quad("glass_clear", [(q0[0], q0[1], g + 0.9), (q1[0], q1[1], g + 0.9), (q1[0], q1[1], g + 2.0), (q0[0], q0[1], g + 2.0)], (200, 215, 225, 255))
        for s in (w0, (w0 + w1) / 2, w1):  # window frame
            q = a + u * s
            _box_d(geos, "metal", P(q[0], q[1], g + 1.45), (0.03, 0.03, 0.55), (170, 172, 176, 255))
        for z in (g + 0.9, g + 2.0):
            quad("metal", [(q0[0] + nrm[0] * 0.03, q0[1] + nrm[1] * 0.03, z - 0.03), (q1[0] + nrm[0] * 0.03, q1[1] + nrm[1] * 0.03, z - 0.03),
                           (q1[0] + nrm[0] * 0.03, q1[1] + nrm[1] * 0.03, z + 0.03), (q0[0] + nrm[0] * 0.03, q0[1] + nrm[1] * 0.03, z + 0.03)],
                 (170, 172, 176, 255))
    else:
        span(d1, L_, g - 0.3, top)
    qa, qb = a + u * d0, a + u * d1
    wall(a[0], a[1], qa[0], qa[1], g - 0.3, top)
    wall(qb[0], qb[1], b[0], b[1], g - 0.3, top)
    # door frame, the lamp beside it and a name plate (blank)
    for s in (d0, d1):
        q = a + u * s
        _box_d(geos, "metal_dark", P(q[0], q[1], g + 1.05), (0.05, 0.05, 1.05), (70, 60, 52, 255))
    qm = a + u * ((d0 + d1) / 2)
    quad("metal_dark", [(qa[0] + nrm[0] * 0.02, qa[1] + nrm[1] * 0.02, g + 2.1), (qb[0] + nrm[0] * 0.02, qb[1] + nrm[1] * 0.02, g + 2.1),
                        (qb[0] + nrm[0] * 0.02, qb[1] + nrm[1] * 0.02, g + 2.18), (qa[0] + nrm[0] * 0.02, qa[1] + nrm[1] * 0.02, g + 2.18)], (70, 60, 52, 255))
    ql = a + u * (d1 + 0.3) + nrm * 0.07
    _box_d(geos, "lamp", P(ql[0], ql[1], g + 2.25), (0.07, 0.07, 0.1), (255, 236, 200, 255))
    qn = a + u * max(0.1, d0 - 0.25) + nrm * 0.03
    _box_d(geos, "sign", P(qn[0], qn[1], g + 1.5), (0.09, 0.09, 0.05), (236, 232, 220, 255))
    # the entrance (genkan) inside and the step up from the pavement
    g0, g1 = qa - nrm * 0.13, qb - nrm * 0.13
    g2, g3 = qb - nrm * 1.1, qa - nrm * 1.1
    quad("sidewalk:in", [(p[0], p[1], zf + 0.01) for p in (g0, g1, g2, g3)], (110, 108, 104, 255))
    so = _door_step(qa, qb, nrm, zf, ground_at)
    _dquad(geos, "sidewalk", xf.p(so), (170, 170, 166, 255))
    ex.decks.append(xf.p([so[0], so[1], so[2]])[None])
    ex.decks.append(xf.p([so[0], so[2], so[3]])[None])
    return ((float(qa[0]), float(qa[1])), (float(qb[0]), float(qb[1])))


def _home_fitout(geos, ex, at_fn, a, u, v, u0, u1, v1, zf, zc, door):
    """The flat's one room (a 1K: fictional, generic furniture): a bed at the back, a kitchen counter
    with a sink, a hob and a fridge along the back wall, a low table with floor cushions and a rug,
    a television on a low board, a wardrobe and a round ceiling light."""
    from .specials import _dquad

    def obox(mat, uu, vv, z, hu, hv, hz, col):
        mat = mat + ":in"
        C = np.array([[su, sv, sz] for sz in (-1, 1) for sv in (-1, 1) for su in (-1, 1)], float)
        V = np.array([at_fn(uu + su * hu, vv + sv * hv, z + sz * hz) for su, sv, sz in C])
        for f in ((0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)):
            _dquad(geos, mat, [V[f[0]], V[f[1]], V[f[2]], V[f[3]]], col)

    def owall(ua, va, ub, vb, h):
        A, B = at_fn(ua, va, zf), at_fn(ub, vb, zf)
        ex.walls.append((A[0], A[1], B[0], B[1], A[2] - 0.2, A[2] + h))

    def ring_walls(uu, vv, hu, hv, h):
        for (ua, va), (ub, vb) in (((-1, -1), (1, -1)), ((1, -1), (1, 1)), ((1, 1), (-1, 1)), ((-1, 1), (-1, -1))):
            owall(uu + ua * hu, vv + va * hv, uu + ub * hu, vv + vb * hv, h)

    def cpt(uu, vv):
        q = a + u * uu + v * vv
        return float(q[0]), float(q[1])

    vf = 0.2
    dd1 = float((np.asarray(door[1]) - a) @ u)
    # bed (back corner, far from the door)
    bu, bv = u1 - 0.55, v1 - 1.05
    obox("metal_dark", bu, bv, zf + 0.17, 0.5, 1.0, 0.17, (120, 92, 66, 255))           # frame
    obox("concrete", bu, bv + 0.02, zf + 0.42, 0.47, 0.96, 0.08, (242, 242, 238, 255))  # mattress
    obox("concrete", bu, bv - 0.3, zf + 0.52, 0.49, 0.62, 0.03, (86, 118, 160, 255))    # quilt
    obox("concrete", bu, v1 - 0.28, zf + 0.56, 0.3, 0.15, 0.06, (250, 250, 246, 255))   # pillow
    obox("metal_dark", bu, v1 - 0.03, zf + 0.5, 0.5, 0.03, 0.5, (120, 92, 66, 255))     # headboard
    ring_walls(bu, bv, 0.5, 1.0, 0.6)
    # kitchen along the back wall from the side wall by the door: fridge, then the counter
    obox("metal", u0 + 0.34, v1 - 0.33, zf + 0.85, 0.32, 0.32, 0.85, (228, 230, 232, 255))
    obox("metal_dark", u0 + 0.34, v1 - 0.65, zf + 1.1, 0.01, 0.02, 0.25, (80, 82, 86, 255))  # handle
    ring_walls(u0 + 0.34, v1 - 0.33, 0.32, 0.32, 1.7)
    k0, k1 = u0 + 0.72, min(u0 + 3.0, bu - 0.5 - 0.8)
    if k1 - k0 > 1.0:
        km = (k0 + k1) / 2
        obox("metal", km, v1 - 0.3, zf + 0.42, (k1 - k0) / 2, 0.3, 0.42, (236, 234, 228, 255))       # cabinets
        obox("metal", km, v1 - 0.3, zf + 0.86, (k1 - k0) / 2 + 0.01, 0.31, 0.02, (190, 192, 196, 255))  # worktop
        obox("metal_dark", k0 + 0.45, v1 - 0.3, zf + 0.885, 0.3, 0.2, 0.005, (150, 154, 160, 255))    # sink
        obox("metal_dark", k1 - 0.35, v1 - 0.3, zf + 0.885, 0.25, 0.2, 0.01, (30, 30, 34, 255))       # hob
        obox("metal", k0 + 0.45, v1 - 0.08, zf + 1.1, 0.02, 0.02, 0.2, (190, 192, 196, 255))          # tap
        obox("metal", km, v1 - 0.18, zf + 1.95, (k1 - k0) / 2, 0.17, 0.3, (236, 234, 228, 255))       # wall cupboard
        ring_walls(km, v1 - 0.3, (k1 - k0) / 2, 0.3, 0.9)
    # wardrobe in the front corner on the bed's side (clear of the window)
    if bv - 1.0 - vf > 1.3:
        obox("metal_dark", u1 - 0.32, vf + 0.5, zf + 0.95, 0.3, 0.48, 0.95, (200, 176, 140, 255))
        ring_walls(u1 - 0.32, vf + 0.5, 0.3, 0.48, 1.9)
    # low table on a rug, two floor cushions, the television facing it
    tu = (max(u0 + 1.2, dd1 + 0.2) + bu - 0.5) / 2
    tv = (vf + 1.4 + v1 - 0.7) / 2
    _dquad(geos, "concrete:in", [at_fn(tu - 1.0, tv - 0.8, zf + 0.012), at_fn(tu + 1.0, tv - 0.8, zf + 0.012),
                                 at_fn(tu + 1.0, tv + 0.8, zf + 0.012), at_fn(tu - 1.0, tv + 0.8, zf + 0.012)], (190, 170, 130, 255))
    obox("metal_dark", tu, tv, zf + 0.33, 0.45, 0.35, 0.025, (150, 110, 74, 255))
    for su in (-1, 1):
        for sv in (-1, 1):
            obox("metal_dark", tu + su * 0.38, tv + sv * 0.28, zf + 0.155, 0.03, 0.03, 0.155, (130, 96, 64, 255))
    for sv in (-1, 1):
        obox("concrete", tu, tv + sv * 0.72, zf + 0.04, 0.27, 0.27, 0.04, (170, 60, 60, 255))
    owall(tu - 0.45, tv, tu + 0.45, tv, 0.4)
    ttv = tu - 1.4
    if ttv - u0 > 0.3:
        obox("metal_dark", max(u0 + 0.22, ttv), tv, zf + 0.2, 0.2, 0.6, 0.2, (90, 70, 54, 255))
        obox("metal_dark", max(u0 + 0.22, ttv), tv, zf + 0.72, 0.025, 0.5, 0.3, (18, 18, 20, 255))
        ring_walls(max(u0 + 0.22, ttv), tv, 0.2, 0.6, 0.9)
    # ceiling light
    lc = ((u0 + u1) / 2, (vf + v1) / 2)
    obox("lamp", lc[0], lc[1], zc - 0.06, 0.28, 0.28, 0.04, (255, 250, 238, 255))
    c = at_fn(lc[0], lc[1], zf + 1.7)
    ex.lights.append(((c[0], c[1], c[2]), max(5.0, math.hypot(u1 - u0, v1) * 0.6), 3))
    return dict(kind="home", counter=cpt(bu, bv), stand=cpt(bu - 0.95, bv), z=float(zf), door=door)


def home_ok(fp: Polygon, max_area: float = 90.0) -> bool:
    """A footprint that makes a one-room flat: about rectangular, 35 to 90 m2 (or max_area)."""
    if not (35.0 <= fp.area <= max_area):
        return False
    r = fp.minimum_rotated_rectangle
    x, y = r.exterior.coords.xy
    s = sorted([math.hypot(x[1] - x[0], y[1] - y[0]), math.hypot(x[2] - x[1], y[2] - y[1])])
    return fp.area / max(r.area, 1e-9) > 0.9 and s[0] >= 4.8


# packaging colours (generic: no brands or labels)
_PACK = [(222, 72, 56), (52, 120, 206), (238, 196, 58), (84, 168, 86), (236, 236, 228), (240, 140, 50),
         (150, 90, 170), (40, 46, 60), (200, 60, 110), (120, 190, 220), (170, 120, 70), (250, 220, 170)]


def _goods(geos, at, rng, uf, side, va, vb, z, tops=False):
    """A shelf of goods facing `side` (+/-u) at u = uf from v = va to vb, standing on height z: packs
    of random width, height and colour (their fronts; their tops too on the top shelf)."""
    from .specials import _dquad
    v = va
    while v < vb - 0.08:
        w = float(min(rng.uniform(0.14, 0.42), vb - v))
        h = float(rng.uniform(0.12, 0.34))
        c = _PACK[int(rng.integers(len(_PACK)))]
        c = (*(min(255, int(k * rng.uniform(0.85, 1.05))) for k in c), 255)
        dep = 0.24
        ub = uf - side * dep
        A, B = at(uf, v + 0.005, z), at(uf, v + w - 0.005, z)
        if side < 0:  # (fronts face the aisle: +u for side +1)
            A, B = B, A
        _dquad(geos, "sign:in", [A, B, B + [0, 0, h], A + [0, 0, h]], c)
        if tops:
            T0, T1 = at(uf, v + 0.005, z + h), at(uf, v + w - 0.005, z + h)
            T2, T3 = at(ub, v + w - 0.005, z + h), at(ub, v + 0.005, z + h)
            tq = [T0, T1, T2, T3] if side > 0 else [T3, T2, T1, T0]
            _dquad(geos, "sign:in", tq, (int(c[0] * 0.8), int(c[1] * 0.8), int(c[2] * 0.8), 255))
        v += w
