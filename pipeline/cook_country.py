#!/usr/bin/env python3
"""Cook 秋津国 (a FICTIONAL country inspired by Japan) into the client's world format.

Usage: python3 pipeline/cook_country.py [--out game/data/world/country] [--seed N] [--workers 4]

Reuses the real-data pipeline's writers (RJCELL cells, RJDET street detail with raised sidewalks
and kerbs, RJROAD road graph) so the client's traffic, pedestrians, signals, markings, lights,
weather and night rendering work unchanged. Buildings use procedural facades (RJCELL page -2).
Adds per cell a land-cover map (forest / paddy / field / bare weights) for the client's rural
materials and forest canopy, and for the whole country a coarse far-view terrain, colour map,
building boxes and a snow-potential map. Nothing here depicts a real place, shop or person.
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import math
import multiprocessing as mp
import os
import struct
import sys
import time
import zlib

import numpy as np
import shapely
from PIL import Image, ImageDraw, ImageFilter
from shapely.geometry import LineString, MultiLineString, Point, Polygon
from shapely.ops import unary_union
from shapely.strtree import STRtree

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from country import layout as L  # noqa: E402
from country import nation as N  # noqa: E402
from country.buildings import make_building  # noqa: E402
from country.generate import generate, parts  # noqa: E402
from country.landcover import LandCover  # noqa: E402
from country import specials  # noqa: E402
from realjapan_pipeline import PIPELINE_VERSION  # noqa: E402
from realjapan_pipeline.geodesy import A as GA, E2 as GE2, LocalFrame, Mesh, meshes3_covering  # noqa: E402
from realjapan_pipeline.rjcell import GEOM_UNVERIFIED, INTERIOR_FICTIONAL, BuildingRec, CellWriter  # noqa: E402
from realjapan_pipeline.streetdetail import TerrainSampler, build_sidewalks  # noqa: E402
from realjapan_pipeline.streetdetail import encode as encode_detail  # noqa: E402
import realjapan_pipeline.streetdetail as streetdetail  # noqa: E402

# The terrain is sampled every ~8.6 m: coarser sidewalk / kerb tessellation keeps the cells small
# without visible difference.
streetdetail.MAX_EDGE = 7.0
streetdetail.CURB_SEG = 5.0

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEX, TEX_RURAL, TEX_SEA = 2048, 1024, 256
COL = {"yard": (150, 144, 132), "road": (70, 70, 74), "sidewalk": (188, 182, 170), "median": (96, 124, 70),
       "park": (88, 128, 62), "forest": (46, 62, 36), "sand": (198, 182, 140), "grass": (104, 136, 70),
       "apron": (168, 168, 164), "runway": (58, 58, 62), "mark": (232, 232, 226), "bed": (92, 86, 74),
       "footprint": (84, 82, 76), "plaza": (196, 188, 172), "field": (124, 110, 76), "paddy": (92, 118, 64), "seabed": (52, 66, 62),
       "levee": (118, 134, 80), "farmroad": (150, 142, 122), "bare": (86, 66, 58), "rock": (118, 112, 104),
       "hole": (255, 0, 255)}


# ---------------------------------------------------------------------------------------------
def frame_mats(f: LocalFrame):
    return np.array(f._m), np.array(f._o)  # rows: E, N, U in ECEF


def rigid(fi: LocalFrame, fc: LocalFrame):
    Mi, oi = frame_mats(fi)
    Mc, oc = frame_mats(fc)
    return Mc @ Mi.T, Mc @ (oi - oc)


def curvature_drop(fi: LocalFrame, x, y):
    """Height of the ellipsoid below the country-local map point (x, y) in the tangent frame:
    about -(d^2 / 2R), -77 m 31 km from the origin (mm-accurate to 50 km)."""
    s = math.sin(math.radians(fi.lat))
    w = 1.0 - GE2 * s * s
    n_r, m_r = GA / math.sqrt(w), GA * (1.0 - GE2) / w ** 1.5
    return -(np.square(x) / (2.0 * n_r) + np.square(y) / (2.0 * m_r))


class Xf:
    """Country-local map coordinates (x, y on the tangent plane, z = height above the sea) ->
    cell-local: the Earth's curvature below (x, y), then the rigid transform between the frames.
    (Exports such as rail.txt give lat/lon of the tangent point plus a height, which the client
    raises along the local vertical; the two agree to about d^3 / 2R^2 + h d / R, i.e. under
    0.6 m at the country's far edge and a few centimetres within 15 km of the origin.)"""

    def __init__(self, fi: LocalFrame, fc: LocalFrame):
        self.fi = fi
        self.R, self.t = rigid(fi, fc)

    def p(self, P):
        P = np.array(P, float)
        if P.shape[-1] == 2:
            P = np.column_stack([P, np.zeros(len(P))])
        P[..., 2] += curvature_drop(self.fi, P[..., 0], P[..., 1])
        return P @ self.R.T + self.t

    def n(self, N_):
        return np.asarray(N_, float) @ self.R.T


def to_local_arr(f: LocalFrame, lat, lon, h=0.0):
    """Vectorised LocalFrame.to_local."""
    la, lo = np.radians(lat), np.radians(lon)
    s = np.sin(la)
    n = GA / np.sqrt(1.0 - GE2 * s * s)
    X = (n + h) * np.cos(la) * np.cos(lo)
    Y = (n + h) * np.cos(la) * np.sin(lo)
    Z = (n * (1 - GE2) + h) * s
    M, o = frame_mats(f)
    D = np.stack([X - o[0], Y - o[1], Z - o[2]], axis=-1)
    return D @ M.T


class RingPoly:
    """Duck-typed stand-in for citygml.Polygon (rings in country-local x, y, 0)."""

    def __init__(self, poly: Polygon, kind: str):
        self.exterior = [(x, y, 0.0) for x, y in poly.exterior.coords]
        self.interiors = [[(x, y, 0.0) for x, y in h.coords] for h in poly.interiors]
        self.kind = kind


class RoadStub:
    def __init__(self):
        self.detail = []
        self.lod1 = []


def raster_mapper(fi: LocalFrame, bounds):
    """Affine country-local (x, y) -> ground-texture uv (north up), fitted at the cell corners."""
    lat0, lon0, lat1, lon1 = bounds
    pts = [(lat0, lon0), (lat0, lon1), (lat1, lon0), (lat1, lon1)]
    A, B = [], []
    for la, lo in pts:
        x, y, _ = fi.to_local(la, lo, 0.0)
        A.append([x, y, 1.0])
        B.append([(lo - lon0) / (lon1 - lon0), (lat1 - la) / (lat1 - lat0)])
    M, *_ = np.linalg.lstsq(np.array(A), np.array(B), rcond=None)
    return M  # (3, 2)


def uv_to_xy(M, W):
    """Country-local (x, y) of every pixel centre of a W x W raster (inverse of raster_mapper)."""
    A2 = M[:2, :].T  # uv = A2 @ xy + b
    b = M[2, :]
    inv = np.linalg.inv(A2)
    u = (np.arange(W) + 0.5) / W
    U, V = np.meshgrid(u, u)
    uv = np.stack([U - b[0], V - b[1]], axis=-1)
    xy = uv @ inv.T
    return xy[..., 0], xy[..., 1]


# ---------------------------------------------------------------------------------------------
G = {}  # shared state for the worker processes (inherited through fork)


def cook_cell(mesh: str):
    ctry, spec, lc, fi, out, by_cell, a = G["ctry"], G["spec"], G["lc"], G["fi"], G["out"], G["by_cell"], G["args"]
    cpoly = G["cell_poly"][mesh]
    terrain = ctry.terrain
    rng = np.random.default_rng(zlib.crc32(mesh.encode()))  # (not hash(): salted per process)
    tc = time.time()
    mm = Mesh(mesh)
    bounds = mm.bounds()
    clat, clon = mm.center()
    ccx, ccy, _ = fi.to_local(clat, clon, 0.0)
    anchor_h = round(float(max(0.0, terrain.sample(ccx, ccy))), 2)
    fc = LocalFrame(clat, clon, anchor_h)
    xf = Xf(fi, fc)
    w = CellWriter(mesh, (clat, clon, anchor_h), bounds)
    homes, works, pois = [], [], []
    blds = list(by_cell.get(mesh, []))  # special structures
    sx, sy = L.SCRAMBLE
    st_xy = G["st_xy"]
    for pc in G["parcels_by_cell"].get(mesh, []):
        c = pc.poly.centroid
        ns = math.hypot(c.x - sx, c.y - sy)
        nst = float(np.min(np.hypot(st_xy[:, 0] - c.x, st_xy[:, 1] - c.y)))
        b = make_building(pc, terrain, rng, ns, nst)
        if b is not None:
            blds.append(b)
    boxes = far_boxes(blds)
    for b in blds:
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
        bid = f"AK-{mesh}-{len(w.buildings):05d}"
        rec = BuildingRec(id=bid, name=b.name, usage=b.usage, bclass=0, measured_height=float(b.height),
                          storeys_above=int(b.storeys), storeys_below=0, lod=2, geometry_status=GEOM_UNVERIFIED,
                          interior_status=INTERIOR_FICTIONAL, source_index=0, ground_z=gz,
                          bmin=tuple(allp.min(0)), bmax=tuple(allp.max(0)), footprint=[tuple(p) for p in fp_local],
                          chunk=ch, first_index=fidx, index_count=cnt, flags=4 if getattr(b, "walk_in", False) else 0)
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

    # terrain grid (vectorised lat/lon -> country-local)
    n = a.terrain_n
    lat0, lon0, lat1, lon1 = bounds
    dlat, dlon = (lat1 - lat0) / (n - 1), (lon1 - lon0) / (n - 1)
    LA, LO = np.meshgrid(lat0 + np.arange(n) * dlat, lon0 + np.arange(n) * dlon, indexing="ij")
    Gl = to_local_arr(fi, LA, LO)
    Gz = terrain.sample(Gl[..., 0], Gl[..., 1])
    Gp = np.stack([Gl[..., 0], Gl[..., 1], Gz], axis=-1)
    hz = xf.p(Gp.reshape(-1, 3))[:, 2].reshape(n, n)
    w.set_terrain(hz, lat0, lon0, dlat, dlon)
    ts = TerrainSampler(hz, lat0, lon0, dlat, dlon, fc)

    land_here = ctry.land.intersection(cpoly.buffer(20))
    has_land = land_here.area > 50.0
    rp = ctry.net.polys(cpoly) if has_land else None
    if rp is not None and spec.road_struct_area is not None and spec.road_struct_area.intersects(cpoly.buffer(30)):
        # no road painted (nor kerbs built) on the ground over a road tunnel or under a viaduct:
        # the carriageway there is the structure's own deck
        S_ = spec.road_struct_area
        rp = type(rp)(rp.whole.difference(S_), rp.carriage.difference(S_), rp.sidewalk.difference(S_), rp.median.difference(S_), rp.plaza)
    geos, walk = {}, []
    if rp is not None and not rp.whole.is_empty:
        clip = cpoly.buffer(3)
        stub = RoadStub()
        for kind, geom in (("road", rp.carriage), ("sidewalk", rp.sidewalk), ("median", rp.median)):
            for p in parts(geom.intersection(clip)):
                if p.area > 0.5:
                    stub.detail.append(RingPoly(p, kind))
        to_local = lambda frame, ring: xf.p(np.array([(x, y, 0.0) for x, y, *_ in ring]))  # noqa: E731
        fps_local = [rec.footprint for rec in w.buildings]
        build_sidewalks([stub], fps_local, fc, to_local, ts, geos, walk)
    extra = specials.cell_detail(spec, ctry, cpoly, xf, ts, rng, geos, rp=rp)
    from country import shop as shop_mod
    for b in blds:  # walk-in shop ground floors
        if getattr(b, "shop", None):
            shop_mod.build(geos, extra, xf, b.shop)
            if os.environ.get("RJ_COOK_SHOPS"):  # (debug: where the shops are)
                r, fe = np.asarray(b.shop["ring"], float), b.shop["front_edge"]
                a_, b_ = r[fe], r[(fe + 1) % len(r)]
                m_ = (a_[:2] + b_[:2]) / 2
                d_ = b_[:2] - a_[:2]
                n_ = np.array([d_[1], -d_[0]]) / max(float(np.hypot(*d_)), 1e-9)
                if float((m_ - r[:, :2].mean(axis=0)) @ n_) < 0:
                    n_ = -n_
                cam = m_ + n_ * 7.0  # (a spot out in front, looking at the shop: compass yaw)
                la_, lo_ = fi.to_geodetic(float(cam[0]), float(cam[1]))
                print(f"SHOP {b.shop['kind']} view {la_:.7f},{lo_:.7f} yaw {math.degrees(math.atan2(-n_[0], -n_[1])) % 360:.0f}", flush=True)
    # ground raster, contact AO, land-cover map
    M = raster_mapper(fi, bounds)
    size = TEX if (w.buildings or (rp is not None and rp.whole.area > 20000)) else (TEX_RURAL if has_land else TEX_SEA)
    w.ground_png, ao_png = ground_raster(ctry, spec, lc, rp, cpoly, M, blds, size)
    lc_png = landcover_png(lc, M) if has_land else b""
    sections = []
    if extra.walls:  # collision walls (x0, y0, x1, y1, z low, z high), cell ENU
        sections.append((b"WALL", np.asarray(extra.walls, np.float32).tobytes()))
    det, dst = encode_detail(geos, extra.decks, extra.lights, [], [], ao_png, extra.trees, [], landcover=lc_png,
                             sections=sections)
    with open(os.path.join(out, "cells", f"{mesh}.rjdet"), "wb") as f:
        f.write(det)
    data = w.to_bytes()
    with open(os.path.join(out, "cells", f"{mesh}.rjcell"), "wb") as f:
        f.write(data)
    summ = {"mesh": mesh, "anchor": [clat, clon, anchor_h], "buildings": len(w.buildings), "bytes": len(data),
            "det": len(det)}
    print(f"{mesh}: {len(w.buildings)} buildings, {len(data) / 1e6:.2f} MB + detail {len(det) / 1e6:.2f} MB "
          f"({size}px, {dst['trees']} trees), {time.time() - tc:.1f}s", flush=True)
    return summ, homes, works, pois, boxes


def far_boxes(blds):
    """Boxes of the buildings that read from afar (taller than 9 m or large)."""
    boxes = []
    for b in blds:
        fp = Polygon(b.footprint)
        if fp.area < 30 or (b.height < 9.0 and fp.area < 350.0):
            continue
        r = fp.minimum_rotated_rectangle
        cs = np.asarray(r.exterior.coords)[:4]
        e0, e1 = cs[1] - cs[0], cs[2] - cs[1]
        c = cs.mean(axis=0)
        yaw = math.atan2(e0[1], e0[0])
        boxes.append((float(c[0]), float(c[1]), float(np.linalg.norm(e0) / 2), float(np.linalg.norm(e1) / 2), yaw,
                      float(b.ground), float(max(3.0, b.height))))
    return boxes


# ---------------------------------------------------------------------------------------------
def _cover_masks(lc, M, W, rng_seed):
    """Organic class masks at W x W: weights sampled on a coarse grid, upsampled, noise-thresholded."""
    cw = 256
    X, Y = uv_to_xy(M, cw)
    out = {}
    noise = None
    for name in ("grass", "field", "paddy", "forest", "bare", "sand"):
        wgt = lc.sample(name, X, Y).astype(np.float32)
        if wgt.max() <= 0.02:
            continue
        img = Image.fromarray(np.clip(wgt * 255, 0, 255).astype(np.uint8), "L").resize((W, W), Image.BILINEAR)
        if noise is None:
            rs = np.random.default_rng(rng_seed)
            nz = rs.random((W // 8 + 1, W // 8 + 1)).astype(np.float32)
            noise = np.asarray(Image.fromarray((nz * 255).astype(np.uint8), "L").resize((W, W), Image.BICUBIC),
                               np.float32) / 255.0
        out[name] = (np.asarray(img, np.float32) / 255.0 + (noise - 0.5) * 0.35) > 0.5
    return out


def ground_raster(ctry, spec, lc, rp, cpoly: Polygon, M, blds, size) -> tuple[bytes, bytes]:
    ss = 2 if size >= 1024 else 1
    W = size * ss
    base = np.zeros((W, W, 3), np.uint8)
    base[:] = COL["grass"]  # open country; lots in the towns are painted as yards below
    if not cpoly.within(ctry.land):  # the sea floor (seen through shallow water and at low tide)
        Xs, Ys = uv_to_xy(M, W)
        base[ctry.terrain.sample(Xs, Ys) <= 0.0] = COL["seabed"]
    if ctry.towns is not None and ctry.towns.intersects(cpoly):
        tm = Image.new("L", (W, W), 0)
        tdr = ImageDraw.Draw(tm)
        uvm = lambda coords: [(float(u) * W, float(v) * W) for u, v in (np.column_stack([np.asarray(coords)[:, :2], np.ones(len(coords))]) @ M)]  # noqa: E731
        for p in parts(ctry.towns.intersection(cpoly.buffer(20))):
            tdr.polygon(uvm(p.exterior.coords), fill=255)
        base[np.asarray(tm) > 127] = COL["yard"]
    masks = _cover_masks(lc, M, W, abs(hash(tuple(np.round(M.ravel(), 6)))) % (2**31))
    if "paddy" in masks or "field" in masks:
        X, Y = uv_to_xy(M, W)
    for name in ("grass", "field", "paddy", "forest", "bare", "sand"):
        if name not in masks:
            continue
        m = masks[name]
        if name == "paddy":
            # plots ~30 x 90 m between grass levees, gravel farm roads every few plots
            ang = 0.35
            u = X * math.cos(ang) + Y * math.sin(ang)
            v = -X * math.sin(ang) + Y * math.cos(ang)
            lev = (np.abs(((u + 15) % 30) - 15) < 0.5) | (np.abs(((v + 45) % 90) - 45) < 0.5)
            road = (np.abs(((u + 75) % 150) - 75) < 1.8) | (np.abs(((v + 135) % 270) - 135) < 1.8)
            col = np.where(road[..., None], COL["farmroad"], np.where(lev[..., None], COL["levee"], COL["paddy"]))
            base[m] = col[m]
        elif name == "field":
            base[m] = COL["field"]  # the furrows are drawn by the client (a raster this coarse aliases them)
        else:
            base[m] = COL[name]
    img = Image.fromarray(base, "RGB")
    d = ImageDraw.Draw(img)

    def px(coords):
        c = np.asarray(coords)[:, :2]
        uv = np.column_stack([c, np.ones(len(c))]) @ M
        return [(float(u) * W, float(v) * W) for u, v in uv]

    clip = cpoly.buffer(20)

    def fill(geom, color):
        for p in parts(geom.intersection(clip) if geom is not None and not geom.is_empty else None):
            d.polygon(px(p.exterior.coords), fill=color)
            for h in p.interiors:
                d.polygon(px(h.coords), fill=COL["yard"])

    fill(unary_union([Polygon(pp) for _, pp in L.PARKS]), COL["park"])
    fill(spec.grass, COL["grass"])
    fill(spec.apron, COL["apron"])
    fill(spec.runway, COL["runway"])
    for rv in ctry.rivers:
        if rv.poly.intersects(clip):
            fill(rv.poly, COL["bed"])
    for _, _, pp in L.LAKES:
        fill(Polygon(pp), COL["bed"])
    if rp is not None:
        fill(rp.carriage, COL["road"])
        fill(rp.sidewalk, COL["sidewalk"])
        fill(rp.median, COL["median"])
        fill(rp.plaza, COL["plaza"])
    fill(spec.marks, COL["mark"])
    fpm = None
    if blds:
        fpm = Image.new("L", (W, W), 0)
        fd = ImageDraw.Draw(fpm)
        for b in blds:
            pts = px(b.footprint)
            d.polygon(pts, fill=COL["footprint"])
            fd.polygon(pts, fill=255)
    holes = spec.portal_holes is not None and not spec.portal_holes.is_empty and spec.portal_holes.intersects(clip)
    hole_mask = None
    if holes:  # (applied after the palette reduction, as its own transparent palette entry)
        hm = Image.new("L", (W, W), 0)
        hd = ImageDraw.Draw(hm)
        for p in parts(spec.portal_holes.intersection(clip)):
            hd.polygon(px(p.exterior.coords), fill=255)
        hole_mask = hm
    if fpm is not None:
        ao = np.asarray(fpm.filter(ImageFilter.GaussianBlur(radius=5)), np.float32) / 255.0
        shade = 1.0 - 0.45 * np.clip(ao * 1.6, 0.0, 1.0)
        shade[np.asarray(fpm) > 0] = 1.0
        ao_img = Image.fromarray(np.clip(shade * 255.0, 0, 255).astype(np.uint8), "L").resize((TEX // 2, TEX // 2), Image.BILINEAR)
    else:
        ao_img = Image.new("L", (16, 16), 255)
    abuf = io.BytesIO()
    ao_img.save(abuf, format="PNG", optimize=True)
    if size != W:
        img = img.resize((size, size), Image.LANCZOS)
    q = img.quantize(colors=62, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    buf = io.BytesIO()
    if hole_mask is not None:
        # the tunnel mouths get a palette entry of their own that is transparent (the client
        # discards those texels, so the train is seen entering the portal)
        if size != W:
            hole_mask = hole_mask.resize((size, size), Image.NEAREST)
        pal = q.getpalette()[:62 * 3]
        pal += [0, 0, 0] * (62 - len(pal) // 3) + list(COL["hole"])
        idx = np.asarray(q).copy()
        idx[np.asarray(hole_mask) > 127] = 62
        q2 = Image.fromarray(idx, "P")
        q2.putpalette(pal)
        q2.save(buf, format="PNG", optimize=False, transparency=62)
    else:
        q.save(buf, format="PNG", optimize=True)
    return buf.getvalue(), abuf.getvalue()


def landcover_png(lc, M) -> bytes:
    """256 x 256 RGBA (north up, same mapping as the ground raster): R forest, G paddy, B field, A bare."""
    X, Y = uv_to_xy(M, 256)
    ch = [lc.sample(n, X, Y) for n in ("forest", "paddy", "field", "bare")]
    arr = np.clip(np.stack(ch, axis=-1) * 255.0, 0, 255).astype(np.uint8)
    buf = io.BytesIO()
    Image.fromarray(arr, "RGBA").save(buf, format="PNG", optimize=True)
    return buf.getvalue()


# ---------------------------------------------------------------------------------------------
def road_graph(net, fi: LocalFrame):
    """RJROAD from the generated centerlines (noded at crossings); width = carriageway width."""
    lines, widths, full, kinds = [], [], [], []
    for r in net.roads:
        if r.carriage < 3.0:
            continue
        lines.append(r.line)
        widths.append(r.carriage)
        full.append(r.width)
        kinds.append(r.kind)
    snap_tree = STRtree(lines)
    lines = [LineString(ln.coords) for ln in lines]
    inserts = {}
    for i in range(len(lines)):
        c = list(lines[i].coords)
        for which in (0, -1):
            p = Point(c[which])
            best = None
            for j in snap_tree.query(p.buffer(30.0)):
                if j == i:
                    continue
                # (only the ramps join the expressway; nothing else ends on it)
                if kinds[j] == "expressway" and kinds[i] != "ramp":
                    continue
                if kinds[i] == "expressway":
                    continue
                d = lines[j].distance(p)
                if 1e-6 < d < full[j] * 0.5 + 3.0 and (best is None or d < best[0]):
                    best = (d, j)
            if best is not None:
                j = best[1]
                t = lines[j].project(p)
                q = lines[j].interpolate(t)
                c[which] = (q.x, q.y)
                inserts.setdefault(j, []).append(t)
        lines[i] = LineString(c)
    for j, ts in inserts.items():
        coords = list(lines[j].coords)
        cum = [0.0]
        for a, b in zip(coords, coords[1:]):
            cum.append(cum[-1] + math.hypot(b[0] - a[0], b[1] - a[1]))
        pts = [(cum[k], coords[k]) for k in range(len(coords))]
        for t in ts:
            q = lines[j].interpolate(t)
            pts.append((t, (q.x, q.y)))
        pts.sort(key=lambda e: e[0])
        o = [pts[0][1]]
        for _, xy in pts[1:]:
            if math.hypot(xy[0] - o[-1][0], xy[1] - o[-1][1]) > 1e-6:
                o.append(xy)
        lines[j] = LineString(o)
    # (the expressway and its ramps cross the other roads on bridges: they meet them only where a
    # ramp ends on a road, so the two sets are noded apart)
    exw = [k for k in range(len(lines)) if kinds[k] in ("expressway", "ramp")]
    rest = [k for k in range(len(lines)) if kinds[k] not in ("expressway", "ramp")]
    segs = []
    for group in (rest, exw):
        if not group:
            continue
        noded = unary_union(MultiLineString([lines[k] for k in group]))
        segs += [g for g in getattr(noded, "geoms", [noded]) if isinstance(g, LineString) and g.length > 0.5]
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


def write_residents(out, homes, works, pois, rng, n_res=9000):
    rows = []
    if homes and works:
        hw = np.array([h["storeys"] * max(h["area"], 30.0) for h in homes])
        ww = np.array([w["storeys"] * max(w["area"], 30.0) for w in works])
        occ = {401: ["office_worker"] * 6 + ["engineer", "programmer", "designer", "executive", "lawyer"],
               402: ["shop_clerk"] * 5 + ["cook", "cook", "hairdresser", "barber", "patissier"],
               403: ["shop_clerk", "cook"], 404: ["shop_clerk", "office_worker", "cook"], 421: ["civil_servant"],
               422: ["teacher", "nurse", "doctor", "university_student"], 431: ["truck_driver", "delivery_rider"],
               441: ["factory_worker", "mechanic"]}
        hi = rng.choice(len(homes), size=n_res, p=hw / hw.sum())
        wi = rng.choice(len(works), size=n_res, p=ww / ww.sum())
        for k in range(n_res):
            h, wk = homes[hi[k]], works[wi[k]]
            # people work near home (a few commute between towns)
            if math.hypot(h["lat"] - wk["lat"], (h["lon"] - wk["lon"]) * 0.83) > 0.06 and rng.random() < 0.85:
                continue
            roll = rng.random()
            if roll < 0.12:
                o, w2 = rng.choice(["high_school_student", "university_student", "junior_high_student"]), wk
            elif roll < 0.25:
                o, w2 = "retired", None
            else:
                o, w2 = rng.choice(occ.get(wk["usage"], ["office_worker"])), wk
            rows.append([len(rows) + 1, o, h["id"], f"{h['lat']:.7f}", f"{h['lon']:.7f}", w2["id"] if w2 else "",
                         f"{w2['lat']:.7f}" if w2 else "", f"{w2['lon']:.7f}" if w2 else "", "resident"])
    with open(os.path.join(out, "residents.csv"), "w", newline="", encoding="utf-8") as f:
        cw = csv.writer(f)
        cw.writerow(["id", "occupation", "home_id", "home_lat", "home_lon", "work_id", "work_lat", "work_lon", "kind"])
        cw.writerows(rows)


# ---------------------------------------------------------------------------------------------
def write_far(out, ctry, spec, lc, fi, boxes):
    """Far view of the whole country: heights and colours on a lat/lon grid (1/24 of a mesh cell),
    building boxes, and the snow-potential map (all in the country frame)."""
    x0, y0, x1, y1 = -46000.0, -21000.0, 9000.0, 30000.0
    corners = [fi.to_geodetic(x, y) for x, y in ((x0, y0), (x1, y0), (x0, y1), (x1, y1))]
    lat0 = math.floor(min(c[0] for c in corners) * 120) / 120
    lat1 = math.ceil(max(c[0] for c in corners) * 120) / 120
    lon0 = math.floor(min(c[1] for c in corners) * 80) / 80
    lon1 = math.ceil(max(c[1] for c in corners) * 80) / 80
    sub = 24
    ny = int(round((lat1 - lat0) * 120 * sub)) + 1
    nx = int(round((lon1 - lon0) * 80 * sub)) + 1
    LA, LO = np.meshgrid(np.linspace(lat0, lat1, ny), np.linspace(lon0, lon1, nx), indexing="ij")
    P = to_local_arr(fi, LA, LO)
    z = ctry.terrain.sample(P[..., 0], P[..., 1])
    hz = np.ascontiguousarray(np.clip((z + 100.0) * 20.0, 0, 65535).astype("<u2")[::-1])  # north up
    import zlib
    comp = zlib.compressobj(9, zlib.DEFLATED, -15)
    raw = hz.tobytes()
    with open(os.path.join(out, "far_height.bin"), "wb") as f:
        f.write(b"RJFARH01" + struct.pack("<3I", nx, ny, len(raw)) + comp.compress(raw) + comp.flush())
    # colour map at twice the resolution
    cy, cx = ny * 2 - 1, nx * 2 - 1
    LA2, LO2 = np.meshgrid(np.linspace(lat0, lat1, cy), np.linspace(lon0, lon1, cx), indexing="ij")
    P2 = to_local_arr(fi, LA2, LO2)
    X, Y = P2[..., 0], P2[..., 1]
    z2 = ctry.terrain.sample(X, Y)
    col = np.zeros(X.shape + (3,), np.float32)
    col[:] = (112, 128, 84)
    for name, c in (("grass", COL["grass"]), ("field", COL["field"]), ("paddy", COL["paddy"]), ("forest", (40, 58, 32)),
                    ("bare", COL["bare"]), ("sand", COL["sand"])):
        wgt = lc.sample(name, X, Y)[..., None]
        col = col * (1 - wgt) + np.array(c, np.float32) * wgt
    towns = unary_union([Polygon(p) for n, s, p in L.DISTRICTS if s != "airport"])
    shapely.prepare(towns)
    tw = shapely.contains_xy(towns, X, Y)
    col[tw] = col[tw] * 0.3 + np.array((128, 124, 118)) * 0.7
    col[z2 <= 0.2] = (40, 70, 90)
    Image.fromarray(np.clip(col[::-1], 0, 255).astype(np.uint8), "RGB").quantize(colors=128, method=Image.Quantize.MEDIANCUT,
                                                                                  dither=Image.Dither.NONE).save(
        os.path.join(out, "far_color.png"), optimize=True)
    snow = lc.sample("snow", X, Y)
    Image.fromarray(np.clip(snow[::-1] * 255, 0, 255).astype(np.uint8), "L").resize((nx // 2, ny // 2), Image.BILINEAR).save(
        os.path.join(out, "snow.png"), optimize=True)
    # building boxes (every building bigger than a shed), country-local metres
    rec = bytearray(b"RJFARB01")
    rec += struct.pack("<I", len(boxes))
    for bx in boxes:
        rec += struct.pack("<7f", *bx)
    with open(os.path.join(out, "far_blds.bin"), "wb") as f:
        f.write(bytes(rec))
    with open(os.path.join(out, "far.txt"), "w", encoding="utf-8") as f:
        f.write("# far view: far_height.bin (uint16 (h + 100) * 20, north up), far_color.png, snow.png on a lat/lon grid\n")
        f.write(f"grid {lat0:.8f} {lon0:.8f} {lat1:.8f} {lon1:.8f} {nx} {ny}\n")
        f.write(f"frame {L.ORIGIN[0]} {L.ORIGIN[1]}\n")
        f.write(f"boxes {len(boxes)}\n")
    print(f"far view: {nx} x {ny} grid, {len(boxes)} building boxes", flush=True)


# ---------------------------------------------------------------------------------------------
def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "game", "data", "world", "country"))
    ap.add_argument("--seed", type=int, default=20260927)
    ap.add_argument("--terrain-n", type=int, default=129)
    ap.add_argument("--workers", type=int, default=4)
    ap.add_argument("--only", default="", help="comma-separated mesh codes (debug)")
    ap.add_argument("--preview", action="store_true", help="coarse 40 m terrain (quick test cooks)")
    ap.add_argument("--cache", default="", help="pickle of the generated layout (reused if present; debug)")
    a = ap.parse_args()
    out = a.out
    os.makedirs(os.path.join(out, "cells"), exist_ok=True)
    t0 = time.time()
    if a.cache and os.path.exists(a.cache):
        import pickle
        with open(a.cache, "rb") as f:
            ctry = pickle.load(f)
    else:
        ctry = generate(a.seed, preview=a.preview)
        if a.cache:
            import pickle
            with open(a.cache, "wb") as f:
                pickle.dump(ctry, f, protocol=pickle.HIGHEST_PROTOCOL)
    print(f"layout: {len(ctry.net.roads)} roads, {len(ctry.parcels)} parcels ({time.time() - t0:.0f}s)", flush=True)
    if getattr(ctry, "towns", None) is None:  # (layouts cached before the field existed)
        ctry.towns = unary_union([Polygon(p).buffer(0) for n, s, p in L.DISTRICTS if s not in ("airport", "village")])
    rng = np.random.default_rng(a.seed + 1)
    fi = LocalFrame(L.ORIGIN[0], L.ORIGIN[1], 0.0)
    terrain = ctry.terrain
    spec = specials.build_all(ctry, terrain, rng)
    print(f"specials: {len(spec.buildings)} structures ({time.time() - t0:.0f}s)", flush=True)
    lc = LandCover(ctry, spec)
    print(f"land cover ({time.time() - t0:.0f}s)", flush=True)

    # ---- buildings (parallel over parcels) ----
    sx, sy = L.SCRAMBLE
    from country.railgeom import stations_aligned
    st_xy = np.array([(x, y) for _, x, y, _, li, _ in stations_aligned() if L.RAIL_LINES[li]["kind"] != "shinkansen"])
    G.update(ctry=ctry, terrain=terrain, st_xy=st_xy, seed=a.seed)

    # ---- cells covering the country ----
    x0, y0, x1, y1 = -46000, -21000, 9000, 30000
    corners = [fi.to_geodetic(x, y) for x, y in ((x0, y0), (x1, y0), (x0, y1), (x1, y1))]
    lats, lons = [c[0] for c in corners], [c[1] for c in corners]
    meshes = meshes3_covering(min(lats), min(lons), max(lats), max(lons))
    land_all = ctry.land
    shapely.prepare(land_all)
    routes = unary_union([LineString(r) for _, _, _, r in L.FERRY_ROUTES]).buffer(1)
    cell_poly = {}
    for m in meshes:
        la0, lo0, la1, lo1 = Mesh(m).bounds()
        ring = to_local_arr(fi, np.array([la0, la0, la1, la1]), np.array([lo0, lo1, lo1, lo0]))[:, :2]
        P = Polygon(ring)
        if P.intersects(land_all.buffer(60)) or P.intersects(routes):
            cell_poly[m] = P
    if a.only:
        keep = set(a.only.split(","))
        cell_poly = {k: v for k, v in cell_poly.items() if k in keep}
    print(f"cells: {len(cell_poly)} of {len(meshes)} ({time.time() - t0:.0f}s)", flush=True)
    keys = list(cell_poly)
    ctree = STRtree([cell_poly[k] for k in keys])
    by_cell: dict[str, list] = {}
    for b in spec.buildings:
        cx, cy = b.footprint.mean(axis=0)
        hit = ctree.query(Point(cx, cy), predicate="intersects")
        if len(hit):
            by_cell.setdefault(keys[hit[0]], []).append(b)
    parcels_by_cell: dict[str, list] = {}
    for pc in ctry.parcels:
        c = pc.poly.centroid
        hit = ctree.query(c, predicate="intersects")
        if len(hit):
            parcels_by_cell.setdefault(keys[hit[0]], []).append(pc)
    G.update(spec=spec, lc=lc, fi=fi, out=out, by_cell=by_cell, args=a, cell_poly=cell_poly, parcels_by_cell=parcels_by_cell)
    # busy cells first so the pool stays balanced
    order = sorted(keys, key=lambda k: -len(parcels_by_cell.get(k, [])))
    with mp.get_context("fork").Pool(a.workers) as pool:
        results = pool.map(cook_cell, order, chunksize=1)
    summary = sorted([r[0] for r in results], key=lambda s: s["mesh"])
    homes = [h for r in results for h in r[1]]
    works = [w for r in results for w in r[2]]
    pois = []
    for name, x, y, hd, li, k in stations_aligned():
        la, lo = fi.to_geodetic(x, y)
        pois.append({"name": name, "lat": la, "lon": lo, "usage": 431, "height": 18.0, "id": f"akitsu-st-{li}-{name}"})
    for key, (name, x, y) in L.LANDMARKS.items():
        la, lo = fi.to_geodetic(x, y)
        pois.append({"name": name, "lat": la, "lon": lo, "usage": 454, "height": 0.0, "id": f"akitsu-{key}"})
    for apd in L.AIRPORTS:
        la, lo = fi.to_geodetic(*apd["terminal"])
        pois.append({"name": apd["name"], "lat": la, "lon": lo, "usage": 431, "height": 20.0, "id": "akitsu-ap-" + apd["name"]})
    for name, (x, y, hd) in L.PIERS.items():
        la, lo = fi.to_geodetic(x, y)
        pois.append({"name": name, "lat": la, "lon": lo, "usage": 431, "height": 12.0, "id": "akitsu-pier-" + name})
    for r in results:
        pois.extend(r[3])
    print(f"cells cooked: {sum(s['buildings'] for s in summary)} buildings ({time.time() - t0:.0f}s)", flush=True)

    rdata, nn, ne = road_graph(ctry.net, fi)
    with open(os.path.join(out, "roads.rjroad"), "wb") as f:
        f.write(rdata)
    print(f"road graph: {nn} nodes, {ne} edges", flush=True)
    specials.write_extra(spec, out, fi)
    write_far(out, ctry, spec, lc, fi, [bx for r in results for bx in r[4]])
    write_residents(out, homes, works, pois, rng)
    sp_lat, sp_lon = fi.to_geodetic(-265, -700)
    meta = {"id": "country", "name_ja": L.NAME_JA, "name_en": L.NAME_EN, "cells": summary,
            "pipeline_version": PIPELINE_VERSION, "fictional": True}
    with open(os.path.join(out, "slice.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=1)
    with open(os.path.join(out, "pois.json"), "w", encoding="utf-8") as f:
        json.dump(pois, f, ensure_ascii=False, indent=1)
    with open(os.path.join(out, "client.txt"), "w", encoding="utf-8") as f:
        f.write(f"# generated by cook_country.py {PIPELINE_VERSION}\n")
        f.write(f"name_ja {L.NAME_JA}\nname_en {L.NAME_EN}\n")
        f.write("world fictional\n")
        f.write(f"spawn {sp_lat} {sp_lon} 225\n")
        f.write(f"core_bbox {min(lats)} {min(lons)} {max(lats)} {max(lons)}\n")
        for s in summary:
            f.write(f"cell {s['mesh']} {s['anchor'][0]:.10f} {s['anchor'][1]:.10f} {s['anchor'][2]:.3f}\n")
        f.write("source 0 FICTIONAL 秋津国は日本をモチーフにした架空の国です。実在の場所・店・人物とは関係ありません。\n")
        for name, en, st, poly in N.CITIES:
            c = Polygon(poly).centroid
            la, lo = fi.to_geodetic(c.x, c.y)
            f.write(f"place {la:.7f} {lo:.7f} {'city' if st not in ('village',) else 'village'} {name}|{en}\n")
        for name, en, x, y, r in N.VILLAGES:
            la, lo = fi.to_geodetic(x, y)
            f.write(f"place {la:.7f} {lo:.7f} village {name}|{en}\n")
        for key, (reading_kana, roman) in L.READINGS.items():
            f.write(f"reading {key} {reading_kana} {roman}\n")
        for p in pois:
            f.write(f"poi {p['lat']:.7f} {p['lon']:.7f} {p['usage']} {p['height']:.1f} {p['name']}\n")
    tot = sum(s["bytes"] + s["det"] for s in summary)
    print(f"done in {time.time() - t0:.0f}s -> {out} ({tot / 1e6:.0f} MB in cells)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
