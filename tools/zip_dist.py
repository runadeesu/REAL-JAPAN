#!/usr/bin/env python3
"""Zip a dist folder: <name>.zip with everything; with PARTS=1 in the environment also
<name>-partN.zip of at most ~29 MB compressed each (part1: everything but the world cells; the
cells - a cell's .rjcell and .rjdet together - packed greedily by compressed size). All parts
extract into the same <name> folder.

usage (in dist/): python3 tools/zip_dist.py RealJapan-<ver>-win64
"""
import os
import sys
import zipfile
import zlib


def main() -> int:
    name = sys.argv[1]
    files = []
    for dp, _, fs in os.walk(name):
        for f in sorted(fs):
            files.append(os.path.join(dp, f))

    def write(zname, sel):
        with zipfile.ZipFile(zname, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for p in sorted(sel):
                z.write(p, p)

    write(name + ".zip", files)
    print(f"{name}.zip: {len(files)} files, {os.path.getsize(name + '.zip') / 1048576:.1f} MB")
    if os.environ.get("PARTS", "0") != "1":
        return 0

    def csize(p):
        with open(p, "rb") as fh:
            return len(zlib.compress(fh.read(), 9))

    groups, base = {}, []
    for p in files:
        if p.endswith(".rjcell") or p.endswith(".rjdet"):
            groups.setdefault(os.path.splitext(p)[0], []).append(p)
        else:
            base.append(p)
    budget = 29 * 1024 * 1024
    parts = [[base, sum(csize(p) for p in base)]]
    for key in sorted(groups, key=lambda k: -sum(csize(p) for p in groups[k])):
        g = groups[key]
        s = sum(csize(p) for p in g)
        for part in parts:
            if part[1] + s <= budget:
                part[0] = part[0] + g
                part[1] += s
                break
        else:
            parts.append([list(g), s])
    for i, (sel, s) in enumerate(parts):
        write("%s-part%d.zip" % (name, i + 1), sel)
        print("part%d: %d files, ~%.1f MB compressed" % (i + 1, len(sel), s / 1048576))
    return 0


if __name__ == "__main__":
    sys.exit(main())
