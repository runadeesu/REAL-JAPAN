"""Walk-in stations of the fictional country (generic designs; no real station is reproduced).

Every station has its concourse at street level under the elevated platforms: entrances in both
long walls, a free passage, a line of IC ticket gates and, in the paid area, a staircase up to each
of the two side platforms. The stairs rise through an opening in the platform that is railed on
its far end (the stair parapets rail its sides). Walkable surfaces go into the cell's deck
triangles and collision walls into its WALL section; the client opens the gate flaps as the player
walks up to them and charges the fare at the exit gate.

Station frame: u along the platform heading, v to the right of it (metres from the line centre).
"""
from __future__ import annotations

import math

import numpy as np

CU0, CU1, CV = -26.0, 26.0, 12.8               # concourse box
GATE_U = -10.0                                  # the line of ticket gates (free side: u < GATE_U)
GATE_V = (-3.0, -1.8, -0.6, 0.6, 1.8, 3.0)      # gate cabinets; the five lanes run between them
GATE_LANES = tuple((a + b) / 2 for a, b in zip(GATE_V[:-1], GATE_V[1:]))
ENTRY_U = (-21.0, -15.0)                        # entrance openings in both long walls
ENTRY_H = 3.0
STAIR_U0, STAIR_HW = -6.0, 1.4                  # stair foot, half width
PLATFORM_W = 5.0


def platform_offset(shink: bool) -> float:
    return 7.4 if shink else 6.6


def concourse_height(ground: float, ztop: float) -> float:
    return max(3.2, min(4.6, ztop - ground - 1.6))


def stair(ground: float, ztop: float):
    """(u foot, u head, slope) of the staircase from the concourse floor to the platform top."""
    rise = ztop - ground
    run = min(max(rise / 0.58, 4.0), CU1 - 1.5 - STAIR_U0)
    return STAIR_U0, STAIR_U0 + run, rise / run


def hole(ground: float, ztop: float):
    """u range of the opening in the platform over the stair (headroom under the deck edge)."""
    u0, u1, slope = stair(ground, ztop)
    return max(u0, u1 - max(3.5, 2.9 / slope)), u1


class Frame:
    def __init__(self, x, y, hd_deg):
        th = math.radians(hd_deg)
        self.o = np.array([x, y], float)
        self.f = np.array([math.sin(th), math.cos(th)])
        self.r = np.array([math.cos(th), -math.sin(th)])

    def xy(self, u, v):
        return self.o + self.f * u + self.r * v

    def p(self, u, v, z):
        q = self.xy(u, v)
        return [q[0], q[1], z]


def _quad(geos, mat, xf, pts, col):
    """One face (the renderer draws detail meshes double-sided, flipping the normal to the viewer)."""
    from .specials import _dquad
    _dquad(geos, mat, xf.p(pts), col)


def _obox(geos, mat, xf, F: Frame, u, v, z, hu, hv, hz, col):
    """Box aligned with the station frame: centre (u, v, z), half sizes along u, v, z."""
    c = [[su, sv, sz] for sz in (-1, 1) for sv in (-1, 1) for su in (-1, 1)]
    P = np.array([F.p(u + su * hu, v + sv * hv, z + sz * hz) for su, sv, sz in c])
    V = xf.p(P)
    from .specials import _dquad
    faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    for a, b, cc, d in faces:
        _dquad(geos, mat, [V[a], V[b], V[cc], V[d]], col)


def _wall(ex, xf, F: Frame, u0, v0, u1, v1, zlo, zhi):
    A = xf.p([F.p(u0, v0, zlo)])[0]
    B = xf.p([F.p(u1, v1, zlo)])[0]
    ex.walls.append((A[0], A[1], B[0], B[1], zlo + (A[2] - zlo), zhi + (A[2] - zlo)))


def _deck(ex, xf, pts):
    V = xf.p(pts)
    ex.decks.append(np.array([[V[0], V[1], V[2]], [V[0], V[2], V[3]]]))


