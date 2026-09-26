"""Drivable road graph for traffic, derived from the real PLATEAU carriageway.

The tran LOD2 TrafficArea polygons (車道部 1000, 車道交差部 1020) are rasterised at 0.5 m,
closed over small gaps, skeletonised, and turned into a graph: skeleton branch points become
junction nodes, the paths between them become edges with a simplified centre line and the
measured carriageway width. Lane counts and one-way rules are NOT in the data: the client
derives lanes from the measured width and treats every road as two-way (left-hand traffic).

RJROAD v1 = b"RJROAD01"
  u32 n_nodes + n * (f64 lat, f64 lon)
  u32 n_edges + n * (u32 a, u32 b, f32 width_m, u32 n_pts, n_pts * (f64 lat, f64 lon))
"""

from __future__ import annotations

import math
import struct

import numpy as np
from PIL import Image, ImageDraw

RES = 0.5  # metres per pixel


def _rdp(pts: np.ndarray, eps: float) -> np.ndarray:
    if len(pts) < 3:
        return pts
    a, b = pts[0], pts[-1]
    ab = b - a
    L = np.hypot(*ab)
    if L < 1e-9:
        d = np.hypot(*(pts - a).T)
    else:
        d = np.abs(ab[0] * (pts[:, 1] - a[1]) - ab[1] * (pts[:, 0] - a[0])) / L
    i = int(np.argmax(d))
    if d[i] > eps:
        return np.vstack([_rdp(pts[: i + 1], eps)[:-1], _rdp(pts[i:], eps)])
    return np.vstack([a, b])


def build(road_polys_local: list[np.ndarray], frame) -> tuple[bytes, dict]:
    """road_polys_local: carriageway polygons as (N, 2) arrays in the slice frame (metres)."""
    from scipy import ndimage
    from skimage.morphology import skeletonize

    allp = np.vstack(road_polys_local)
    x0, y0 = allp.min(axis=0) - 10
    x1, y1 = allp.max(axis=0) + 10
    W, H = int((x1 - x0) / RES) + 1, int((y1 - y0) / RES) + 1
    img = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(img)
    for p in road_polys_local:
        d.polygon([((x - x0) / RES, (y1 - y) / RES) for x, y in p], fill=255)
    m = np.asarray(img) > 0
    m = ndimage.binary_closing(m, structure=np.ones((5, 5)), iterations=1)
    lab, n = ndimage.label(m)
    sizes = ndimage.sum(m, lab, range(1, n + 1))
    keep = np.isin(lab, 1 + np.where(sizes * RES * RES > 400.0)[0])  # drop islands < 400 m2
    m = keep
    edt = ndimage.distance_transform_edt(m) * RES
    sk = skeletonize(m)
    nb = ndimage.convolve(sk.astype(np.uint8), np.ones((3, 3), np.uint8), mode="constant") - 1
    nb[~sk] = 0
    node_px = sk & (nb != 2)
    nlab, nn = ndimage.label(node_px, structure=np.ones((3, 3)))
    ys, xs = np.nonzero(sk)
    pix_node = {}
    for y, x in zip(ys, xs):
        if nlab[y, x]:
            pix_node[(y, x)] = nlab[y, x] - 1
    node_xy = np.array(ndimage.center_of_mass(node_px, nlab, range(1, nn + 1))) if nn else np.zeros((0, 2))
    # trace edges
    visited = set()
    edges = []
    offs = [(-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1)]
    for (y, x), a in pix_node.items():
        for dy, dx in offs:
            q = (y + dy, x + dx)
            if not (0 <= q[0] < H and 0 <= q[1] < W) or not sk[q] or q in pix_node:
                continue
            if (a, q) in visited:
                continue
            path = [(y, x), q]
            prev, cur = (y, x), q
            end = None
            while True:
                nxt = None
                for ddy, ddx in offs:
                    r = (cur[0] + ddy, cur[1] + ddx)
                    if r == prev or not (0 <= r[0] < H and 0 <= r[1] < W) or not sk[r]:
                        continue
                    if r in pix_node and pix_node[r] != a or (r in pix_node and len(path) > 3):
                        end = pix_node[r]
                        path.append(r)
                        break
                    if r not in pix_node and r not in path[-3:]:
                        nxt = r
                if end is not None or nxt is None:
                    break
                prev, cur = cur, nxt
                path.append(cur)
                if len(path) > 20000:
                    break
            if end is None:
                continue
            visited.add((a, path[1]))
            visited.add((end, path[-2]))
            pts = np.array([(x0 + px * RES, y1 - py * RES) for py, px in path])
            width = float(np.median([edt[py, px] for py, px in path])) * 2.0
            edges.append([a, end, pts, width])
    # prune short dead-end spurs and merge degree-2 chains
    for _ in range(3):
        deg = np.zeros(len(node_xy), int)
        for a, b, *_ in edges:
            deg[a] += 1
            deg[b] += 1
        edges = [e for e in edges if not ((deg[e[0]] == 1 or deg[e[1]] == 1) and _length(e[2]) < 14.0) and e[0] != e[1]]
    # de-duplicate (a,b) pairs traced from both ends
    seen, uniq = set(), []
    for a, b, pts, w in edges:
        key = (min(a, b), max(a, b), round(float(pts[len(pts) // 2][0]), 0), round(float(pts[len(pts) // 2][1]), 0))
        if key in seen:
            continue
        seen.add(key)
        uniq.append([a, b, _rdp(pts, 0.8), w])
    used = sorted({a for a, *_ in uniq} | {b for _, b, *_ in uniq})
    remap = {old: i for i, old in enumerate(used)}
    nodes_local = []
    for old in used:
        cy, cx = node_xy[old]
        nodes_local.append((x0 + cx * RES, y1 - cy * RES))
    out = bytearray(b"RJROAD01")
    out += struct.pack("<I", len(nodes_local))
    for x, y in nodes_local:
        la, lo = frame.to_geodetic(x, y)
        out += struct.pack("<2d", la, lo)
    out += struct.pack("<I", len(uniq))
    total = 0.0
    for a, b, pts, w in uniq:
        # snap the ends onto the junction centres
        pts = pts.copy()
        pts[0] = nodes_local[remap[a]]
        pts[-1] = nodes_local[remap[b]]
        out += struct.pack("<2IfI", remap[a], remap[b], w, len(pts))
        for x, y in pts:
            la, lo = frame.to_geodetic(x, y)
            out += struct.pack("<2d", la, lo)
        total += _length(pts)
    return bytes(out), {"nodes": len(nodes_local), "edges": len(uniq), "km": round(total / 1000.0, 2)}


def _length(pts: np.ndarray) -> float:
    return float(np.sum(np.hypot(*np.diff(pts, axis=0).T))) if len(pts) > 1 else 0.0
