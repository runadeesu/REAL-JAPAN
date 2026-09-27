#!/usr/bin/env python3
"""Cook 千景島 (a FICTIONAL island inspired by Tokyo) into the client's world format.

Usage: python3 pipeline/cook_island.py [--out game/data/world/island] [--seed N]

Reuses the real-data pipeline's writers (RJCELL cells, RJDET street detail with raised sidewalks
and kerbs, RJROAD road graph) so the client's traffic, pedestrians, signals, markings, lights,
weather and night rendering work on the island unchanged. Buildings use procedural facades
(RJCELL page -2). Nothing here depicts a real place, shop or person.
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import math
import os
import struct
import sys
import time

import numpy as np
import shapely
from PIL import Image, ImageDraw, ImageFilter
from shapely.geometry import LineString, MultiLineString, Point, Polygon
from shapely.ops import unary_union
from shapely.strtree import STRtree

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from island import layout as L  # noqa: E402
from island.buildings import make_building  # noqa: E402
from island.generate import generate, parts  # noqa: E402
from island import specials  # noqa: E402
from realjapan_pipeline import PIPELINE_VERSION  # noqa: E402
from realjapan_pipeline.geodesy import LocalFrame, Mesh, meshes3_covering  # noqa: E402
from realjapan_pipeline.rjcell import GEOM_UNVERIFIED, INTERIOR_FICTIONAL, BuildingRec, CellWriter  # noqa: E402
from realjapan_pipeline.streetdetail import Geo as DGeo  # noqa: E402
from realjapan_pipeline.streetdetail import TerrainSampler, build_sidewalks  # noqa: E402
from realjapan_pipeline.streetdetail import encode as encode_detail  # noqa: E402
import realjapan_pipeline.streetdetail as streetdetail  # noqa: E402

# The island's terrain is gentle and sampled every ~8.6 m: coarser sidewalk / kerb tessellation keeps
# the cells small without visible difference.
streetdetail.MAX_EDGE = 7.0
streetdetail.CURB_SEG = 5.0

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEX = 2048
COL = {"yard": (150, 144, 132), "road": (70, 70, 74), "sidewalk": (188, 182, 170), "median": (96, 124, 70),
       "park": (88, 128, 62), "forest": (46, 70, 38), "sand": (198, 182, 140), "grass": (104, 136, 70),
       "apron": (168, 168, 164), "runway": (58, 58, 62), "mark": (232, 232, 226), "bed": (92, 86, 74),
       "footprint": (84, 82, 76), "plaza": (196, 188, 172), "field": (112, 128, 70)}


def frame_mats(f: LocalFrame):
    return np.array(f._m), np.array(f._o)  # rows: E, N, U in ECEF


def rigid(fi: LocalFrame, fc: LocalFrame):
    Mi, oi = frame_mats(fi)
    Mc, oc = frame_mats(fc)
    return Mc @ Mi.T, Mc @ (oi - oc)


class Xf:
    """Island-local -> cell-local rigid transform (+ affine island-local -> lat/lon for rasters)."""

    def __init__(self, fi: LocalFrame, fc: LocalFrame):
        self.R, self.t = rigid(fi, fc)

    def p(self, P):
        P = np.asarray(P, float)
        if P.shape[-1] == 2:
            P = np.column_stack([P, np.zeros(len(P))])
        return P @ self.R.T + self.t

    def n(self, N):
        return np.asarray(N, float) @ self.R.T


class RingPoly:
    """Duck-typed stand-in for citygml.Polygon (rings in island-local x, y, 0)."""

    def __init__(self, poly: Polygon, kind: str):
        self.exterior = [(x, y, 0.0) for x, y in poly.exterior.coords]
        self.interiors = [[(x, y, 0.0) for x, y in h.coords] for h in poly.interiors]
        self.kind = kind


class RoadStub:
    def __init__(self):
        self.detail = []
        self.lod1 = []


def raster_mapper(fi: LocalFrame, bounds):
    """Affine island-local (x, y) -> ground-texture pixel (north up), fitted at the cell corners."""
    lat0, lon0, lat1, lon1 = bounds
    pts = [(lat0, lon0), (lat0, lon1), (lat1, lon0), (lat1, lon1)]
    A, B = [], []
    for la, lo in pts:
        x, y, _ = fi.to_local(la, lo, 0.0)
        A.append([x, y, 1.0])
        B.append([(lo - lon0) / (lon1 - lon0), (lat1 - la) / (lat1 - lat0)])
    M, *_ = np.linalg.lstsq(np.array(A), np.array(B), rcond=None)
    return M  # (3, 2)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "game", "data", "world", "island"))
    ap.add_argument("--seed", type=int, default=20260927)
    ap.add_argument("--terrain-n", type=int, default=129)
    a = ap.parse_args()
    out = a.out
    os.makedirs(os.path.join(out, "cells"), exist_ok=True)
    t0 = time.time()
    isl = generate(a.seed)
    print(f"layout: {len(isl.net.roads)} roads, {len(isl.parcels)} parcels ({time.time() - t0:.0f}s)", flush=True)
    rng = np.random.default_rng(a.seed + 1)
    fi = LocalFrame(L.ORIGIN[0], L.ORIGIN[1], 0.0)
    terrain = isl.terrain

    # ---- buildings ----
    sx, sy = L.SCRAMBLE
    from island.railgeom import stations_aligned
    stations = [st for st in stations_aligned() if st[4] < 2]  # loop and branch stations, snapped to the track
    st_xy = np.array([(x, y) for _, x, y, _, _, _ in stations])
    blds = []
    for pc in isl.parcels:
        c = pc.poly.centroid
        ns = math.hypot(c.x - sx, c.y - sy)
        nst = float(np.min(np.hypot(st_xy[:, 0] - c.x, st_xy[:, 1] - c.y)))
        b = make_building(pc, terrain, rng, ns, nst)
        if b is not None:
            blds.append(b)
    spec = specials.build_all(isl, terrain, rng)
    blds.extend(spec.buildings)
    print(f"buildings: {len(blds)} ({time.time() - t0:.0f}s)", flush=True)

    # ---- cells covering the island ----
    x0, y0, x1, y1 = -4300, -3500, 4300, 3800
    corners = [fi.to_geodetic(x, y) for x, y in ((x0, y0), (x1, y0), (x0, y1), (x1, y1))]
    lats, lons = [c[0] for c in corners], [c[1] for c in corners]
    meshes = meshes3_covering(min(lats), min(lons), max(lats), max(lons))
    land_all = isl.land.union(isl.islet)
    cell_poly = {}
    for m in meshes:
        la0, lo0, la1, lo1 = Mesh(m).bounds()
        ring = [fi.to_local(la, lo, 0.0)[:2] for la, lo in ((la0, lo0), (la0, lo1), (la1, lo1), (la1, lo0))]
        P = Polygon(ring)
        if P.intersects(land_all.buffer(60)) or P.intersects(unary_union([LineString(r) for r in L.FERRY_ROUTES]).buffer(1)):
            cell_poly[m] = P
    print(f"cells: {len(cell_poly)} of {len(meshes)} ({time.time() - t0:.0f}s)", flush=True)

    # building -> cell by footprint centroid
    cell_tree_keys = list(cell_poly)
    cell_tree = STRtree([cell_poly[k] for k in cell_tree_keys])
    by_cell: dict[str, list] = {k: [] for k in cell_poly}
    for b in blds:
        cx, cy = b.footprint.mean(axis=0)
        hit = cell_tree.query(Point(cx, cy), predicate="intersects")
        if len(hit):
            by_cell[cell_tree_keys[hit[0]]].append(b)

    summary, pois = [], []
    homes, works = [], []
    for name, x, y, _, _, _ in stations:
        la, lo = fi.to_geodetic(x, y)
        pois.append({"name": name, "lat": la, "lon": lo, "usage": 431, "height": 18.0, "id": f"chikage-{name}"})
    for key, (name, x, y) in L.LANDMARKS.items():
        la, lo = fi.to_geodetic(x, y)
        pois.append({"name": name, "lat": la, "lon": lo, "usage": 454, "height": 0.0, "id": f"chikage-{key}"})
    la, lo = fi.to_geodetic(*L.TERMINAL)
    pois.append({"name": "千景空港", "lat": la, "lon": lo, "usage": 431, "height": 20.0, "id": "chikage-airport"})
    la, lo = fi.to_geodetic(*L.FERRY_TERMINAL)
    pois.append({"name": "千景港フェリーターミナル", "lat": la, "lon": lo, "usage": 431, "height": 12.0, "id": "chikage-ferry"})

    carr_tree_src = isl.net.carriage
    for mesh, cpoly in cell_poly.items():
        tc = time.time()
        mm = Mesh(mesh)
        bounds = mm.bounds()
        clat, clon = mm.center()
        ccx, ccy, _ = fi.to_local(clat, clon, 0.0)
        anchor_h = round(float(max(0.0, terrain.sample(ccx, ccy))), 2)
        fc = LocalFrame(clat, clon, anchor_h)
        xf = Xf(fi, fc)
        w = CellWriter(mesh, (clat, clon, anchor_h), bounds)
        # buildings
        for b in by_cell[mesh]:
            first = None
            for page, geo in ((-2, b.geo), (-1, b.gear)):
                arr = geo.arrays()
                if arr is None:
                    continue
                pos, nrm, col, uv, idx = arr
                if len(pos) > CellWriter.MAX_VERTS:
                    continue
                r = w.add_geometry(xf.p(pos), xf.n(nrm), col, idx, uv, page)
                first = first or r
            if first is None:
                continue
            ch, fidx, cnt = first
            fp_local = xf.p(b.footprint)[:, :2]
            allp = xf.p(np.vstack([g.arrays()[0] for g in (b.geo, b.gear) if g.arrays() is not None]))
            gz = float(xf.p([[b.footprint[0][0], b.footprint[0][1], b.ground]])[0, 2])
            bid = f"CK-{mesh}-{len(w.buildings):05d}"
            rec = BuildingRec(id=bid, name=b.name, usage=b.usage, bclass=0, measured_height=float(b.height),
                              storeys_above=int(b.storeys), storeys_below=0, lod=2, geometry_status=GEOM_UNVERIFIED,
                              interior_status=INTERIOR_FICTIONAL, source_index=0, ground_z=gz,
                              bmin=tuple(allp.min(0)), bmax=tuple(allp.max(0)), footprint=[tuple(p) for p in fp_local],
                              chunk=ch, first_index=fidx, index_count=cnt)
            w.buildings.append(rec)
            cx, cy = b.footprint.mean(axis=0)
            bla, blo = fi.to_geodetic(cx, cy)
            area = Polygon(b.footprint).area
            entry = {"id": bid, "lat": bla, "lon": blo, "usage": b.usage, "storeys": max(1, b.storeys), "area": area}
            if b.usage in (411, 412, 413, 414, 415):
                homes.append(entry)
            elif b.usage in (401, 402, 403, 404, 421, 422, 431, 441):
                works.append(entry)
            if b.name:
                pois.append({"name": b.name, "lat": bla, "lon": blo, "usage": b.usage, "height": float(b.height), "id": bid})

        # terrain grid
        n = a.terrain_n
        lat0, lon0, lat1, lon1 = bounds
        dlat, dlon = (lat1 - lat0) / (n - 1), (lon1 - lon0) / (n - 1)
        G = np.zeros((n, n, 3))
        for i in range(n):
            for j in range(n):
                G[i, j, :2] = fi.to_local(lat0 + i * dlat, lon0 + j * dlon, 0.0)[:2]
        G[..., 2] = terrain.sample(G[..., 0], G[..., 1])
        hz = xf.p(G.reshape(-1, 3))[:, 2].reshape(n, n)
        w.set_terrain(hz, lat0, lon0, dlat, dlon)
        ts = TerrainSampler(hz, lat0, lon0, dlat, dlon, fc)

        # roads in this cell (clipped) -> raised sidewalks + kerbs
        clip = cpoly.buffer(3)
        stub = RoadStub()
        for kind, geom in (("road", isl.net.carriage), ("sidewalk", isl.net.sidewalk), ("median", isl.net.median)):
            for p in parts(geom.intersection(clip)):
                if p.area > 0.5:
                    stub.detail.append(RingPoly(p, kind))
        to_local = lambda frame, ring: xf.p(np.array([(x, y, 0.0) for x, y, *_ in ring]))  # noqa: E731
        geos, walk = {}, []
        fps_local = [rec.footprint for rec in w.buildings]
        build_sidewalks([stub], fps_local, fc, to_local, ts, geos, walk)
        extra = specials.cell_detail(spec, isl, cpoly, xf, ts, rng, geos)
        # ground raster + AO
        w.ground_png, ao_png = ground_raster(isl, spec, cpoly, raster_mapper(fi, bounds), by_cell[mesh])
        det, dst = encode_detail(geos, extra.decks, [], [], [], ao_png, extra.trees, [])
        with open(os.path.join(out, "cells", f"{mesh}.rjdet"), "wb") as f:
            f.write(det)
        data = w.to_bytes()
        with open(os.path.join(out, "cells", f"{mesh}.rjcell"), "wb") as f:
            f.write(data)
        summary.append({"mesh": mesh, "anchor": [clat, clon, anchor_h], "buildings": len(w.buildings), "bytes": len(data)})
        print(f"{mesh}: {len(w.buildings)} buildings, {len(data) / 1e6:.1f} MB + detail {len(det) / 1e6:.1f} MB "
              f"({dst['trees']} trees), {time.time() - tc:.0f}s", flush=True)

    # ---- road graph straight from the centerlines ----
    rdata, nn, ne = road_graph(isl, fi)
    with open(os.path.join(out, "roads.rjroad"), "wb") as f:
        f.write(rdata)
    print(f"road graph: {nn} nodes, {ne} edges", flush=True)
    specials.write_extra(spec, out, fi)

    # ---- residents (fictional) ----
    write_residents(out, homes, works, pois, rng)
    sp_lat, sp_lon = fi.to_geodetic(-265, -700)
    meta = {"id": "island", "name_ja": L.NAME_JA, "name_en": L.NAME_EN, "cells": summary, "pipeline_version": PIPELINE_VERSION,
            "fictional": True}
    with open(os.path.join(out, "slice.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=1)
    with open(os.path.join(out, "pois.json"), "w", encoding="utf-8") as f:
        json.dump(pois, f, ensure_ascii=False, indent=1)
    with open(os.path.join(out, "client.txt"), "w", encoding="utf-8") as f:
        f.write(f"# generated by cook_island.py {PIPELINE_VERSION}\n")
        f.write(f"name_ja {L.NAME_JA}\nname_en {L.NAME_EN}\n")
        f.write("world fictional\n")
        f.write(f"spawn {sp_lat} {sp_lon} 225\n")
        f.write(f"core_bbox {min(lats)} {min(lons)} {max(lats)} {max(lons)}\n")
        for s in summary:
            f.write(f"cell {s['mesh']} {s['anchor'][0]:.10f} {s['anchor'][1]:.10f} {s['anchor'][2]:.3f}\n")
        f.write("source 0 FICTIONAL 千景島は東京をモチーフにした架空の島です。実在の場所・店・人物とは関係ありません。\n")
        for p in pois:
            f.write(f"poi {p['lat']:.7f} {p['lon']:.7f} {p['usage']} {p['height']:.1f} {p['name']}\n")
    print(f"done in {time.time() - t0:.0f}s -> {out}")
    return 0


def ground_raster(isl, spec, cpoly: Polygon, M, blds) -> tuple[bytes, bytes]:
    ss = 2
    W = TEX * ss
    img = Image.new("RGB", (W, W), COL["yard"])
    d = ImageDraw.Draw(img)

    def px(coords):
        c = np.asarray(coords)[:, :2]
        uv = np.column_stack([c, np.ones(len(c))]) @ M
        return [(float(u) * W, float(v) * W) for u, v in uv]

    clip = cpoly.buffer(20)

    def fill(geom, color):
        for p in parts(geom.intersection(clip) if geom is not None else None):
            d.polygon(px(p.exterior.coords), fill=color)
            for h in p.interiors:
                d.polygon(px(h.coords), fill=COL["yard"])

    fill(spec.forest, COL["forest"])
    fill(spec.grass, COL["grass"])
    fill(spec.fields, COL["field"])
    fill(unary_union([Polygon(pp) for _, pp in L.PARKS]), COL["park"])
    fill(Polygon(L.BEACH).buffer(60).difference(isl.land.buffer(-60)), COL["sand"])
    fill(spec.apron, COL["apron"])
    fill(spec.runway, COL["runway"])
    fill(isl.river.poly, COL["bed"])
    fill(isl.net.carriage, COL["road"])
    fill(isl.net.sidewalk, COL["sidewalk"])
    fill(isl.net.median, COL["median"])
    fill(isl.net.plaza, COL["plaza"])
    fill(spec.marks, COL["mark"])
    fpm = Image.new("L", (W, W), 0)
    fd = ImageDraw.Draw(fpm)
    for b in blds:
        pts = px(b.footprint)
        d.polygon(pts, fill=COL["footprint"])
        fd.polygon(pts, fill=255)
    ao = np.asarray(fpm.filter(ImageFilter.GaussianBlur(radius=5)), np.float32) / 255.0
    shade = 1.0 - 0.45 * np.clip(ao * 1.6, 0.0, 1.0)
    shade[np.asarray(fpm) > 0] = 1.0
    ao_img = Image.fromarray(np.clip(shade * 255.0, 0, 255).astype(np.uint8), "L").resize((TEX // 2, TEX // 2), Image.BILINEAR)
    abuf = io.BytesIO()
    ao_img.save(abuf, format="PNG", optimize=True)
    img = img.resize((TEX, TEX), Image.LANCZOS).quantize(colors=63, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    buf = io.BytesIO()
    img.save(buf, format="PNG", optimize=True)
    return buf.getvalue(), abuf.getvalue()


def road_graph(isl, fi: LocalFrame):
    """RJROAD from the generated centerlines (noded at crossings); width = carriageway width."""
    lines = []
    widths = []
    for r in isl.net.roads:
        if r.carriage < 3.0:
            continue
        lines.append(r.line)
        widths.append(r.carriage)
    noded = unary_union(MultiLineString(lines))  # splits at every crossing
    segs = [g for g in getattr(noded, "geoms", [noded]) if isinstance(g, LineString) and g.length > 0.5]
    tree = STRtree(lines)
    nodes, key = [], {}

    def node(pt):
        k = (round(pt[0], 1), round(pt[1], 1))
        if k not in key:
            key[k] = len(nodes)
            nodes.append(pt)
        return key[k]

    edges = []
    for s in segs:
        c = np.asarray(s.coords)
        mid = s.interpolate(0.5, normalized=True)
        cand = tree.query(mid.buffer(0.5))
        wv = max((widths[i] for i in cand if lines[i].distance(mid) < 0.5), default=6.0)
        edges.append((node(tuple(c[0])), node(tuple(c[-1])), float(wv), c))
    # merge degree-2 chains so edges run junction to junction
    deg = np.zeros(len(nodes), int)
    for a_, b_, _, _ in edges:
        deg[a_] += 1
        deg[b_] += 1
    out = bytearray(b"RJROAD01")
    out += struct.pack("<I", len(nodes))
    for x, y in nodes:
        la, lo = fi.to_geodetic(x, y)
        out += struct.pack("<2d", la, lo)
    out += struct.pack("<I", len(edges))
    for a_, b_, wv, c in edges:
        out += struct.pack("<2IfI", a_, b_, wv, len(c))
        for x, y in c:
            la, lo = fi.to_geodetic(x, y)
            out += struct.pack("<2d", la, lo)
    return bytes(out), len(nodes), len(edges)


def write_residents(out, homes, works, pois, rng):
    rows = []
    if homes and works:
        hw = np.array([h["storeys"] * max(h["area"], 30.0) for h in homes])
        ww = np.array([w["storeys"] * max(w["area"], 30.0) for w in works])
        occ = {401: ["office_worker"] * 6 + ["engineer", "programmer", "designer", "executive", "lawyer"],
               402: ["shop_clerk"] * 5 + ["cook", "cook", "hairdresser", "barber", "patissier"],
               403: ["shop_clerk", "cook"], 404: ["shop_clerk", "office_worker", "cook"], 421: ["civil_servant"],
               422: ["teacher", "nurse", "doctor", "university_student"], 431: ["truck_driver", "delivery_rider"],
               441: ["factory_worker", "mechanic"]}
        hi = rng.choice(len(homes), size=6000, p=hw / hw.sum())
        wi = rng.choice(len(works), size=6000, p=ww / ww.sum())
        for k in range(6000):
            h, wk = homes[hi[k]], works[wi[k]]
            roll = rng.random()
            if roll < 0.12:
                o, w2 = rng.choice(["high_school_student", "university_student", "junior_high_student"]), wk
            elif roll < 0.25:
                o, w2 = "retired", None
            else:
                o, w2 = rng.choice(occ.get(wk["usage"], ["office_worker"])), wk
            rows.append([k + 1, o, h["id"], f"{h['lat']:.7f}", f"{h['lon']:.7f}", w2["id"] if w2 else "",
                         f"{w2['lat']:.7f}" if w2 else "", f"{w2['lon']:.7f}" if w2 else "", "resident"])
    with open(os.path.join(out, "residents.csv"), "w", newline="", encoding="utf-8") as f:
        cw = csv.writer(f)
        cw.writerow(["id", "occupation", "home_id", "home_lat", "home_lon", "work_id", "work_lat", "work_lon", "kind"])
        cw.writerows(rows)


if __name__ == "__main__":
    sys.exit(main())
