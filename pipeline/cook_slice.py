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
from realjapan_pipeline.citygml import iter_buildings, iter_roads  # noqa: E402
from realjapan_pipeline.dem_gsi import DemSampler  # noqa: E402
from realjapan_pipeline.geodesy import LocalFrame, Mesh  # noqa: E402
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


def building_mesh(b, frame: LocalFrame):
    polys = b.lod2 if b.lod2 else b.lod1
    j = jitter(b.gml_id)
    P, N, C, I = [], [], [], []
    base = 0
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
        P.append(v)
        N.append(np.repeat(n[None, :], len(v), axis=0))
        C.append(np.repeat(np.array([color_for(kind, b.usage, j)], dtype=np.uint8), len(v), axis=0))
        I.append(t.reshape(-1) + base)
        base += len(v)
    if not P:
        return None
    pos, nrm, col, idx = np.vstack(P), np.vstack(N), np.vstack(C), np.concatenate(I)
    ground = float(pos[:, 2].min())
    # Baked sky-occlusion gradient: darker near the street, full brightness higher up.
    occl = 0.70 + 0.30 * np.clip((pos[:, 2] - ground) / 12.0, 0.0, 1.0)
    col[:, :3] = (col[:, :3].astype(np.float64) * occl[:, None]).clip(0, 255).astype(np.uint8)
    # Skirt: extend the footprint 2 m below the base so buildings never float on coarse terrain.
    if b.footprint and len(b.footprint) >= 3:
        fp = to_local(frame, b.footprint)[:, :2]
        wc = np.array(color_for("wall", b.usage, j), dtype=np.uint8)
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
            pos = np.vstack([pos, np.array(sp)])
            nrm = np.vstack([nrm, np.array(sn)])
            col = np.vstack([col, np.repeat(wc[None, :], len(sp), axis=0)])
            idx = np.concatenate([idx, np.array(si, dtype=np.int64) + base])
    return pos, nrm, col, idx, ground


def rasterize_ground(bounds, roads, buildings) -> bytes:
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
    for b in buildings:
        if b.footprint:
            d.polygon(px(b.footprint), fill=GROUND_COLORS["footprint"])
    img = img.resize((TEX, TEX), Image.LANCZOS)
    buf = io.BytesIO()
    img.save(buf, format="PNG", optimize=True)
    return buf.getvalue()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("slice")
    ap.add_argument("--raw", default=os.path.join(ROOT, "data", "raw"))
    ap.add_argument("--out", default=None)
    ap.add_argument("--terrain-n", type=int, default=129)
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

        n_lod2 = 0
        for b in blds:
            res = building_mesh(b, frame)
            if res is None:
                continue
            pos, nrm, col, idx, ground = res
            if len(pos) > CellWriter.MAX_VERTS:
                continue
            ch, first, cnt = w.add_geometry(pos, nrm, col, idx)
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
        w.ground_png = rasterize_ground(bounds, roads, blds)

        data = w.to_bytes()
        with open(os.path.join(out, "cells", f"{mesh}.rjcell"), "wb") as f:
            f.write(data)
        summary.append({"mesh": mesh, "buildings": len(w.buildings), "lod2": n_lod2, "roads": len(roads),
                        "bytes": len(data), "anchor": [clat, clon, anchor_h], "bounds": bounds})
        print(f"{mesh}: {len(w.buildings)} buildings ({n_lod2} LOD2), {len(roads)} roads, "
              f"{len(data) / 1e6:.1f} MB, {time.time() - t0:.1f}s", flush=True)

    # Fictional residents placed in real residential buildings; jobs in real workplaces.
    rng = np.random.default_rng(20260926)
    home_w = np.array([h["storeys"] * max(h["area"], 30.0) for h in residents_homes])
    work_w = np.array([wp["storeys"] * max(wp["area"], 30.0) for wp in workplaces])
    occupations_by_usage = {
        401: ["office_worker", "engineer", "programmer", "designer", "executive", "lawyer", "architect"],
        402: ["shop_clerk", "cook", "hairdresser", "patissier"], 403: ["shop_clerk", "cook"],
        404: ["shop_clerk", "office_worker", "cook"], 421: ["civil_servant", "police_officer"],
        422: ["teacher", "nurse", "doctor", "childcare_worker"], 431: ["truck_driver", "delivery_rider"],
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
    with open(os.path.join(out, "residents.csv"), "w", newline="", encoding="utf-8") as f:
        cw = csv.writer(f)
        cw.writerow(["id", "occupation", "home_id", "home_lat", "home_lon", "work_id", "work_lat", "work_lon"])
        cw.writerows(rows)

    meta = {"id": cfg["id"], "name_ja": cfg["name_ja"], "name_en": cfg["name_en"], "cells": summary,
            "core_bbox": cfg["core_bbox"], "spawn": cfg["spawn"], "pipeline_version": PIPELINE_VERSION,
            "generated_at": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
            "notes": ["Heights: T.P. (orthometric). Geoid undulation not applied (GSIGEO2011 not bundled); "
                      "relative placement inside the slice is unaffected.",
                      "Residents are fictional people placed in real residential buildings (weighted by floor area). "
                      "Calibration against census data is TODO."]}
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
    print(f"residents {len(rows)}, pois {len(pois)} -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
