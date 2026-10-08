#!/usr/bin/env python3
"""Build the Android release (arm64-v8a, OpenGL ES 3.0) and assemble the APKs.

  Prerequisites: Android NDK r26 (ANDROID_NDK, default /usr/lib/android-ndk), cmake, ninja,
  aapt, zipalign, apksigner, keytool (a JDK), and an android.jar to link resources against
  (ANDROID_JAR, default /usr/lib/android-sdk/platforms/android-23/android.jar). The game data
  must be cooked first (as for tools/package_windows.sh).

  Output in dist/RealJapan-<ver>-android/:
    RealJapan-<ver>-android-arm64.apk        the program, settings, languages, fonts and the
                                             worlds' maps (everything but the city cells)
    RealJapan-<ver>-android-data01.apk ...   the city cells, as split APKs of at most ~29 MB
                                             (size-limited transfers); installed together with
                                             the main APK (a split-APK installer or
                                             `adb install-multiple *.apk`)
    --full also writes one APK with everything (~560 MB) for `adb install`; --single writes only
    that one APK (dist/RealJapan-<ver>-android-arm64-full.apk), no split APKs.

  The game reads its data straight from the installed APKs (nothing is copied on the first
  start). All APKs are signed with packaging/android/debug.keystore, a public debug key kept in
  the repository so that a newer build installs over an older one (and keeps the saves).
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VER = "1.0.1"
VCODE = 101
PKG = "io.github.runadeesu.realjapan"
MIN_SDK, TARGET_SDK = 24, 34
SPLIT_BUDGET = int(28.5 * 1024 * 1024)  # file bytes per data APK (stored, the cells are compressed already)


def run(cmd, **kw):
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, **kw)


def build_lib(ndk: str) -> str:
    subprocess.run([sys.executable, os.path.join(ROOT, "tools", "patch_raylib.py")], check=True)
    b = os.path.join(ROOT, "build-android")
    run(["cmake", "-S", os.path.join(ROOT, "client"), "-B", b, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         f"-DCMAKE_TOOLCHAIN_FILE={ndk}/build/cmake/android.toolchain.cmake", "-DANDROID_ABI=arm64-v8a",
         f"-DANDROID_PLATFORM=android-{MIN_SDK}", "-DANDROID_STL=c++_static"])
    run(["cmake", "--build", b, "--target", "RealJapan"])
    lib = os.path.join(b, "libmain.so")
    stripped = os.path.join(b, "libmain.stripped.so")
    strip = os.path.join(ndk, "toolchains", "llvm", "prebuilt", "linux-x86_64", "bin", "llvm-strip")
    run([strip, "--strip-unneeded", "-o", stripped, lib])
    return stripped


def is_cell(rel: str) -> bool:
    return rel.endswith(".rjcell") or rel.endswith(".rjdet")


def data_files():
    base = os.path.join(ROOT, "game", "data")
    out = []
    for dp, _, fs in os.walk(base):
        for f in sorted(fs):
            p = os.path.join(dp, f)
            out.append((os.path.relpath(p, base).replace(os.sep, "/"), p))
    return sorted(out)


def stage(dst: str, files):
    """assets/data/<rel> for each (rel, src)."""
    for rel, src in files:
        t = os.path.join(dst, "assets", "data", rel)
        os.makedirs(os.path.dirname(t), exist_ok=True)
        try:
            os.link(src, t)  # (hard link: no copy of half a gigabyte)
        except OSError:
            shutil.copy2(src, t)


def aapt_package(out_apk: str, manifest: str, assets: str | None, res: str | None, jar: str):
    cmd = ["aapt", "package", "-f", "-M", manifest, "-I", jar, "-F", out_apk, "-0", "",
           "--version-code", str(VCODE), "--version-name", VER,
           "--min-sdk-version", str(MIN_SDK), "--target-sdk-version", str(TARGET_SDK)]
    if res:
        cmd += ["-S", res]
    if assets:
        cmd += ["-A", assets]
    run(cmd)


def finish(unsigned: str, out_apk: str, ks: str):
    aligned = unsigned + ".aligned"
    run(["zipalign", "-f", "-p", "4", unsigned, aligned])
    run(["apksigner", "sign", "--ks", ks, "--ks-pass", "pass:android", "--key-pass", "pass:android",
         "--ks-key-alias", "realjapan", "--v4-signing-enabled", "false", "--out", out_apk, aligned])
    os.remove(aligned)
    os.remove(unsigned)
    run(["apksigner", "verify", out_apk])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--full", action="store_true", help="also one APK with everything (~560 MB)")
    ap.add_argument("--single", action="store_true", help="only the one APK with everything (no split APKs)")
    ap.add_argument("--no-build", action="store_true", help="use build-android/libmain.so as it is")
    a = ap.parse_args()
    ndk = os.environ.get("ANDROID_NDK", "/usr/lib/android-ndk")
    jar = os.environ.get("ANDROID_JAR", "/usr/lib/android-sdk/platforms/android-23/android.jar")
    ks = os.path.join(ROOT, "packaging", "android", "debug.keystore")
    for need in ("game/data/world/country/client.txt", "game/data/fonts/BIZUDPGothic-Regular.ttf"):
        if not os.path.exists(os.path.join(ROOT, need)):
            print(f"missing {need}: cook the world / run tools/fetch_deps.sh first", file=sys.stderr)
            return 1
    if not os.path.exists(os.path.join(ROOT, "game/data/world/shibuya/client.txt")):
        print("note: the Shibuya world is not cooked; packaging without it", file=sys.stderr)

    if a.no_build:
        lib = os.path.join(ROOT, "build-android", "libmain.stripped.so")
        strip = os.path.join(ndk, "toolchains", "llvm", "prebuilt", "linux-x86_64", "bin", "llvm-strip")
        run([strip, "--strip-unneeded", "-o", lib, os.path.join(ROOT, "build-android", "libmain.so")])
    else:
        lib = build_lib(ndk)

    name = f"RealJapan-{VER}-android"
    dist = os.path.join(ROOT, "dist", name)
    work = os.path.join(ROOT, "build-android", "apk")
    shutil.rmtree(dist, ignore_errors=True)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(dist)
    os.makedirs(work)

    # resources: the strings and the launcher icon (the game's own)
    res = os.path.join(work, "res")
    shutil.copytree(os.path.join(ROOT, "packaging", "android", "res"), res)
    os.makedirs(os.path.join(res, "drawable-nodpi"))
    shutil.copy2(os.path.join(ROOT, "game", "data", "icon.png"), os.path.join(res, "drawable-nodpi", "icon.png"))
    manifest = os.path.join(ROOT, "packaging", "android", "AndroidManifest.xml")

    files = data_files()
    base_files = [f for f in files if not is_cell(f[0])]
    cells = {}
    for rel, src in files:
        if is_cell(rel):
            cells.setdefault(rel.rsplit(".", 1)[0], []).append((rel, src))

    def base_apk(out_apk, sel):
        d = os.path.join(work, os.path.basename(out_apk) + ".d")
        stage(d, sel)
        unsigned = os.path.join(work, os.path.basename(out_apk) + ".unsigned")
        aapt_package(unsigned, manifest, os.path.join(d, "assets"), res, jar)
        with zipfile.ZipFile(unsigned, "a", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            z.write(lib, "lib/arm64-v8a/libmain.so")
        finish(unsigned, out_apk, ks)
        shutil.rmtree(d)

    if a.single:
        base_apk(os.path.join(ROOT, "dist", f"{name}-arm64-full.apk"), files)
        print(f"packaged: {os.path.join(ROOT, 'dist', name + '-arm64-full.apk')}")
        return 0
    main_apk = os.path.join(dist, f"{name}-arm64.apk")
    base_apk(main_apk, base_files)

    # the cells in split APKs: a cell's .rjcell and .rjdet stay together, packed greedily
    parts = []
    for key in sorted(cells, key=lambda k: (-sum(os.path.getsize(s) for _, s in cells[k]), k)):
        g = cells[key]
        size = sum(os.path.getsize(s) for _, s in g)
        for p in parts:
            if p[1] + size <= SPLIT_BUDGET:
                p[0].extend(g)
                p[1] += size
                break
        else:
            parts.append([list(g), size])
    for i, (sel, size) in enumerate(parts, 1):
        split = f"data{i:02d}"
        d = os.path.join(work, split)
        stage(d, sel)
        man = os.path.join(d, "AndroidManifest.xml")  # (aapt wants that name)
        with open(man, "w", encoding="utf-8") as f:
            f.write('<?xml version="1.0" encoding="utf-8"?>\n'
                    '<!-- city cells for PROJECT: REAL JAPAN (installed with the main APK) -->\n'
                    f'<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="{PKG}" split="{split}">\n'
                    '    <application android:hasCode="false" />\n'
                    '</manifest>\n')
        unsigned = os.path.join(work, split + ".unsigned")
        aapt_package(unsigned, man, os.path.join(d, "assets"), None, jar)
        finish(unsigned, os.path.join(dist, f"{name}-{split}.apk"), ks)
        shutil.rmtree(d)
        print(f"{split}: {len(sel)} files, {size / 1048576:.1f} MB", flush=True)

    if a.full:
        base_apk(os.path.join(ROOT, "dist", f"{name}-arm64-full.apk"), files)

    for readme in ("README_ja.txt", "README_en.txt"):
        shutil.copy2(os.path.join(ROOT, "packaging", "android", readme), os.path.join(dist, readme))
    with open(os.path.join(dist, f"{name}.sha256"), "w") as f:
        for fn in sorted(os.listdir(dist)):
            if fn.endswith(".apk"):
                h = hashlib.sha256(open(os.path.join(dist, fn), "rb").read()).hexdigest()
                f.write(f"{h}  {fn}\n")
    for fn in sorted(os.listdir(dist)):
        print(f"{os.path.getsize(os.path.join(dist, fn)) / 1048576:8.1f} MB  {fn}")
    print(f"packaged: {dist}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
