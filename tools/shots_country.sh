#!/usr/bin/env bash
# Feature screenshots in the fictional country 秋津国 (PROJECT: REAL JAPAN).
# usage: tools/shots_country.sh <exe-dir> <out-dir> [extra args...]
# Each shot scripts the game to a moment (drive, ride, fly ...) using the test aids
# (--state drive|ride|ferry|jet|fly, --simspeed) and captures the frame. --ride is game time;
# frames are capped at 0.1 s, so slow software rendering needs a high --simspeed.
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
want drive_cockpit && shot drive_cockpit "RJ_DRIVE_FP=1" --state drive --pos 33.78650,140.99643 --yaw 0 --drive "1:0:5,0.4:0:1"
want drive_chase && shot drive_chase "" --state drive --pos 33.78650,140.99643 --yaw 0 --drive "1:0:5,0.4:0.25:1.2"
want drive_side && shot drive_side "RJ_DRIVE_LOOK=70" --state drive --pos 33.78650,140.99643 --yaw 0 --drive "1:0:3,0.3:0:1"
# Shinkansen: the train at the terminal platform (doors open), window seat after departure;
# main-line train standing at its terminal; loop line ride and the driver's cab.
# Station indices (rail.txt): loop 0-3, main line 4-10, Shinkansen 11-14, Yukimi Shinkansen 15-17
want shinkansen_platform && shot shinkansen_platform "RJ_PLATFORM_LOOK=-22,40,1.6" --state platform --station 11
want branch_platform && SHOT_T=2026-09-28T08:10 shot branch_platform "RJ_PLATFORM_LOOK=16,56,0.6" --state platform --station 4
want ride_shinkansen && shot ride_shinkansen "RJ_RIDE_LOOK=-1.15,-0.06" --state ride --station 11 --ride 85 --simspeed 10
want ride_loop && SHOT_T=2026-09-28T08:20 shot ride_loop "" --state ride --station 0 --ride 40 --simspeed 16
want train_cab && shot train_cab "" --state trainjob --station 0 --ride 45 --simspeed 18
# main line through the countryside, Yukimi Shinkansen out of the capital
want ride_main && shot ride_main "" --state ride --station 6 --ride 60 --simspeed 12
want ride_yukimi && shot ride_yukimi "RJ_RIDE_LOOK=-1.15,-0.06" --state ride --station 15 --ride 120 --simspeed 16
# the regions from the air (fly mode, north = yaw 0)
want air_capital && shot air_capital "" --state walk --fly --alt 420 --pitch -20 --pos 33.7700,140.9860 --yaw 20
want air_shion && shot air_shion "" --state walk --fly --alt 380 --pitch -22 --pos 33.7880,140.7420 --yaw 0
want air_asanagi && shot air_asanagi "" --state walk --fly --alt 380 --pitch -20 --pos 33.7430,140.6300 --yaw 305
want air_onsen && shot air_onsen "" --state walk --fly --alt 120 --pitch -12 --pos 33.902743,140.908098 --yaw 280
want air_volcano && shot air_volcano "" --state walk --fly --alt 1300 --pitch -8 --pos 33.8900,140.8800 --yaw 320
want air_paddies && SHOT_T=2026-09-20T10:00 shot air_paddies "" --state walk --fly --alt 60 --pitch -14 --pos 33.8150,140.8700 --yaw 270
want air_yukimi_winter && SHOT_T=2027-01-20T11:00 shot air_yukimi_winter "" --state walk --fly --alt 260 --pitch -16 --pos 34.0450,140.8300 --yaw 190
# ferry: alongside the pier (from the pier), on deck under way
want ferry_view && shot ferry_view "" --state ferryview --station 0
want ferry_deck && shot ferry_deck "" --state ferry --station 0 --ride 160 --simspeed 16
# scheduled flight: window seat during the climb
want jet_window && shot jet_window "RJ_JET_LOOK=-1.35,-0.42" --state jet --ride 430 --simspeed 72
# light aircraft: after take-off, chase and cockpit
want plane_chase && shot plane_chase "" --state fly --simspeed 8 --fly-script "1:0:0:15,1:0.5:0:3,1:0.12:0:10"
want plane_cockpit && shot plane_cockpit "RJ_FLY_COCKPIT=1" --state fly --simspeed 8 --fly-script "1:0:0:15,1:0.5:0:3,1:0.12:0:10"
# phone apps
want phone_work && shot phone_work "" --state phone:work
want phone_hobby && shot phone_hobby "" --state phone:hobby
want phone_map && shot phone_map "" --state phone:map
ls "$OUT"
