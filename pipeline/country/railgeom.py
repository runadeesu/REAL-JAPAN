"""Centre lines of the country's rail lines (the smoothed curves the tracks and trains follow), the
stations snapped onto them, and the lines' vertical profiles.

The layout gives each station a nominal position and heading; the smoothed line generally does not
pass exactly through it. Everything built for a station (platforms, canopies, concourse, the station
record the client reads, the reserved land around it) uses the snapped position, the local track
heading and, for platforms, the curve itself.

Vertical profile (designed, generic): the rail level follows the ground at a structure height (a
viaduct in the cities, lower in the countryside) but may not climb or fall faster than the line's
maximum grade, so it bridges narrow valleys and tunnels through ridges; stations are level.
"""
from __future__ import annotations

import math

import numpy as np
from scipy.ndimage import gaussian_filter1d
from shapely.geometry import LineString

from . import layout as L

LINE_KINDS = tuple(ln["kind"] for ln in L.RAIL_LINES)
PLATFORM_BY_KIND = {"loop": 200.0, "branch": 200.0, "shinkansen": 320.0}
PLATFORM_LEN = {i: PLATFORM_BY_KIND[k] for i, k in enumerate(LINE_KINDS)}
MAX_GRADE = {"loop": 0.030, "branch": 0.025, "shinkansen": 0.025}


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
    """(N, 2) centre lines, ~6 m point spacing, one per RAIL_LINES entry (closed lines repeat their first point)."""
    out = []
    for ln in L.RAIL_LINES:
        P = resample(chaikin(ln["pts"], 4, ln["closed"]), 6.0)
        if ln["closed"]:
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
    for li, ln in enumerate(L.RAIL_LINES):
        for name, x, y, hd in ln["stations"]:
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


def _lipschitz(z, g, pinned, lb):
    """Project z onto profiles with |dz| <= g per step, keeping pinned points and staying above the
    lower bound lb where the grade allows (a few sweeps each way)."""
    z = z.copy()
    for _ in range(3):
        for k in range(1, len(z)):
            if not pinned[k]:
                z[k] = min(max(z[k], z[k - 1] - g, lb[k]), z[k - 1] + g)
        for k in range(len(z) - 2, -1, -1):
            if not pinned[k]:
                z[k] = min(max(z[k], z[k + 1] - g, lb[k]), z[k + 1] + g)
    return z


def rail_profile(P, kind, closed, terrain, station_ks, platform_len):
    """Rail level (m above sea) for every point of centre line P (6 m spacing)."""
    gz = np.maximum(terrain.sample(P[:, 0], P[:, 1]), 2.0)
    deck = np.array([L.rail_deck(kind, x, y) for x, y in P[:, :2]])
    target = gaussian_filter1d(gz, 8, mode="wrap" if closed else "nearest") + gaussian_filter1d(deck, 20, mode="nearest")
    step = 6.0
    g = MAX_GRADE[kind] * step
    # the highest profile under the target with the grade limit (tunnels under ridges) ...
    z = target.copy()
    for _ in range(2):
        for k in range(1, len(z)):
            z[k] = min(z[k], z[k - 1] + g)
        for k in range(len(z) - 2, -1, -1):
            z[k] = min(z[k], z[k + 1] + g)
    # ... smoothed into long even grades (bridges over dips) ...
    z = gaussian_filter1d(z, 40, mode="wrap" if closed else "nearest")
    # in the towns the viaduct clears the streets (no level crossings)
    lb = np.where(np.array([L.urban(x, y) for x, y in P[:, :2]]), gz + 6.0, -1e9)
    z = np.maximum(z, lb)
    # ... with level track through the stations
    pinned = np.zeros(len(z), bool)
    half = int((platform_len / 2 + 60.0) / step)
    for k in station_ks:
        lo, hi = max(0, k - half), min(len(z), k + half + 1)
        z[lo:hi] = max(z[k], lb[k])
        pinned[lo:hi] = True
    z = _lipschitz(z, g, pinned, lb)
    return z
