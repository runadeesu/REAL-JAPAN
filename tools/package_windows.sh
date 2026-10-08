#!/usr/bin/env bash
# Build the Windows x64 Release and assemble the distributables:
#   dist/RealJapan-<ver>-win64.zip          everything in one ZIP (extract and run RealJapan.exe)
#   dist/RealJapan-<ver>-win64-setup.exe    the same as a one-file installer (NSIS; INSTALLER=0 skips it)
#   dist/RealJapan-<ver>-win64-partN.zip    with PARTS=1 also the ZIP in parts of ~29 MB (size-limited transfers)
#   Prerequisites: mingw-w64, cmake, ninja, python3, nsis (makensis) for the installer
#   Steps: tools/fetch_deps.sh -> pipeline/cook_country.py (-> pipeline/fetch_plateau.py ->
#          pipeline/cook_slice.py for the Shibuya world, optional) -> this script
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="1.0.1"
NAME="RealJapan-${VER}-win64"
BUILD="$ROOT/build-win"
DIST="$ROOT/dist/$NAME"
PARTS="${PARTS:-0}"
INSTALLER="${INSTALLER:-1}"
export PARTS

test -f "$ROOT/game/data/world/country/client.txt" || { echo "fictional country missing: run pipeline/cook_country.py"; exit 1; }
test -f "$ROOT/game/data/world/shibuya/client.txt" || echo "note: the Shibuya world is not cooked (pipeline/cook_slice.py); packaging without it"
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

# The ZIP (and, with PARTS=1, the parts: part1 holds the program, settings, languages and fonts; the
# world cells (.rjcell + .rjdet of a cell stay together) are packed greedily by compressed size).
(cd "$ROOT/dist" && rm -f "$NAME".zip "$NAME"-part*.zip "$NAME"-setup.exe && python3 "$ROOT/tools/zip_dist.py" "$NAME")

if [ "$INSTALLER" = "1" ]; then
  # one-file installer: per-user install (no administrator rights), Start menu and desktop shortcuts,
  # an uninstaller; the saves and settings in %APPDATA%\RealJapan are left alone
  makensis -V2 -DVER="$VER" -DSRC="$DIST" -DICON="$ROOT/client/res/realjapan.ico" -DOUT="$ROOT/dist/$NAME-setup.exe" "$ROOT/packaging/windows/installer.nsi"
fi
(cd "$ROOT/dist" && sha256sum $(ls "$NAME".zip "$NAME"-part*.zip "$NAME"-setup.exe 2>/dev/null) > "$NAME.sha256" && ls -la "$NAME"* && cat "$NAME.sha256")
echo "packaged: $ROOT/dist/$NAME.zip"
