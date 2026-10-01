#!/usr/bin/env bash
# Everything in one file: dist/RealJapan-<ver>-all.zip with
#   Windows/RealJapan-<ver>-win64-setup.exe   the one-file installer
#   Android/RealJapan-<ver>-android-arm64.apk  the one APK with everything
#   README_ja.txt / README_en.txt              what is in it and how to install
# Run tools/package_windows.sh and `python3 tools/package_android.py --single` first (or let this
# script run them: BUILD=1).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VER="0.8.0"
if [ "${BUILD:-0}" = "1" ]; then
  "$ROOT/tools/package_windows.sh"
  python3 "$ROOT/tools/package_android.py" --single
fi
WIN="$ROOT/dist/RealJapan-${VER}-win64-setup.exe"
APK="$ROOT/dist/RealJapan-${VER}-android-arm64-full.apk"
test -f "$WIN" || { echo "missing $WIN: run tools/package_windows.sh"; exit 1; }
test -f "$APK" || { echo "missing $APK: run python3 tools/package_android.py --single"; exit 1; }
OUT="$ROOT/dist/RealJapan-${VER}-all.zip"
rm -f "$OUT"
python3 - "$OUT" "$WIN" "$APK" "$ROOT/packaging/README_ALL_ja.txt" "$ROOT/packaging/README_ALL_en.txt" "$VER" <<'PY'
import sys, zipfile
out, win, apk, rja, ren, ver = sys.argv[1:]
with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED, allowZip64=True) as z:  # (the installer and the APK are compressed already)
    z.write(win, f"Windows/RealJapan-{ver}-win64-setup.exe")
    z.write(apk, f"Android/RealJapan-{ver}-android-arm64.apk")
    for src, name in ((rja, "README_ja.txt"), (ren, "README_en.txt")):
        text = open(src, encoding="utf-8").read().replace("{VER}", ver).replace("\r\n", "\n").replace("\n", "\r\n")
        z.writestr(name, text.encode("utf-8"))
PY
(cd "$ROOT/dist" && sha256sum "$(basename "$OUT")" | tee "RealJapan-${VER}-all.sha256" && ls -la "$(basename "$OUT")")
echo "packaged: $OUT"
