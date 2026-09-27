"""Centre lines of the island's rail lines (the smoothed curves the tracks and trains follow) and
the stations snapped onto them.

The layout gives each station a nominal position and heading; the smoothed line generally does not
pass exactly through it (by up to ~75 m where a station sits near a bend). Everything built for a
station (platforms, canopies, concourse, the station record the client reads, the reserved land
around it) uses the snapped position, the local track heading and, for platforms, the curve itself.
"""
from __future__ import annotations

import math

import numpy as np
from shapely.geometry import LineString

from . import layout as L

LINE_KINDS = ("loop", "branch", "shinkansen")
PLATFORM_LEN = {0: 200.0, 1: 200.0, 2: 320.0}  # by line index (10 x 20 m, 6 x 20 m, 8 x 25 m + margin)


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


def rail_lines2d():
    """(N, 2) centre lines, ~6 m point spacing: loop (closed, first point repeated), branch, Shinkansen."""
    out = []
    for pts, closed in ((L.RAIL_LOOP[:-1], True), (L.RAIL_BRANCH, False), (L.SHINKANSEN, False)):
        P = resample(chaikin(pts, 4, closed), 6.0)
        if closed:
            P = np.vstack([P, P[:1]])
        out.append(P)
    return out


def snap(P, x, y, hd):
    """Nearest point of polyline P to (x, y), the track heading there (compass degrees, kept within
    90 degrees of the nominal heading `hd`) and the index of the nearest vertex."""
    P = np.asarray(P, float)[:, :2]
    q0 = np.array([x, y], float)
    d = np.hypot(P[:, 0] - x, P[:, 1] - y)
    k = int(np.argmin(d))
    best, bq = d[k], P[k]
    for j in (k - 1, k):
        if j < 0 or j + 1 >= len(P):
            continue
        a, b = P[j], P[j + 1]
        ab = b - a
        l2 = float(ab @ ab)
        if l2 <= 0:
            continue
        t = float(np.clip((q0 - a) @ ab / l2, 0.0, 1.0))
        q = a + ab * t
        dq = float(np.hypot(*(q - q0)))
        if dq < best:
            best, bq = dq, q
    t = P[min(len(P) - 1, k + 2)] - P[max(0, k - 2)]
    h = math.degrees(math.atan2(t[0], t[1]))
    if abs(((h - hd + 180.0) % 360.0) - 180.0) > 90.0:
        h += 180.0
    return float(bq[0]), float(bq[1]), round(h % 360.0, 2), k


def stations_aligned(lines=None):
    """[(name, x, y, heading, line index, vertex index)] for every station, snapped to its line."""
    lines = lines if lines is not None else rail_lines2d()
    out = []
    todo = [(s, 0) for s in L.STATIONS[:4]] + [(s, 1) for s in L.STATIONS[4:]] + [(s, 2) for s in L.SHINKANSEN_STATIONS]
    for (name, x, y, hd), li in todo:
        xs, ys, h, k = snap(lines[li], x, y, hd)
        out.append((name, xs, ys, h, li, k))
    return out


def track_piece(P, k, half_len, closed=False):
    """The centre line within +-half_len (m) of vertex k, as an (M, 2) array (wraps round a loop)."""
    P = np.asarray(P, float)[:, :2]
    n = len(P) - 1 if closed else len(P)  # (a closed line repeats its first point)
    idx = [k]
    acc = 0.0
    j = k
    while acc < half_len:
        jn = (j - 1) % n if closed else j - 1
        if jn < 0:
            break
        acc += float(np.hypot(*(P[j] - P[jn])))
        idx.insert(0, jn)
        j = jn
    acc, j = 0.0, k
    while acc < half_len:
        jn = (j + 1) % n if closed else j + 1
        if jn >= n:
            break
        acc += float(np.hypot(*(P[jn] - P[j])))
        idx.append(jn)
        j = jn
    return P[idx]
