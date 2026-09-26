"""Triangulation of planar 3D polygons (with holes) via earcut on the best-fit plane."""

from __future__ import annotations

import math

import mapbox_earcut as earcut
import numpy as np


def newell_normal(pts: np.ndarray) -> np.ndarray:
    n = np.zeros(3)
    for i in range(len(pts)):
        a, b = pts[i], pts[(i + 1) % len(pts)]
        n[0] += (a[1] - b[1]) * (a[2] + b[2])
        n[1] += (a[2] - b[2]) * (a[0] + b[0])
        n[2] += (a[0] - b[0]) * (a[1] + b[1])
    ln = np.linalg.norm(n)
    return n / ln if ln > 1e-12 else n


def triangulate(exterior: np.ndarray, holes: list[np.ndarray]) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Returns (vertices Nx3, triangle indices Mx3, face normal). Empty arrays when degenerate."""
    n = newell_normal(exterior)
    if not np.any(n):
        return np.zeros((0, 3)), np.zeros((0, 3), dtype=np.uint32), n
    # Orthonormal basis of the polygon plane.
    ref = np.array([0.0, 0.0, 1.0]) if abs(n[2]) < 0.9 else np.array([1.0, 0.0, 0.0])
    u = np.cross(ref, n)
    u /= np.linalg.norm(u)
    v = np.cross(n, u)
    rings = [exterior] + holes
    verts = np.vstack(rings)
    uv = np.stack([verts @ u, verts @ v], axis=1)
    ends = np.cumsum([len(r) for r in rings]).astype(np.uint32)
    idx = earcut.triangulate_float64(uv, ends)
    if len(idx) == 0:
        return np.zeros((0, 3)), np.zeros((0, 3), dtype=np.uint32), n
    tris = np.asarray(idx, dtype=np.uint32).reshape(-1, 3)
    # Earcut output winding follows the 2D projection; make triangles face +n.
    a, b, c = verts[tris[:, 0]], verts[tris[:, 1]], verts[tris[:, 2]]
    face = np.cross(b - a, c - a)
    flip = (face @ n) < 0
    tris[flip] = tris[flip][:, [0, 2, 1]]
    return verts, tris, n


def polygon_area_2d(pts: list[tuple[float, float]]) -> float:
    s = 0.0
    for i in range(len(pts)):
        x1, y1 = pts[i]
        x2, y2 = pts[(i + 1) % len(pts)]
        s += x1 * y2 - x2 * y1
    return 0.5 * s


def is_finite(a: np.ndarray) -> bool:
    return bool(np.all(np.isfinite(a))) and not math.isnan(float(a.sum()))
