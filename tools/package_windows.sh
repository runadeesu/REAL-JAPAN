#!/usr/bin/env bash
# Build the Windows x64 Release and assemble the distributable ZIP.
#   Prerequisites: mingw-w64, cmake, ninja, python3 (+ numpy pillow mapbox-earcut for cooking)
#   Steps: tools/fetch_deps.sh -> pipeline/fetch_plateau.py -> pipeline/cook_slice.py -> this script
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="0.4.0"
NAME="RealJapan-${VER}-win64"
BUILD="$ROOT/build-win"
DIST="$ROOT/dist/$NAME"

test -f "$ROOT/game/data/world/shibuya/client.txt" || { echo "cooked world missing: run pipeline/cook_slice.py"; exit 1; }
test -f "$ROOT/game/data/world/island/client.txt" || { echo "fictional island missing: run pipeline/cook_island.py"; exit 1; }
test -f "$ROOT/game/data/fonts/BIZUDPGothic-Regular.ttf" || { echo "font missing: run tools/fetch_deps.sh"; exit 1; }

cmake -S "$ROOT/client" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$ROOT/client/cmake/mingw-w64-x86_64.cmake"
cmake --build "$BUILD"

rm -rf "$DIST"
mkdir -p "$DIST/LICENSES"
cp "$BUILD/RealJapan.exe" "$DIST/"
cp -r "$ROOT/game/data" "$DIST/data"
cp "$ROOT/packaging/windows/README_ja.txt" "$DIST/README_ja.txt"
cp "$ROOT/packaging/windows/README_en.txt" "$DIST/README_en.txt"
cp "$ROOT/packaging/windows/DATA_SOURCES.txt" "$DIST/LICENSES/DATA_SOURCES.txt"
cp "$ROOT/third_party/raylib/LICENSE" "$DIST/LICENSES/raylib_LICENSE.txt"
cp "$ROOT/game/data/fonts/OFL.txt" "$DIST/LICENSES/BIZ_UDPGothic_OFL.txt"
# Windows line endings for the text files users will open in Notepad.
for f in "$DIST"/README_*.txt "$DIST"/LICENSES/*.txt; do sed -i 's/\r\?$/\r/' "$f"; done

# One full ZIP, plus the same content in parts of at most ~29 MB each (size-limited transfers):
# part1 holds the program, settings, languages and fonts; the world cells (.rjcell + .rjdet of a
# cell stay together) are packed greedily by their compressed size. All parts extract into the same
# RealJapan-<ver>-win64 folder; the game reports missing cells if a part was not extracted.
(cd "$ROOT/dist" && rm -f "$NAME".zip "$NAME"-part*.zip && python3 - "$NAME" <<'EOF'
import os, sys, zipfile, zlib
name = sys.argv[1]
files = []
for dp, _, fs in os.walk(name):
    for f in sorted(fs):
        files.append(os.path.join(dp, f))
def csize(p):
    with open(p, "rb") as fh:
        return len(zlib.compress(fh.read(), 9))
groups = {}
base = []
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
def write(zname, sel):
    with zipfile.ZipFile(zname, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(sel):
            z.write(p, p)
write(name + ".zip", files)
for i, (sel, s) in enumerate(parts):
    write("%s-part%d.zip" % (name, i + 1), sel)
    print("part%d: %d files, ~%.1f MB compressed" % (i + 1, len(sel), s / 1048576))
EOF
)
(cd "$ROOT/dist" && sha256sum "$NAME".zip "$NAME"-part*.zip > "$NAME.sha256" && ls -la "$NAME"*.zip && cat "$NAME.sha256")
echo "packaged: $ROOT/dist/$NAME.zip"
