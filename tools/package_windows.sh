#!/usr/bin/env bash
# Build the Windows x64 Release and assemble the distributable ZIP.
#   Prerequisites: mingw-w64, cmake, ninja, python3 (+ numpy pillow mapbox-earcut for cooking)
#   Steps: tools/fetch_deps.sh -> pipeline/fetch_plateau.py -> pipeline/cook_slice.py -> this script
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="0.1.0"
NAME="RealJapan-${VER}-win64"
BUILD="$ROOT/build-win"
DIST="$ROOT/dist/$NAME"

test -f "$ROOT/game/data/world/shibuya/client.txt" || { echo "cooked world missing: run pipeline/cook_slice.py"; exit 1; }
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

(cd "$ROOT/dist" && rm -f "$NAME.zip" && python3 - "$NAME" <<'EOF'
import os, sys, zipfile
name = sys.argv[1]
with zipfile.ZipFile(name + ".zip", "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for dp, _, files in os.walk(name):
        for f in sorted(files):
            p = os.path.join(dp, f)
            z.write(p, p)
EOF
)
(cd "$ROOT/dist" && sha256sum "$NAME.zip" > "$NAME.zip.sha256" && ls -la "$NAME.zip" && cat "$NAME.zip.sha256")
echo "packaged: $ROOT/dist/$NAME.zip"
