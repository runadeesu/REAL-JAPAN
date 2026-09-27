#!/usr/bin/env bash
# Feature screenshots on the fictional island (PROJECT: REAL JAPAN).
# usage: tools/shots_island.sh <exe-dir> <out-dir> [extra args...]
# Each shot scripts the game to a moment (drive, ride, fly ...) using the test aids
# (--state drive|ride|ferry|jet|fly, --simspeed) and captures the frame.
set -uo pipefail
BIN="$1"; OUT="$2"; shift 2
mkdir -p "$OUT"
T="2026-09-26T11:00"
shot() {  # name, env, args...   (SHOT_T=... overrides the time for one shot)
  local name="$1" envs="$2"; shift 2
  (cd "$BIN" && env $envs LANG=C.UTF-8 LC_ALL=C.UTF-8 timeout 2400 xvfb-run -a -s "-screen 0 1600x900x24" ./RealJapan "$@" \
    --time "${SHOT_T:-$T}" --weather clear --screenshot "$OUT/$name.png" --frames 2 >/dev/null 2>&1) || echo "fail $name"
}
ONLY="${ONLY:-}"  # optional: space-separated shot names to (re)take
want() { [ -z "$ONLY" ] || [[ " $ONLY " == *" $1 "* ]]; }
# car: driver's seat on the central avenue (mirrors show the real rear view), chase view, side view
want drive_cockpit && shot drive_cockpit "RJ_DRIVE_FP=1" --state drive --pos 34.58650,140.39640 --yaw 0 --drive "1:0:5,0.4:0:1"
want drive_chase && shot drive_chase "" --state drive --pos 34.58650,140.39640 --yaw 0 --drive "1:0:5,0.4:0.25:1.2"
want drive_side && shot drive_side "RJ_DRIVE_LOOK=70" --state drive --pos 34.58650,140.39640 --yaw 0 --drive "1:0:3,0.3:0:1"
# Shinkansen: the train at the terminal platform (doors open), window seat after departure;
# branch-line train standing at its terminal; loop line ride and the driver's cab
want shinkansen_platform && shot shinkansen_platform "RJ_PLATFORM_LOOK=-22,40,1.6" --state platform --station 6
want branch_platform && SHOT_T=2026-09-28T08:10 shot branch_platform "RJ_PLATFORM_LOOK=16,56,0.6" --state platform --station 4
want ride_shinkansen && shot ride_shinkansen "" --state ride --station 6 --ride 85 --simspeed 3
want ride_loop && SHOT_T=2026-09-28T08:20 shot ride_loop "" --state ride --station 0 --ride 40 --simspeed 8
want train_cab && shot train_cab "" --state trainjob --station 0 --ride 45 --simspeed 8
# ferry: alongside the pier (from the pier), on deck under way
want ferry_view && shot ferry_view "" --state ferryview --station 0
want ferry_deck && shot ferry_deck "" --state ferry --station 0 --ride 160 --simspeed 4
# scheduled flight: window seat during the climb
want jet_window && shot jet_window "" --state jet --ride 430 --simspeed 10
# light aircraft: after take-off, chase and cockpit
want plane_chase && shot plane_chase "" --state fly --simspeed 8 --fly-script "1:0:0:15,1:0.5:0:3,1:0.12:0:10"
want plane_cockpit && shot plane_cockpit "RJ_FLY_COCKPIT=1" --state fly --simspeed 8 --fly-script "1:0:0:15,1:0.5:0:3,1:0.12:0:10"
# phone apps
want phone_work && shot phone_work "" --state phone:work
want phone_hobby && shot phone_hobby "" --state phone:hobby
ls "$OUT"
