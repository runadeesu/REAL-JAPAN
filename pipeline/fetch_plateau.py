#!/usr/bin/env python3
"""Download the PLATEAU CityGML files for a slice, per mesh, via the PLATEAU data catalog API.

Usage: python3 pipeline/fetch_plateau.py pipeline/slices/shibuya.json [--types bldg,tran,ubld]
Files land in data/raw/plateau/ (git-ignored). Existing files are kept.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("slice")
    ap.add_argument("--types", default="bldg,tran,frn,ubld,veg")
    ap.add_argument("--out", default=os.path.join(ROOT, "data", "raw", "plateau"))
    a = ap.parse_args()
    cfg = json.load(open(a.slice, encoding="utf-8"))
    city = cfg["plateau"]["city_code"]
    types = a.types.split(",")
    os.makedirs(a.out, exist_ok=True)
    manifest = []
    for mesh in cfg["cells"]:
        url = cfg["plateau"]["catalog_api"].format(mesh=mesh)
        with urllib.request.urlopen(url, timeout=60) as r:
            info = json.load(r)
        for c in info["cities"]:
            if c["cityCode"] != city:
                continue
            for t in types:
                for f in c["files"].get(t, []):
                    if f["code"] != mesh:
                        continue
                    dest = os.path.join(a.out, os.path.basename(f["url"]))
                    manifest.append({"mesh": mesh, "type": t, "url": f["url"], "file": os.path.basename(dest),
                                     "year": c["year"], "spec": c["spec"], "max_lod": f["maxLod"]})
                    if os.path.exists(dest) and os.path.getsize(dest) == f["fileSize"]:
                        print("have", dest)
                        continue
                    print("get ", f["url"])
                    urllib.request.urlretrieve(f["url"], dest)
    with open(os.path.join(a.out, "manifest.json"), "w", encoding="utf-8") as fp:
        json.dump(manifest, fp, ensure_ascii=False, indent=1)
    print(f"{len(manifest)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
