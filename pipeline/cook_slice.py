#!/usr/bin/env python3
"""Cook a slice: PLATEAU CityGML + GSI DEM -> RJCELL packages + metadata for the game client.

Usage: python3 pipeline/cook_slice.py pipeline/slices/shibuya.json [--out game/data/world/shibuya]
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import hashlib
import io
import json
import math
import os
import sys
import time

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from realjapan_pipeline import PIPELINE_VERSION  # noqa: E402
from realjapan_pipeline.atlas import Atlas, fetch_images  # noqa: E402
from realjapan_pipeline.citygml import iter_buildings, iter_furniture, iter_roads, parse_appearance  # noqa: E402
from realjapan_pipeline.dem_gsi import DemSampler  # noqa: E402
from realjapan_pipeline.geodesy import LocalFrame, Mesh  # noqa: E402
from realjapan_pipeline.interior import build_interior, opening_outlines, parse_underground, street_openings  # noqa: E402
from realjapan_pipeline.rjcell import (  # noqa: E402
    GEOM_VERIFIED_EXTERIOR, INTERIOR_UNKNOWN, BuildingRec, CellWriter)
from realjapan_pipeline.triangulate import triangulate  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

WALL = {401: (178, 186, 196), 402: (206, 200, 190), 403: (196, 184, 170), 404: (190, 192, 198),
        411: (222, 210, 188), 412: (214, 206, 194), 413: (208, 198, 184), 414: (204, 198, 190),
        415: (200, 190, 176), 421: (196, 196, 186), 422: (212, 204, 180), 431: (170, 170, 165),
        441: (165, 168, 170)}
DEFAULT_WALL = (190, 188, 184)
GROUND_COLORS = {"base": (150, 146, 136), "road": (72, 72, 76), "sidewalk": (190, 184, 172),
                 "median": (125, 122, 114), "planting": (93, 122, 68), "rail": (107, 94, 80),
                 "footprint": (85, 82, 76)}
TEX = 2048

# PLATEAU CityFurniture_function codes (codelist) -> render treatment.
MARKING_CODES = range(1000, 1300)          # 道路標示・区画線・横断歩道・停止線 -> painted into the ground texture
FURNITURE_COLORS = {2000: (150, 150, 146),  # 柵・壁
                    4800: (96, 98, 102), 4810: (96, 98, 102), 4820: (96, 98, 102), 4830: (96, 98, 102),
                    4840: (120, 116, 108),  # 電柱
                    4200: (118, 120, 124),  # 照明施設
                    4900: (58, 60, 64),     # 交通信号機
                    4020: (132, 130, 126), 4010: (150, 150, 150), 4030: (150, 150, 150),
                    8010: (120, 130, 140)}  # 停留所
DEFAULT_FURNITURE = (128, 128, 128)


def jitter(bid: str) -> float:
    h = int(hashlib.md5(bid.encode()).hexdigest()[:8], 16)
    return 0.92 + 0.16 * (h / 0xFFFFFFFF)


def to_local(frame: LocalFrame, ring) -> np.ndarray:
    return np.array([frame.to_local(la, lo, h) for la, lo, h in ring], dtype=np.float64)


def color_for(kind: str, usage: int, j: float) -> tuple[int, int, int, int]:
    w = WALL.get(usage, DEFAULT_WALL)
    if kind == "roof":
        c = tuple(v * 0.70 for v in w)
    elif kind == "installation":
        c = (150, 150, 150)
    else:
        c = w
    return tuple(int(max(0, min(255, v * j))) for v in c) + (255,)


def building_mesh(b, frame: LocalFrame, app=None, atlas: Atlas | None = None):
    """Returns ({page: (pos, nrm, col, uv, idx)}, ground_z) or None. page 0 = photo atlas, -1 = vertex colour."""
    polys = b.lod2 if b.lod2 else b.lod1
    j = jitter(b.gml_id)
    parts: dict[int, list] = {}
    for poly in polys:
        if poly.kind == "ground":
            continue
        ext = to_local(frame, poly.exterior)
        holes = [to_local(frame, h) for h in poly.interiors]
        v, t, n = triangulate(ext, holes)
        if len(t) == 0:
            continue
        kind = poly.kind
        if kind == "lod1":
            kind = "roof" if n[2] > 0.9 else "wall"
            if n[2] < -0.9:
                continue  # bottom face
        uv = None
        if atlas is not None and app is not None and atlas.rects:
            rings = [(poly.ext_id, len(poly.exterior))] + list(zip(poly.int_ids, (len(h) for h in poly.interiors)))
            uvs = []
            for rid, npts in rings:
                tex = app.ring_uv.get(rid)
                if tex is None or tex[0] not in atlas.rects:
                    uvs = None
                    break
                coords = tex[1][:npts] if len(tex[1]) == npts + 1 else tex[1]
                if len(coords) != npts:
                    uvs = None
                    break
                uvs.extend(atlas.remap(tex[0], cu, cv) for cu, cv in coords)
            if uvs is not None:
                uv = np.array(uvs, dtype=np.float64)
        page = 0 if uv is not None else -1
        col = (255, 255, 255, 255) if page == 0 else color_for(kind, b.usage, j)
        P = parts.setdefault(page, [[], [], [], [], [], 0])
        P[0].append(v)
        P[1].append(np.repeat(n[None, :], len(v), axis=0))
        P[2].append(np.repeat(np.array([col], dtype=np.uint8), len(v), axis=0))
        P[3].append(uv if uv is not None else np.zeros((len(v), 2)))
        P[4].append(t.reshape(-1) + P[5])
        P[5] += len(v)
    if not parts:
        return None
    ground = min(float(np.vstack(P[0])[:, 2].min()) for P in parts.values())
    out = {}
    for page, P in parts.items():
        pos, nrm, col, uv, idx = np.vstack(P[0]), np.vstack(P[1]), np.vstack(P[2]), np.vstack(P[3]), np.concatenate(P[4])
        # Baked sky-occlusion gradient: darker near the street, full brightness higher up.
        occl = 0.70 + 0.30 * np.clip((pos[:, 2] - ground) / 12.0, 0.0, 1.0)
        col[:, :3] = (col[:, :3].astype(np.float64) * occl[:, None]).clip(0, 255).astype(np.uint8)
        out[page] = [pos, nrm, col, uv, idx]
    # Skirt: extend the footprint 2 m below the base so buildings never float on coarse terrain.
    if b.footprint and len(b.footprint) >= 3:
        fp = to_local(frame, b.footprint)[:, :2]
        wc = np.array(color_for("wall", b.usage, j), dtype=np.uint8)
        wc[:3] = (wc[:3] * 0.7).astype(np.uint8)
        sp, sn, si = [], [], []
        for k in range(len(fp)):
            a, c2 = fp[k], fp[(k + 1) % len(fp)]
            d = c2 - a
            ln = math.hypot(*d)
            if ln < 1e-3:
                continue
            nx, ny = d[1] / ln, -d[0] / ln
            q = len(sp)
            sp += [(a[0], a[1], ground - 2.0), (c2[0], c2[1], ground - 2.0), (c2[0], c2[1], ground + 0.05),
                   (a[0], a[1], ground + 0.05)]
            sn += [(nx, ny, 0.0)] * 4
            si += [q, q + 1, q + 2, q, q + 2, q + 3, q, q + 2, q + 1, q, q + 3, q + 2]  # double-sided
        if sp:
            base = out.setdefault(-1, [np.zeros((0, 3)), np.zeros((0, 3)), np.zeros((0, 4), np.uint8), np.zeros((0, 2)),
                                       np.zeros(0, np.int64)])
            nb = len(base[0])
            base[0] = np.vstack([base[0], np.array(sp)])
            base[1] = np.vstack([base[1], np.array(sn)])
            base[2] = np.vstack([base[2], np.repeat(wc[None, :], len(sp), axis=0)])
            base[3] = np.vstack([base[3], np.zeros((len(sp), 2))])
            base[4] = np.concatenate([base[4], np.array(si, dtype=np.int64) + nb])
    return out, ground


def rasterize_ground(bounds, roads, buildings, markings=(), holes=()) -> bytes:
    """Ground colour raster; `holes` (street-level openings of underground spaces) become transparent
    so the client can cut them out of the terrain and show the real stairwell below."""
    min_lat, min_lon, max_lat, max_lon = bounds
    ss = 2
    W = H = TEX * ss
    img = Image.new("RGB", (W, H), GROUND_COLORS["base"])
    d = ImageDraw.Draw(img)

    def px(ring):
        return [((lo - min_lon) / (max_lon - min_lon) * W, (max_lat - la) / (max_lat - min_lat) * H)
                for la, lo, *_ in ring]

    for r in roads:
        for p in r.lod1:
            d.polygon(px(p.exterior), fill=GROUND_COLORS["road"])
    order = ["road", "rail", "sidewalk", "median", "planting"]
    for kind in order:
        for r in roads:
            for p in r.detail:
                if p.kind == kind:
                    d.polygon(px(p.exterior), fill=GROUND_COLORS[kind])
                    for hole in p.interiors:
                        d.polygon(px(hole), fill=GROUND_COLORS["base"])
    for poly in markings:  # real road markings (crosswalks, lane lines, stop lines) from PLATEAU frn
        d.polygon(px(poly.exterior), fill=(226, 226, 220))
    for b in buildings:
        if b.footprint:
            d.polygon(px(b.footprint), fill=GROUND_COLORS["footprint"])
    img = img.resize((TEX, TEX), Image.LANCZOS)
    # Few flat colours + anti-aliased edges: an adaptive 64-colour palette is visually lossless here.
    img = img.quantize(colors=63, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    save = {}
    if holes:
        mask = Image.new("L", (W, H), 0)
        md = ImageDraw.Draw(mask)
        for ring in holes:
            md.polygon(px(ring), fill=255)
        m = np.asarray(mask.resize((TEX, TEX), Image.BILINEAR)) >= 128
        if m.any():
            pal = img.getpalette()[: 63 * 3] + [0, 0, 0]
            idx = np.asarray(img).copy()
            idx[m] = 63
            img = Image.fromarray(idx, mode="P")
            img.putpalette(pal)
            save["transparency"] = 63
    buf = io.BytesIO()
    img.save(buf, format="PNG", optimize=True, **save)
    return buf.getvalue()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("slice")
    ap.add_argument("--raw", default=os.path.join(ROOT, "data", "raw"))
    ap.add_argument("--out", default=None)
    ap.add_argument("--terrain-n", type=int, default=129)
    ap.add_argument("--no-textures", action="store_true")
    ap.add_argument("--atlas-size", type=int, default=4096)
    a = ap.parse_args()
    cfg = json.load(open(a.slice, encoding="utf-8"))
    out = a.out or os.path.join(ROOT, "game", "data", "world", cfg["id"])
    os.makedirs(os.path.join(out, "cells"), exist_ok=True)
    pl = cfg["plateau"]
    dem = DemSampler(os.path.join(a.raw, "gsi"))
    today = dt.date.today().isoformat()

    sources = [
        {"id": f"plateau:{pl['dataset']}", "title": f"3D都市モデル（Project PLATEAU）{pl['city_name']}（{pl['year']}年度）",
         "url": pl["landing_page"], "license": "PLATEAU-TOU",
         "attribution": f"「3D都市モデル（Project PLATEAU）{pl['city_name']}（{pl['year']}年度）」（国土交通省）（{pl['landing_page']}）を加工して作成",
         "retrieved_at": today, "usage": "asset"},
        {"id": "gsi:dem_tiles", "title": "国土地理院 標高タイル（DEM5A/DEM5B/DEM10B）",
         "url": "https://maps.gsi.go.jp/development/ichiran.html", "license": "GSI-TOU",
         "attribution": "出典：国土地理院 標高タイル（https://maps.gsi.go.jp/development/ichiran.html）を加工して作成", "retrieved_at": today, "usage": "asset",
         "rights_note": "基本測量成果。焼き込み配布時の測量法上の申請要否は未確認（公開配布前に要確認）"},
    ]

    # PLATEAU LOD4 underground buildings first: their street-level openings are cut out of the ground.
    undergrounds, openings = [], []
    for mesh in cfg["cells"]:
        upath = os.path.join(a.raw, "plateau", f"{mesh}_ubld_6697_op.gml")
        if not os.path.exists(upath):
            continue
        for u in parse_underground(upath):
            if len(u.polys) < 100:
                continue  # LOD1-only blocks carry no interior
            undergrounds.append((mesh, u))
            openings.extend(street_openings(u, dem))

    pois, residents_homes, workplaces = [], [], []
    summary = []
    for mesh in cfg["cells"]:
        t0 = time.time()
        m = Mesh(mesh)
        bounds = m.bounds()
        clat, clon = m.center()
        anchor_h = dem.height(clat, clon)
        anchor_h = 0.0 if math.isnan(anchor_h) else round(anchor_h, 2)
        frame = LocalFrame(clat, clon, anchor_h)
        w = CellWriter(mesh, (clat, clon, anchor_h), bounds)

        bpath = os.path.join(a.raw, "plateau", f"{mesh}_bldg_6697_op.gml")
        tpath = os.path.join(a.raw, "plateau", f"{mesh}_tran_6697_op.gml")
        blds = list(iter_buildings(bpath)) if os.path.exists(bpath) else []
        roads = list(iter_roads(tpath)) if os.path.exists(tpath) else []

        # Real facade/roof photographs from PLATEAU appearance data, packed into one atlas page.
        app, atlas = None, None
        if not a.no_textures and os.path.exists(bpath) and pl.get("asset_base"):
            app = parse_appearance(bpath)
            paths = fetch_images(pl["asset_base"] + "bldg/", app.images, os.path.join(a.raw, "plateau", "textures"))
            imgs = {}
            for i, uri in enumerate(app.images):
                if uri in paths:
                    try:
                        imgs[i] = Image.open(paths[uri])
                    except Exception:
                        pass
            atlas = Atlas(a.atlas_size)
            atlas.build(imgs)
            w.atlas_jpegs.append(atlas.jpeg(quality=75))
            print(f"  textures: {len(imgs)}/{len(app.images)} images, atlas scale {atlas.scale:.2f}", flush=True)

        n_lod2 = n_tex = 0
        for b in blds:
            res = building_mesh(b, frame, app, atlas)
            if res is None:
                continue
            parts, ground = res
            if any(len(p[0]) > CellWriter.MAX_VERTS for p in parts.values()):
                continue
            first_part = None
            for page in sorted(parts, reverse=True):
                pos, nrm, col, uv, idx = parts[page]
                r = w.add_geometry(pos, nrm, col, idx, uv, page)
                first_part = first_part or r
            ch, first, cnt = first_part
            n_tex += 0 in parts
            allpos = np.vstack([p[0] for p in parts.values()])
            pos = allpos
            fp = [tuple(p[:2]) for p in to_local(frame, b.footprint)] if b.footprint else []
            rec = BuildingRec(
                id=b.building_id or b.gml_id, name=b.name, usage=b.usage, bclass=b.bclass,
                measured_height=b.measured_height, storeys_above=b.storeys_above, storeys_below=b.storeys_below,
                lod=b.best_lod, geometry_status=GEOM_VERIFIED_EXTERIOR, interior_status=INTERIOR_UNKNOWN,
                source_index=0, ground_z=ground, bmin=tuple(pos.min(0)), bmax=tuple(pos.max(0)), footprint=fp,
                chunk=ch, first_index=first, index_count=cnt)
            w.buildings.append(rec)
            n_lod2 += b.best_lod == 2
            fpll = b.footprint or b.lod1[0].exterior
            clat_b = sum(p[0] for p in fpll) / len(fpll)
            clon_b = sum(p[1] for p in fpll) / len(fpll)
            area = abs(sum(fp[k][0] * fp[(k + 1) % len(fp)][1] - fp[(k + 1) % len(fp)][0] * fp[k][1]
                           for k in range(len(fp)))) / 2 if len(fp) >= 3 else 0.0
            if b.name:
                pois.append({"name": b.name, "lat": clat_b, "lon": clon_b, "usage": b.usage, "id": rec.id,
                             "cell": mesh, "height": b.measured_height})
            entry = {"id": rec.id, "lat": clat_b, "lon": clon_b, "usage": b.usage,
                     "storeys": max(1, b.storeys_above), "area": area}
            if b.usage in (411, 412, 413, 414, 415):
                residents_homes.append(entry)
            elif b.usage in (401, 402, 403, 404, 421, 422, 431, 441):
                workplaces.append(entry)

        # Street furniture (PLATEAU frn): markings -> ground texture, the rest -> 3D meshes snapped to terrain.
        markings, n_frn = [], 0
        fpath = os.path.join(a.raw, "plateau", f"{mesh}_frn_6697_op.gml")
        if os.path.exists(fpath):
            for f in iter_furniture(fpath):
                if f.function in MARKING_CODES:
                    markings.extend(f.polys)
                    continue
                P, N, C, I, base = [], [], [], [], 0
                colr = FURNITURE_COLORS.get(f.function, DEFAULT_FURNITURE) + (255,)
                for poly in f.polys:
                    v, t, nrm_ = triangulate(to_local(frame, poly.exterior), [to_local(frame, h) for h in poly.interiors])
                    if len(t) == 0:
                        continue
                    P.append(v)
                    N.append(np.repeat(nrm_[None, :], len(v), axis=0))
                    C.append(np.repeat(np.array([colr], dtype=np.uint8), len(v), axis=0))
                    I.append(t.reshape(-1) + base)
                    base += len(v)
                if not P:
                    continue
                pos = np.vstack(P)
                if len(pos) > CellWriter.MAX_VERTS:
                    continue
                # Snap the item's base onto our terrain (survey heights vs DEM can differ by decimetres).
                ring = f.polys[0].exterior
                la_c = sum(p[0] for p in ring) / len(ring)
                lo_c = sum(p[1] for p in ring) / len(ring)
                hh = dem.height(la_c, lo_c)
                if not math.isnan(hh):
                    pos[:, 2] += frame.to_local(la_c, lo_c, hh)[2] - pos[:, 2].min()
                w.add_geometry(pos, np.vstack(N), np.vstack(C), np.concatenate(I))
                n_frn += 1
            print(f"  furniture: {n_frn} objects, {len(markings)} marking polygons", flush=True)

        # Terrain grid over the cell bounds (shared edges -> seamless neighbours).
        n = a.terrain_n
        lat0, lon0, lat1, lon1 = bounds
        dlat, dlon = (lat1 - lat0) / (n - 1), (lon1 - lon0) / (n - 1)
        hz = np.zeros((n, n), dtype=np.float64)
        for i in range(n):
            for jj in range(n):
                la, lo = lat0 + i * dlat, lon0 + jj * dlon
                hh = dem.height(la, lo)
                hz[i, jj] = frame.to_local(la, lo, anchor_h if math.isnan(hh) else hh)[2]
        w.set_terrain(hz, lat0, lon0, dlat, dlon)
        w.ground_png = rasterize_ground(bounds, roads, blds, markings, openings)

        data = w.to_bytes()
        with open(os.path.join(out, "cells", f"{mesh}.rjcell"), "wb") as f:
            f.write(data)
        summary.append({"mesh": mesh, "buildings": len(w.buildings), "lod2": n_lod2, "textured": n_tex, "roads": len(roads),
                        "bytes": len(data), "anchor": [clat, clon, anchor_h], "bounds": bounds})
        print(f"{mesh}: {len(w.buildings)} buildings ({n_lod2} LOD2, {n_tex} photo-textured), {len(roads)} roads, "
              f"{len(data) / 1e6:.1f} MB, {time.time() - t0:.1f}s", flush=True)

    # Verified interiors: PLATEAU LOD4 underground buildings (floors, walls, stairs; no shop fit-out).
    interiors = []
    os.makedirs(os.path.join(out, "interiors"), exist_ok=True)
    for mesh, u in undergrounds:
        lat_c = float(np.mean([p[0] for _, r in u.polys for p in r]))
        lon_c = float(np.mean([p[1] for _, r in u.polys for p in r]))
        frame = LocalFrame(lat_c, lon_c, 0.0)
        data, st, ents = build_interior(u, frame, dem, triangulate, to_local, 0)
        fname = f"{u.gml_id}.rjint"
        with open(os.path.join(out, "interiors", fname), "wb") as f:
            f.write(data)
        geo_ents = [g for _, _, g in ents]
        interiors.append({"id": u.gml_id, "file": fname, "name": u.name, "entrances": geo_ents,
                          "openings": opening_outlines(street_openings(u, dem))})
        print(f"interior {u.name} ({mesh}): {st} -> {len(data) / 1e6:.1f} MB", flush=True)

    # Fictional residents placed in real residential buildings; jobs in real workplaces.
    rng = np.random.default_rng(20260926)
    home_w = np.array([h["storeys"] * max(h["area"], 30.0) for h in residents_homes])
    work_w = np.array([wp["storeys"] * max(wp["area"], 30.0) for wp in workplaces])
    # Job mix per workplace usage (game assumption; repeated entries weight the draw).
    occupations_by_usage = {
        401: ["office_worker"] * 6 + ["engineer", "engineer", "programmer", "programmer", "designer", "executive",
                                      "lawyer", "architect", "video_creator"],
        402: ["shop_clerk"] * 5 + ["cook", "cook", "hairdresser", "barber", "patissier"],
        403: ["shop_clerk", "shop_clerk", "cook"],
        404: ["shop_clerk", "shop_clerk", "office_worker", "office_worker", "cook"],
        421: ["civil_servant"] * 3 + ["police_officer"],
        422: ["university_student"] * 4 + ["high_school_student"] * 2 + ["teacher", "teacher", "nurse", "doctor",
                                                                          "childcare_worker", "vocational_student"],
        431: ["truck_driver", "delivery_rider"],
        441: ["factory_worker", "mechanic"]}
    rows = []
    n_res = 3000
    if len(residents_homes) and len(workplaces):
        hi = rng.choice(len(residents_homes), size=n_res, p=home_w / home_w.sum())
        wi = rng.choice(len(workplaces), size=n_res, p=work_w / work_w.sum())
        for k in range(n_res):
            h, wp = residents_homes[hi[k]], workplaces[wi[k]]
            roll = rng.random()
            if roll < 0.12:
                occ, wp2 = rng.choice(["high_school_student", "university_student", "junior_high_student"]), wp
            elif roll < 0.25:
                occ, wp2 = "retired", None
            else:
                occ, wp2 = rng.choice(occupations_by_usage.get(wp["usage"], ["office_worker"])), wp
            rows.append([k + 1, occ, h["id"], f"{h['lat']:.7f}", f"{h['lon']:.7f}",
                         wp2["id"] if wp2 else "", f"{wp2['lat']:.7f}" if wp2 else "", f"{wp2['lon']:.7f}" if wp2 else ""])
    for r in rows:
        r.append("resident")
    # Inbound commuters: people who live outside the slice, arrive by train at a real station
    # (buildings named 〜駅 in PLATEAU) and walk to a real workplace. Their count is a placeholder
    # (calibration against census daytime population is TODO). Their "home" position is the station.
    stations = [p for p in pois if p["name"].endswith("駅")]
    n_comm = 4000 if stations and workplaces else 0
    if n_comm:
        wi = rng.choice(len(workplaces), size=n_comm, p=work_w / work_w.sum())
        for k in range(n_comm):
            wp = workplaces[wi[k]]
            st = min(stations, key=lambda p: (p["lat"] - wp["lat"]) ** 2 + ((p["lon"] - wp["lon"]) * 0.81) ** 2)
            occ = rng.choice(occupations_by_usage.get(wp["usage"], ["office_worker"]))
            rows.append([len(rows) + 1, occ, st["id"], f"{st['lat']:.7f}", f"{st['lon']:.7f}", wp["id"],
                         f"{wp['lat']:.7f}", f"{wp['lon']:.7f}", "commuter"])
    with open(os.path.join(out, "residents.csv"), "w", newline="", encoding="utf-8") as f:
        cw = csv.writer(f)
        cw.writerow(["id", "occupation", "home_id", "home_lat", "home_lon", "work_id", "work_lat", "work_lon", "kind"])
        cw.writerows(rows)

    meta = {"id": cfg["id"], "name_ja": cfg["name_ja"], "name_en": cfg["name_en"], "cells": summary,
            "core_bbox": cfg["core_bbox"], "spawn": cfg["spawn"], "pipeline_version": PIPELINE_VERSION,
            "generated_at": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
            "notes": ["Heights: T.P. (orthometric). Geoid undulation not applied (GSIGEO2011 not bundled); "
                      "relative placement inside the slice is unaffected.",
                      "Residents are fictional people placed in real residential buildings (weighted by floor area). "
                      "Calibration against census data is TODO.",
                      "Inbound commuters (placeholder count) arrive at real stations and walk to real workplaces."]}
    with open(os.path.join(out, "slice.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=1)
    with open(os.path.join(out, "sources.json"), "w", encoding="utf-8") as f:
        json.dump(sources, f, ensure_ascii=False, indent=1)
    with open(os.path.join(out, "pois.json"), "w", encoding="utf-8") as f:
        json.dump(pois, f, ensure_ascii=False, indent=1)
    # Line-oriented metadata for the C++ client (no JSON dependency there).
    with open(os.path.join(out, "client.txt"), "w", encoding="utf-8") as f:
        f.write(f"# generated by cook_slice.py {PIPELINE_VERSION}\n")
        f.write(f"name_ja {cfg['name_ja']}\nname_en {cfg['name_en']}\n")
        sp = cfg["spawn"]
        f.write(f"spawn {sp['lat']} {sp['lon']} {sp['heading_deg']}\n")
        f.write(f"core_bbox {' '.join(str(v) for v in cfg['core_bbox'])}\n")
        for s in summary:
            f.write(f"cell {s['mesh']} {s['anchor'][0]:.10f} {s['anchor'][1]:.10f} {s['anchor'][2]:.3f}\n")
        for i, s in enumerate(sources):
            f.write(f"source {i} {s['license']} {s['attribution']}\n")
        for p in pois:
            f.write(f"poi {p['lat']:.7f} {p['lon']:.7f} {p['usage']} {p['height']:.1f} {p['name']}\n")
        for it in interiors:
            f.write(f"interior {it['id']} {it['file']} 0 VERIFIED {it['name']}\n")
            for la, lo, h in it["entrances"]:
                f.write(f"entrance {it['id']} {la:.8f} {lo:.8f} {h:.3f}\n")
            # Street-level openings (for pedestrian navigation; the terrain holes live in the ground rasters).
            for ring in it["openings"]:
                f.write(f"opening {it['id']} " + " ".join(f"{la:.8f},{lo:.8f}" for la, lo in ring) + "\n")
    print(f"residents {len(rows)}, pois {len(pois)} -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
