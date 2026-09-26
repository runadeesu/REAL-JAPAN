"""Verified interiors from PLATEAU LOD4 underground buildings (uro:UndergroundBuilding).

The LOD4 model is an official public source for the interior (floors, walls, ceilings,
doors, stair ramps), so the result is labelled VERIFIED interior. It contains no shop
names or fit-out; none are invented.

RJINT v2 = b"RJINT002" + u32 body_len + raw-DEFLATE(body)
body:
  b"RJINT002"
  u32 len + utf8 id, u32 len + utf8 name
  f64 anchor lat, lon, h                  (local ENU frame of all coordinates below)
  u32 interior_status (0 VERIFIED), u32 source_index
  u32 n_chunks + chunks (same encoding as RJCELL v3 chunks)
  u32 n_floor_tris + f32[9n]              walkable surfaces (FloorSurface incl. stair ramps)
  u32 n_wall_tris  + f32[9n]              blocking surfaces (walls, interior walls, windows, fixtures)
  u32 n_entrances  + n * (f32 street[3], f32 inside[3])
  u32 n_opening_tris + f32[9n]            street-level openings (ClosureSurface lids of stairwells)
"""

from __future__ import annotations

import math
import struct
import xml.etree.ElementTree as ET
import zlib
from dataclasses import dataclass, field

import numpy as np

from .citygml import NS, _parse_poslist, q
from .rjcell import CellWriter

BLDG = "{http://www.opengis.net/citygml/building/2.0}"
POLYGON = "{http://www.opengis.net/gml}Polygon"
KINDS = {"FloorSurface": "floor", "InteriorWallSurface": "iwall", "WallSurface": "wall", "CeilingSurface": "ceiling",
         "Door": "door", "Window": "window", "ClosureSurface": "closure", "RoofSurface": "roof", "GroundSurface": "ground"}
COLORS = {"floor": (178, 172, 162), "iwall": (216, 212, 204), "wall": (198, 194, 188), "ceiling": (234, 234, 230),
          "door": (118, 126, 136), "window": (150, 170, 186), "installation": (164, 162, 156)}
RENDER = {"floor", "iwall", "wall", "ceiling", "door", "window", "installation"}
BLOCKING = {"iwall", "wall", "window", "installation"}


@dataclass
class Underground:
    gml_id: str
    name: str = ""
    polys: list[tuple[str, list]] = field(default_factory=list)  # (kind, exterior ring of (lat, lon, h))
    holes: list[list] = field(default_factory=list)  # interior rings per polygon (parallel to polys)


def parse_underground(path: str) -> list[Underground]:
    out = []
    for ev, el in ET.iterparse(path, events=("end",)):
        if el.tag != q("uro:UndergroundBuilding"):
            continue
        u = Underground(gml_id=el.get(q("gml:id"), ""))
        n = el.find("gml:name", NS)
        u.name = n.text.strip() if n is not None and n.text else ""
        # Each polygon belongs to its innermost semantic surface: a Door nested in a WallSurface
        # (bldg:opening) is a door, not wall, so doorways stay passable.
        def walk(node, kind):
            tag = node.tag
            if tag.startswith(BLDG):
                local = tag[len(BLDG):]
                k = "installation" if local == "IntBuildingInstallation" else KINDS.get(local)
                if k is not None:
                    kind = k
            if tag == POLYGON:
                if kind is not None:
                    add(kind, node)
                return
            for ch in node:
                walk(ch, kind)

        def add(kind, poly):
            ext = poly.find("gml:exterior/gml:LinearRing/gml:posList", NS)
            ring = _parse_poslist(ext.text if ext is not None else None)
            if len(ring) < 3:
                return
            holes = []
            for it in poly.findall("gml:interior/gml:LinearRing/gml:posList", NS):
                h = _parse_poslist(it.text)
                if len(h) >= 3:
                    holes.append(h)
            u.polys.append((kind, ring))
            u.holes.append(holes)

        walk(el, None)
        out.append(u)
        el.clear()
    return out


