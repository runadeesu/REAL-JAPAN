# ビルド手順

開発は Linux（Ubuntu 24.04 で確認）で行い、Windows x64 向け EXE は MinGW-w64 で、Android 向け APK は Android NDK でクロスコンパイルする。

## 1. 必要なもの

```bash
sudo apt-get install -y build-essential cmake ninja-build git python3 python3-pip \
  mingw-w64 nsis libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev
pip install numpy pillow mapbox-earcut shapely scipy fonttools pytest
# 任意（Windows EXE の起動確認）: sudo apt-get install -y wine64 xvfb
```

## 2. 依存ソースとフォント

```bash
tools/fetch_deps.sh        # third_party/raylib (5.5) と BIZ UDPゴシック (OFL) を取得し、フォントを
                           # JIS 第1・第2水準＋ゲーム内の全文字にサブセット化（要 pip install fonttools）
                           # raylib には OpenGL ES 3.0 / Android 用の小さな修正を当てる（tools/patch_raylib.py）
```

## 3. 実データの取得とクック

```bash
python3 pipeline/fetch_plateau.py pipeline/slices/shibuya.json   # PLATEAU CityGML（約 470 MB）
python3 pipeline/cook_slice.py pipeline/slices/shibuya.json      # 写真テクスチャ（約 120 MB）と
                                                                 # 国土地理院 DEM タイルも取得してクック
```
出力：`game/data/world/shibuya/`（`cells/*.rjcell`, `client.txt`, `sources.json`, `pois.json`, `residents.csv`, `slice.json`）。
生データは `data/raw/`（git 管理外）。写真テクスチャ無しで速く試す場合は `--no-textures`。

### 架空の国「秋津国」（既定のワールド）

```bash
python3 pipeline/cook_country.py              # 全国を生成して game/data/world/country/ へ
                                              # 4 プロセス並列で約 45 分（地形と区画 11 分＋セル 30 分）、1,507 セル・約 535 MB（外部データ不要）
                                              # --cache layout.pkl で地形と区画を保存・再利用（2 回目からは約 33 分）
python3 pipeline/cook_country.py --preview --only 50405559,50405569   # 地形 40 m の簡易版で一部だけ（確認用）
python3 pipeline/tools_preview_country.py out.png                     # 国全体の地図のプレビュー
```
設計と出力ファイルの説明は [COUNTRY.md](COUNTRY.md)。

## 4. コアのテスト

```bash
cmake -S core -B core/build -G Ninja && cmake --build core/build && ./core/build/rjcore_tests
```

## 5. クライアント

Linux（開発用）:
```bash
cmake -S client -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux
./build-linux/RealJapan
```

Windows x64（Release, 配布用）:
```bash
tools/package_windows.sh
# -> dist/RealJapan-0.8.0-win64.zip         1 本の ZIP（RealJapan.exe, data/, README_ja/en.txt, LICENSES/）
#    dist/RealJapan-0.8.0-win64-setup.exe   1 本のインストーラ（NSIS: makensis。INSTALLER=0 で作らない）
#    PARTS=1 で ZIP を約 29 MB ごとに分けたもの（-partN.zip）も作る
```
インストーラはユーザー単位（管理者権限不要）で `%LOCALAPPDATA%\Programs\RealJapan` に入れ、スタートメニューと
デスクトップにショートカット、アンインストーラを作る（`packaging/windows/installer.nsi`）。セーブと設定
（`%APPDATA%\RealJapan`）はインストール・更新・アンインストールで消さない。

Windows と Android をひとつに（`tools/package_all.sh`、先に下の Android の `--single` も作る）:
```bash
tools/package_all.sh        # -> dist/RealJapan-0.8.0-all.zip（Windows/…-setup.exe, Android/…apk, README）
```

GitHub の下書きリリース（ソースから全部を作る）：Actions の「release」ワークフロー（`.github/workflows/release.yml`）を
手動で実行すると、依存の取得・コアのテスト・秋津国のクック・（任意で）渋谷の取得とクック・Linux 版の selftest・
Windows の ZIP とインストーラ・Android の APK・ひとまとめの ZIP を作り、**下書き**のリリースに置く（公開は手動）。

OpenGL ES 3.0 の描画とタッチ操作を Linux で試す（Android 版と同じコード。マウス左ドラッグが指 1 本、
視点固定中は Alt＋ドラッグ）:
```bash
cmake -S client -B build-gles -G Ninja -DCMAKE_BUILD_TYPE=Release -DRJ_GLES=ON
cmake --build build-gles --target RealJapan && ln -sfn ../game/data build-gles/data
```

Android（arm64-v8a, OpenGL ES 3.0, Android 7.0 以上）:
```bash
# 必要: Android NDK r26（ANDROID_NDK, 既定 /usr/lib/android-ndk）、aapt・zipalign・apksigner、
#       keytool（JDK）、android.jar（ANDROID_JAR, 既定 /usr/lib/android-sdk/platforms/android-23/android.jar）
#       Ubuntu: sudo apt-get install -y google-android-ndk-r26c-installer aapt zipalign apksigner \
#                 libandroid-23-java openjdk-21-jre-headless
python3 tools/package_android.py --single # 全部入りの 1 本だけ（約 560 MB）
# -> dist/RealJapan-0.8.0-android-arm64-full.apk   インストール: adb install <apk>
python3 tools/package_android.py          # 本体＋スプリット APK（各 30 MB 未満）、--full で 1 本も
# -> dist/RealJapan-0.8.0-android/
#      RealJapan-0.8.0-android-arm64.apk     本体（libmain.so・設定・言語・フォント・地図）
#      RealJapan-0.8.0-android-dataNN.apk    街のセル（スプリット APK）
#    インストール: adb install-multiple dist/RealJapan-0.8.0-android/*.apk
```
Java のコードは無く、NativeActivity が `libmain.so`（raylib の android_main → main()）を読み込む。
ゲームデータは APK の中から AAssetManager で直接読む（`platform/paths.cpp` の `apk:/data`）。
署名は `packaging/android/debug.keystore`（リポジトリに置いた公開のデバッグ鍵。新しい版を上書き
インストールしてセーブを残すため。パスワード `android`）。

## 6. 自動スクリーンショット（起動確認用）

秋津国の各機能・各地方：`tools/shots_country.sh build-linux <出力先>`（`ONLY="air_shion ride_main"` で一部だけ）。

```bash
xvfb-run -s "-screen 0 1600x900x24" ./build-linux/RealJapan \
  --state game --time 2026-09-26T15:30 --screenshot shot.png --frames 30
# Windows EXE を Wine で:
xvfb-run -s "-screen 0 1600x900x24" wine build-win/RealJapan.exe --state title --screenshot shot.png
```
オプション：`--state title|game|walk|drive|pause|phone:<アプリ>|settings|credits`（アプリ：map town clock wallet work hobby messages call transit camera music shopping delivery taxi flights hotel sns flat bag）、
`--time YYYY-MM-DDTHH:MM`（日本時間）、`--pos 緯度,経度`、`--yaw 度`、`--pitch 度`、`--fly --alt m`、`--third-person`、`--lang ja|en`。

テスト用の環境変数（撮影・確認用）：`RJ_LIFE=umbrella,light,hungry,wet,home,guitar,band`（持ち物・自宅・ギター・バンド）、
`RJ_TALK_TEST=1`（近くの人と会話）、`RJ_VEND_TEST=1 RJ_VEND_LOOK=1`（近くの自販機の前へ）、`RJ_BUSK_TEST=1`（ギターを自動で弾く）、
`RJ_MOTION_BLUR=1`、`RJ_NO_WAVES=1`。
