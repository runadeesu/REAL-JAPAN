#!/usr/bin/env bash
# Fetch pinned third-party sources into third_party/ (not committed).
#   raylib 5.5 (zlib/libpng licence)
#   BIZ UDPGothic font (SIL OFL 1.1) from github.com/google/fonts
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/third_party"
mkdir -p "$TP"

if [ ! -f "$TP/raylib/CMakeLists.txt" ]; then
  git clone --depth 1 --branch 5.5 https://github.com/raysan5/raylib.git "$TP/raylib"
fi
(cd "$TP/raylib" && test "$(git rev-parse HEAD)" = "c1ab645ca298a2801097931d1079b10ff7eb9df8") \
  || echo "warning: raylib is not the pinned 5.5 commit"

if [ ! -f "$ROOT/game/data/fonts/BIZUDPGothic-Regular.ttf" ]; then
  if [ ! -d "$TP/gfonts" ]; then
    git clone --depth 1 --filter=blob:none --sparse https://github.com/google/fonts.git "$TP/gfonts"
    (cd "$TP/gfonts" && git sparse-checkout set ofl/bizudpgothic)
  fi
  mkdir -p "$ROOT/game/data/fonts"
  # Subset (JIS X 0208 + kana + symbols + every character used by the game) to keep downloads small.
  python3 "$ROOT/tools/subset_font.py" "$TP/gfonts/ofl/bizudpgothic/BIZUDPGothic-Regular.ttf" \
    "$ROOT/game/data/fonts/BIZUDPGothic-Regular.ttf"
  cp "$TP/gfonts/ofl/bizudpgothic/OFL.txt" "$ROOT/game/data/fonts/OFL.txt"
fi
echo "dependencies ready"