def build(geos, ex, xf, st: dict, terrain) -> None:
    """Concourse, gates, ticket machines, stairs and lights of one station (station frame from st)."""
    F = Frame(st["x"], st["y"], st["hd"])
    g, ztop, off = st["ground"], st["ztop"], st["off"]
    H = concourse_height(g, ztop)
    zf, zc = g + 0.02, g + H
    floor_c = (176, 174, 168, 255)
    wall_in = (222, 219, 212, 255)
    wall_out = (196, 196, 192, 255)
    ceil_c = (232, 232, 228, 255)
    metal = (196, 198, 202, 255)

    # floor (walkable) and ceiling (with openings for the two stairwells)
    _quad(geos, "sidewalk", xf, [F.p(CU0, -CV, zf), F.p(CU1, -CV, zf), F.p(CU1, CV, zf), F.p(CU0, CV, zf)], floor_c)
    _deck(ex, xf, [F.p(CU0, -CV, zf), F.p(CU1, -CV, zf), F.p(CU1, CV, zf), F.p(CU0, CV, zf)])
    us0, us1, slope = stair(g, ztop)
    uc = us0 + max(0.0, (H - 2.4)) / slope  # ceiling opening from here to the stair head
    bands = [(-CV, -off - STAIR_HW), (-off + STAIR_HW, off - STAIR_HW), (off + STAIR_HW, CV)]
    _quad(geos, "concrete", xf, [F.p(CU0, CV, zc), F.p(uc, CV, zc), F.p(uc, -CV, zc), F.p(CU0, -CV, zc)], ceil_c)
    _quad(geos, "concrete", xf, [F.p(max(us1, uc), CV, zc), F.p(CU1, CV, zc), F.p(CU1, -CV, zc), F.p(max(us1, uc), -CV, zc)], ceil_c)
    for va, vb in bands:
        if vb > va:
            _quad(geos, "concrete", xf, [F.p(uc, vb, zc), F.p(max(us1, uc), vb, zc), F.p(max(us1, uc), va, zc), F.p(uc, va, zc)], ceil_c)
    # light panels in rows (lit diffusers) and lamps for the renderer
    for u in np.arange(CU0 + 3.0, CU1 - 1.0, 4.0):
        for v in (-8.0, 0.0, 8.0):
            if any(abs(v - s * off) < STAIR_HW + 0.3 for s in (-1, 1)) and uc <= u <= us1:
                continue
            _quad(geos, "lamp", xf, [F.p(u - 1.2, v + 0.15, zc - 0.03), F.p(u + 1.2, v + 0.15, zc - 0.03),
                                     F.p(u + 1.2, v - 0.15, zc - 0.03), F.p(u - 1.2, v - 0.15, zc - 0.03)], (255, 255, 248, 255))
    for u in (-18.0, -4.0, 12.0):
        for v in (-7.0, 7.0):
            P = xf.p([F.p(u, v, zc - 0.3)])[0]
            ex.lights.append(((P[0], P[1], P[2]), 11.0, 3))

    # walls: inner faces (with the entrance openings), outer faces, lintels over the entrances
    def wall_line(u0, v0, u1, v1, gaps, inward):
        """Wall from (u0, v0) to (u1, v1) with openings [(t0, t1)] along it (metres from the start)."""
        L = math.hypot(u1 - u0, v1 - v0)
        du, dv = (u1 - u0) / L, (v1 - v0) / L
        nu, nv = -dv * (1 if inward else -1), du * (1 if inward else -1)  # outward normal (u, v)
        cuts, t = [], 0.0
        for a, b in sorted(gaps):
            cuts.append((t, a))
            t = b
        cuts.append((t, L))
        for a, b in cuts:
            if b - a < 0.05:
                continue
            A0, A1 = (u0 + du * a, v0 + dv * a), (u0 + du * b, v0 + dv * b)
            pts = [F.p(*A0, g - 0.3), F.p(*A1, g - 0.3), F.p(*A1, zc), F.p(*A0, zc)]
            _quad(geos, "concrete", xf, pts, wall_out)
            _quad(geos, "concrete", xf, [F.p(A0[0] - nu * 0.15, A0[1] - nv * 0.15, z) for z in (g - 0.3,)] +
                  [F.p(A1[0] - nu * 0.15, A1[1] - nv * 0.15, g - 0.3), F.p(A1[0] - nu * 0.15, A1[1] - nv * 0.15, zc),
                   F.p(A0[0] - nu * 0.15, A0[1] - nv * 0.15, zc)], wall_in)
            _wall(ex, xf, F, *A0, *A1, g - 0.5, zc + 0.5)
        for a, b in sorted(gaps):  # lintel and a station sign over each entrance
            A0, A1 = (u0 + du * a, v0 + dv * a), (u0 + du * b, v0 + dv * b)
            _quad(geos, "concrete", xf, [F.p(*A0, g + ENTRY_H), F.p(*A1, g + ENTRY_H), F.p(*A1, zc), F.p(*A0, zc)], wall_out)
            _quad(geos, "concrete", xf, [F.p(A0[0] - nu * 0.15, A0[1] - nv * 0.15, g + ENTRY_H), F.p(A1[0] - nu * 0.15, A1[1] - nv * 0.15, g + ENTRY_H),
                                         F.p(A1[0] - nu * 0.15, A1[1] - nv * 0.15, zc), F.p(A0[0] - nu * 0.15, A0[1] - nv * 0.15, zc)], wall_in)
            _quad(geos, "concrete", xf, [F.p(A0[0] - nu * 0.15, A0[1] - nv * 0.15, g + ENTRY_H), F.p(A1[0] - nu * 0.15, A1[1] - nv * 0.15, g + ENTRY_H),
                                         F.p(*A1, g + ENTRY_H), F.p(*A0, g + ENTRY_H)], wall_in)  # soffit
            mu, mv = (A0[0] + A1[0]) / 2, (A0[1] + A1[1]) / 2
            _obox(geos, "sign", xf, F, mu + nu * 0.08, mv + nv * 0.08, g + ENTRY_H + 0.55, abs(du) * 2.2 + abs(nu) * 0.04,
                  abs(dv) * 2.2 + abs(nv) * 0.04, 0.35, (30, 60, 120, 255))
    e0, e1 = ENTRY_U
    wall_line(CU0, CV, CU1, CV, [(e0 - CU0, e1 - CU0)], True)      # right long wall (v = +CV)
    wall_line(CU1, -CV, CU0, -CV, [(CU1 - e1, CU1 - e0)], True)    # left long wall (v = -CV)
    wall_line(CU1, CV, CU1, -CV, [], True)                          # far end
    wall_line(CU0, -CV, CU0, CV, [], True)                          # near end
    # roof beside the viaduct, and the walls closing the space between the concourse and the platforms
    pe = off + PLATFORM_W / 2
    for sv in (-1, 1):
        _quad(geos, "concrete", xf, [F.p(CU0, sv * CV, zc), F.p(CU1, sv * CV, zc), F.p(CU1, sv * pe, zc), F.p(CU0, sv * pe, zc)],
              (150, 150, 146, 255))
        _quad(geos, "concrete", xf, [F.p(CU0, sv * pe, zc), F.p(CU1, sv * pe, zc), F.p(CU1, sv * pe, ztop - 0.35), F.p(CU0, sv * pe, ztop - 0.35)],
              wall_out)
        for u in np.arange(CU0 + 1.0, CU1, 2.0):
            if e0 - 0.5 < u < e1 + 0.5:
                continue
            _obox(geos, "glass", xf, F, u, sv * (CV + 0.02), g + H - 0.7, 0.85, 0.03, 0.45, (60, 80, 96, 255))
    for ue in (CU0, CU1):
        _quad(geos, "concrete", xf, [F.p(ue, -pe, zc), F.p(ue, pe, zc), F.p(ue, pe, ztop - 0.35), F.p(ue, -pe, ztop - 0.35)], wall_out)
    # entrance aprons: ramps from the concourse floor down (or up) to the street
    for sv in (-1, 1):
        a, b = (e0, e1)
        outer = [F.xy(a, sv * (CV + 3.0)), F.xy(b, sv * (CV + 3.0))]
        zo = [float(terrain.sample(*q)) + 0.02 for q in outer]
        pts = [F.p(a, sv * CV, zf), F.p(b, sv * CV, zf), [*outer[1], zo[1]], [*outer[0], zo[0]]]
        if sv < 0:
            pts = [pts[1], pts[0], pts[3], pts[2]]
        _quad(geos, "sidewalk", xf, pts, floor_c)
        _deck(ex, xf, pts)

    # ticket gates: cabinets with card readers, and glass partitions to the side walls
    for v in GATE_V:
        _obox(geos, "metal", xf, F, GATE_U, v, g + 0.5, 0.8, 0.09, 0.5, metal)
        _obox(geos, "metal_dark", xf, F, GATE_U, v, g + 1.02, 0.8, 0.1, 0.02, (40, 44, 50, 255))
        for su in (-1, 1):  # IC readers on the lane sides, facing the approach
            _obox(geos, "lamp", xf, F, GATE_U + su * 0.55, v + 0.1, g + 1.05, 0.12, 0.02, 0.012, (80, 150, 255, 255))
        _wall(ex, xf, F, GATE_U - 0.8, v, GATE_U + 0.8, v, g - 0.5, g + 1.05)
    for sv in (-1, 1):
        v0, v1 = sv * (GATE_V[-1] + 0.1), sv * CV
        pts = [F.p(GATE_U, v0, g), F.p(GATE_U, v1, g), F.p(GATE_U, v1, zc), F.p(GATE_U, v0, zc)]
        _quad(geos, "glass", xf, pts, (150, 170, 180, 255))
        _obox(geos, "metal", xf, F, GATE_U, (v0 + v1) / 2, g + 1.0, 0.05, abs(v1 - v0) / 2, 0.03, metal)
        _wall(ex, xf, F, GATE_U, v0, GATE_U, v1, g - 0.5, zc + 0.5)
    # ticket machines and a fare chart on the free side of the partition
    for v in (5.0, 6.1, 7.2, 8.3):
        _obox(geos, "metal", xf, F, GATE_U - 0.45, v, g + 0.85, 0.35, 0.45, 0.85, (224, 226, 230, 255))
        _obox(geos, "lamp", xf, F, GATE_U - 0.81, v, g + 1.25, 0.01, 0.3, 0.22, (120, 170, 230, 255))
    _wall(ex, xf, F, GATE_U - 0.85, 4.5, GATE_U - 0.85, 8.8, g - 0.5, g + 1.8)
    _obox(geos, "sign", xf, F, GATE_U - 0.12, 6.65, g + 2.45, 0.02, 2.0, 0.5, (240, 240, 236, 255))

    # stairs up to both platforms
    step_c, side_c = (190, 188, 182, 255), (206, 204, 198, 255)
    run = us1 - us0
    n = max(2, int(run / 0.3))
    for sg in (-1, 1):
        vc = sg * off
        va, vb = vc - STAIR_HW, vc + STAIR_HW
        zr = lambda u: g + (u - us0) * slope  # noqa: E731
        for i in range(n):
            ua, ub = us0 + run * i / n, us0 + run * (i + 1) / n
            za, zb = zr(ua), zr(ub)
            _quad(geos, "concrete", xf, [F.p(ua, va, za), F.p(ua, vb, za), F.p(ua, vb, zb), F.p(ua, va, zb)], step_c)  # riser
            _quad(geos, "concrete", xf, [F.p(ua, va, zb), F.p(ua, vb, zb), F.p(ub, vb, zb), F.p(ub, va, zb)], step_c)  # tread
        _deck(ex, xf, [F.p(us0, va, g + 0.02), F.p(us1, va, ztop), F.p(us1, vb, ztop), F.p(us0, vb, g + 0.02)])
        for s, v in ((-1, va), (1, vb)):
            # parapet / enclosure: floor to 1.0 m above the treads, both faces, and a handrail
            pts = [F.p(us0, v, g - 0.3), F.p(us1, v, g - 0.3), F.p(us1, v, ztop + 1.0), F.p(us0, v, g + 1.0)]
            _quad(geos, "concrete", xf, pts, side_c)
            _wall(ex, xf, F, us0, v, us1, v, g - 0.5, ztop + 1.1)
            P0 = F.p(us0, v - s * 0.08, g + 0.85)
            P1 = F.p(us1, v - s * 0.08, ztop + 0.85)
            A, B = xf.p([P0, P1])
            d = B - A
            L = float(np.linalg.norm(d))
            from .specials import _dquad
            up = np.array([0, 0, 0.03])
            _dquad(geos, "metal", [A - up, B - up, B + up, A + up], metal)
            del L
        # the space under the stair head, closed off from the concourse
        pts = [F.p(us1, va, g - 0.3), F.p(us1, vb, g - 0.3), F.p(us1, vb, ztop - 0.35), F.p(us1, va, ztop - 0.35)]
        _quad(geos, "concrete", xf, pts, side_c)
        _wall(ex, xf, F, us1, va, us1, vb, g - 0.5, ztop - 0.3)
        # stairwell above the concourse ceiling: end wall under the platform deck
        if uc < us1:
            pts = [F.p(uc, va, zc), F.p(uc, vb, zc), F.p(uc, vb, ztop - 0.4), F.p(uc, va, ztop - 0.4)]
            _quad(geos, "concrete", xf, pts, side_c)


