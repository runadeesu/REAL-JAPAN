#!/usr/bin/env bash
# Eye-level quality check shots (PROJECT: REAL JAPAN).
# usage: tools/shots.sh <exe-dir> <out-dir> [extra args...]
# Each view: name lat lon yaw pitch; each taken at day (12:00) and night (21:00).
set -euo pipefail
BIN="$1"; OUT="$2"; shift 2
mkdir -p "$OUT"
VIEWS=(
  "avenue 35.66005 139.70246 0 2"      # 明治通り (宮益坂下付近)
  "station 35.65920 139.70060 313 2"   # 駅前 (ハチ公前広場 → スクランブル交差点)
  "shops 35.660685 139.699224 292 2"   # 店舗前 (宇田川町の商店街)
  "alley 35.659519 139.696681 307 2"   # 路地 (道玄坂二丁目, 幅員約4 m)
)
TIMES=("day 2026-09-26T12:00" "night 2026-09-26T21:00")
for v in "${VIEWS[@]}"; do
  read -r name lat lon yaw pitch <<<"$v"
  for t in "${TIMES[@]}"; do
    read -r tn tv <<<"$t"
    (cd "$BIN" && timeout 600 xvfb-run -a -s "-screen 0 1600x900x24" ./RealJapan --state game --pos "$lat,$lon" \
      --yaw "$yaw" --pitch "$pitch" --time "$tv" --screenshot "$OUT/${name}_${tn}.png" --frames 40 "$@" >/dev/null 2>&1) || echo "fail $name $tn"
  done
done
ls "$OUT"