def _cluster(points: list[tuple[float, float, float]], radius_m: float) -> list[tuple[float, float, float]]:
    groups: list[list[tuple[float, float, float]]] = []
    for p in points:
        for g in groups:
            q0 = g[0]
            if math.hypot((p[0] - q0[0]) * 110950, (p[1] - q0[1]) * 90400) < radius_m:
                g.append(p)
                break
        else:
            groups.append([p])
    return [tuple(float(np.mean([x[i] for x in g])) for i in range(3)) for g in groups]


def street_openings(u: Underground, dem) -> list[list]:
    """Horizontal ClosureSurfaces lying on the street surface: the lids of stairwells, i.e. the
    real openings where the underground space meets the pavement."""
    out = []
    for kind, ring in u.polys:
        if kind != "closure":
            continue
        zs = [p[2] for p in ring]
        la = sum(p[0] for p in ring) / len(ring)
        lo = sum(p[1] for p in ring) / len(ring)
        h = dem.height(la, lo)
        if not math.isnan(h) and max(zs) - min(zs) < 0.5 and abs(max(zs) - h) < 1.0:
            out.append(ring)
    return out


def opening_outlines(openings: list[list], join_m: float = 0.3, grow_m: float = 0.6) -> list[list[tuple[float, float]]]:
    """Group touching opening polygons (the data splits one stairwell lid into many thin triangles)
    and return each group's convex hull, grown by `grow_m`, as (lat, lon) rings. Used to keep
    pedestrians from walking across stairwells."""
    if not openings:
        return []
    lat0 = sum(p[0] for r in openings for p in r) / sum(len(r) for r in openings)
    kx, ky = 111320.0 * math.cos(math.radians(lat0)), 110574.0
    pts = [np.array([[p[1] * kx, p[0] * ky] for p in r]) for r in openings]
    parent = list(range(len(pts)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    for i in range(len(pts)):
        for j in range(i + 1, len(pts)):
            d = np.min(np.hypot(pts[i][:, None, 0] - pts[j][None, :, 0], pts[i][:, None, 1] - pts[j][None, :, 1]))
            if d < join_m:
                parent[find(i)] = find(j)
    groups: dict[int, list] = {}
    for i in range(len(pts)):
        groups.setdefault(find(i), []).append(pts[i])
    out = []
    for g in groups.values():
        P = sorted(map(tuple, np.vstack(g)))

        def cross(o, a, b):
            return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

        lower, upper = [], []
        for q_ in P:
            while len(lower) >= 2 and cross(lower[-2], lower[-1], q_) <= 0:
                lower.pop()
            lower.append(q_)
        for q_ in reversed(P):
            while len(upper) >= 2 and cross(upper[-2], upper[-1], q_) <= 0:
                upper.pop()
            upper.append(q_)
        hull = np.array(lower[:-1] + upper[:-1])
        if len(hull) < 3:
            continue
        c = hull.mean(axis=0)
        d = hull - c
        n = np.hypot(d[:, 0], d[:, 1])[:, None]
        hull = c + d * (1.0 + grow_m / np.maximum(n, 1e-6))
        out.append([(y / ky, x / kx) for x, y in hull])
    return out


def build_interior(u: Underground, frame, dem, triangulate, to_local, source_index: int) -> bytes:
    """Triangulate, classify and package one underground building."""
    w = CellWriter("interior", (frame.lat, frame.lon, frame.h), (0, 0, 0, 0))
    floor_tris, wall_tris = [], []
    P, N, C, I, base = [], [], [], [], 0
    seen = set()
    openings = street_openings(u, dem)
    opening_tris = []
    opening_pts = []
    for ring in openings:
        la = sum(p[0] for p in ring) / len(ring)
        lo = sum(p[1] for p in ring) / len(ring)
        opening_pts.append((la, lo, dem.height(la, lo)))
        v, t, _ = triangulate(to_local(frame, ring), [])
        if len(t):
            opening_tris.append(v[t])
    for (kind, ring), holes in zip(u.polys, u.holes):
        key = (kind, tuple(round(c, 7) for p in ring[:3] for c in p))
        if key in seen:  # rooms and the building can reference the same surface
            continue
        seen.add(key)
        if kind == "closure":
            continue
        if kind in ("roof", "ground"):
            continue
        v, t, n = triangulate(to_local(frame, ring), [to_local(frame, h) for h in holes])
        if len(t) == 0:
            continue
        tri = v[t]  # (m, 3, 3)
        if kind == "floor" and n[2] > 0.5:
            floor_tris.append(tri)
        elif kind in BLOCKING and abs(n[2]) < 0.6:
            wall_tris.append(tri)
        if kind in RENDER:
            if base + len(v) > CellWriter.MAX_VERTS:
                w.add_geometry(np.vstack(P), np.vstack(N), np.vstack(C), np.concatenate(I))
                P, N, C, I, base = [], [], [], [], 0
            P.append(v)
            N.append(np.repeat(n[None, :], len(v), axis=0))
            C.append(np.repeat(np.array([COLORS[kind] + (255,)], dtype=np.uint8), len(v), axis=0))
            I.append(t.reshape(-1) + base)
            base += len(v)
    if P:
        w.add_geometry(np.vstack(P), np.vstack(N), np.vstack(C), np.concatenate(I))
    floors = np.concatenate(floor_tris).astype(np.float32) if floor_tris else np.zeros((0, 3, 3), np.float32)
    walls = np.concatenate(wall_tris).astype(np.float32) if wall_tris else np.zeros((0, 3, 3), np.float32)

    # Entrances: street-level openings of stairways, paired with the nearest floor just below.
    entrances = []
    fc = floors.mean(axis=1) if len(floors) else np.zeros((0, 3))
    for la, lo, h in _cluster(opening_pts, 10.0):
        s = np.array(frame.to_local(la, lo, h))
        if not len(fc):
            break
        dz = fc[:, 2] - s[2]
        d = np.hypot(fc[:, 0] - s[0], fc[:, 1] - s[1])
        ok = (dz < -0.3) & (dz > -4.5) & (d < 14.0)
        if not ok.any():
            continue
        j = int(np.argmin(np.where(ok, d - dz * 0.2, 1e9)))
        entrances.append((s, fc[j], (la, lo, h)))

    body = bytearray(b"RJINT002")
    for sstr in (u.gml_id, u.name):
        b = sstr.encode("utf-8")
        body += struct.pack("<I", len(b)) + b
    body += struct.pack("<3d", frame.lat, frame.lon, frame.h)
    body += struct.pack("<2I", 0, source_index)
    # Reuse the RJCELL v3 chunk encoder by serialising a throwaway cell and slicing its chunk block.
    chunks = [c for c in w.chunks if c.nv > 0]
    body += struct.pack("<I", len(chunks))
    for c in chunks:
        pos = np.concatenate(c.pos).astype(np.float64)
        scale = max(float(np.abs(pos).max()) / 32767.0, 1e-4)
        body += struct.pack("<2Iif", c.nv, c.ni, -1, scale)
        qp = np.clip(np.round(pos / scale), -32767, 32767).astype(np.int16).tobytes()
        body += qp + b"\0" * ((4 - len(qp) % 4) % 4)
        nrm = np.concatenate(c.nrm).astype(np.float64)
        qn = np.zeros((len(nrm), 4), dtype=np.int8)
        qn[:, :3] = np.clip(np.round(nrm * 127.0), -127, 127).astype(np.int8)
        body += qn.tobytes()
        body += np.concatenate(c.col).astype(np.uint8).tobytes()
        body += np.zeros((c.nv, 2), np.uint16).tobytes()
        idx = np.concatenate(c.idx).tobytes()
        body += idx + b"\0" * ((4 - len(idx) % 4) % 4)
    body += struct.pack("<I", len(floors)) + floors.tobytes()
    body += struct.pack("<I", len(walls)) + walls.tobytes()
    body += struct.pack("<I", len(entrances))
    for s, i, _ in entrances:
        body += struct.pack("<6f", *s, *i)
    otris = np.concatenate(opening_tris).astype(np.float32) if opening_tris else np.zeros((0, 3, 3), np.float32)
    body += struct.pack("<I", len(otris)) + otris.tobytes()
    comp = zlib.compressobj(9, zlib.DEFLATED, -15)
    data = comp.compress(bytes(body)) + comp.flush()
    stats = {"floors": len(floors), "walls": len(walls), "entrances": len(entrances), "openings": len(otris),
             "chunks": len(chunks), "vertices": int(sum(c.nv for c in chunks))}
    return b"RJINT002" + struct.pack("<I", len(body)) + data, stats, entrances