def platform_items(geos, ex, xf, st: dict, C, Nr, u, sg: int, o_in: float, o_out: float, length: float, strip):
    """Walkable platform deck with the stair opening, its railing, and the platform's collision walls
    (track edge, outer fence, ends). C, Nr, u: the platform's centre line, right normals and distance
    along it; strip(o0, o1, z, mat, lo, hi, walk): band drawer of the caller."""
    g, ztop, off = st["ground"], st["ztop"], st["off"]
    h0, h1 = hole(g, ztop)
    lo_v, hi_v = min(o_in, o_out), max(o_in, o_out)
    hv0, hv1 = sg * off - STAIR_HW, sg * off + STAIR_HW
    strip(lo_v, hi_v, ztop, "sidewalk", -1e9, h0, True)
    strip(lo_v, hi_v, ztop, "sidewalk", h1, 1e9, True)
    strip(lo_v, min(hv0, hv1), ztop, "sidewalk", h0, h1, True)
    strip(max(hv0, hv1), hi_v, ztop, "sidewalk", h0, h1, True)
    from .specials import _dquad

    def at(uu, o, z):
        a = int(np.clip(np.searchsorted(u, uu) - 1, 0, len(C) - 2))
        t = (uu - u[a]) / max(u[a + 1] - u[a], 1e-9)
        c = C[a] * (1 - t) + C[a + 1] * t
        n = Nr[a] * (1 - t) + Nr[a + 1] * t
        n = n / max(np.linalg.norm(n), 1e-9)
        q = c + n * o
        return [q[0], q[1], z]

    # railing across the far end of the opening (its sides are the stair parapets)
    A, B = xf.p([at(h0, hv0, ztop), at(h0, hv1, ztop)])
    up = np.array([0, 0, 1.1])
    _dquad(geos, "fence", [A, B, B + up, A + up], (200, 202, 206, 255))
    ex.walls.append((A[0], A[1], B[0], B[1], A[2] - 0.25, A[2] + 1.1))
    # a roof over the stair head
    P = xf.p([at(h0 - 0.5, hv0 - 0.3, ztop + 2.6), at(h1 + 0.5, hv0 - 0.3, ztop + 2.6), at(h1 + 0.5, hv1 + 0.3, ztop + 2.6),
              at(h0 - 0.5, hv1 + 0.3, ztop + 2.6)])
    _dquad(geos, "metal_dark", P, (120, 124, 130, 255))
    # collision: track edge (the client lets the player through an open train door), outer fence, ends
    edge, outer = (o_in, o_out) if abs(o_in) < abs(o_out) else (o_out, o_in)
    step = 4.0
    us = np.arange(-length / 2, length / 2 + 1e-6, step)
    for a, b in zip(us[:-1], us[1:]):
        E0, E1 = xf.p([at(a, edge, ztop), at(b, edge, ztop)])
        ex.walls.append((E0[0], E0[1], E1[0], E1[1], E0[2] - 0.3, E0[2] + 2.4))
        O0, O1 = xf.p([at(a, outer, ztop), at(b, outer, ztop)])
        ex.walls.append((O0[0], O0[1], O1[0], O1[1], O0[2] - 0.3, O0[2] + 1.2))
        _dquad(geos, "fence", [O0, O1, O1 + np.array([0, 0, 1.1]), O0 + np.array([0, 0, 1.1])], (190, 192, 196, 255))
    for e in (-length / 2, length / 2):
        E0, E1 = xf.p([at(e, edge, ztop), at(e, outer, ztop)])
        ex.walls.append((E0[0], E0[1], E1[0], E1[1], E0[2] - 0.3, E0[2] + 1.2))
        _dquad(geos, "fence", [E0, E1, E1 + np.array([0, 0, 1.1]), E0 + np.array([0, 0, 1.1])], (190, 192, 196, 255))


def gate_record(st: dict, fi) -> str:
    """rail.txt line: gate row centre (lat lon), floor height, heading (u axis) and lane offsets."""
    F = Frame(st["x"], st["y"], st["hd"])
    q = F.xy(GATE_U, 0.0)
    la, lo = fi.to_geodetic(float(q[0]), float(q[1]))
    lanes = " ".join(f"{v:.2f}" for v in GATE_LANES)
    return f"gate {st['index']} {la:.8f} {lo:.8f} {st['ground'] + 0.02:.2f} {st['hd']:.2f} {lanes}"
