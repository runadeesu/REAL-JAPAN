# ビルド手順

開発は Linux（Ubuntu 24.04 で確認）で行い、Windows x64 向け EXE は MinGW-w64 でクロスコンパイルする。

## 1. 必要なもの

```bash
sudo apt-get install -y build-essential cmake ninja-build git python3 python3-pip \
  mingw-w64 libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev
pip install numpy pillow mapbox-earcut fonttools pytest
# 任意（Windows EXE の起動確認）: sudo apt-get install -y wine64 xvfb
```

## 2. 依存ソースとフォント

```bash
tools/fetch_deps.sh        # third_party/raylib (5.5) と BIZ UDPゴシック (OFL) を取得し、フォントを
                           # JIS 第1・第2水準＋ゲーム内の全文字にサブセット化（要 pip install fonttools）
```

## 3. 実データの取得とクック

```bash
python3 pipeline/fetch_plateau.py pipeline/slices/shibuya.json   # PLATEAU CityGML（約 470 MB）
python3 pipeline/cook_slice.py pipeline/slices/shibuya.json      # 写真テクスチャ（約 120 MB）と
                                                                 # 国土地理院 DEM タイルも取得してクック
```
出力：`game/data/world/shibuya/`（`cells/*.rjcell`, `client.txt`, `sources.json`, `pois.json`, `residents.csv`, `slice.json`）。
生データは `data/raw/`（git 管理外）。写真テクスチャ無しで速く試す場合は `--no-textures`。

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
# -> dist/RealJapan-0.1.0-win64.zip（RealJapan.exe, data/, README_ja/en.txt, LICENSES/）
```

## 6. 自動スクリーンショット（起動確認用）

```bash
xvfb-run -s "-screen 0 1600x900x24" ./build-linux/RealJapan \
  --state game --time 2026-09-26T15:30 --screenshot shot.png --frames 30
# Windows EXE を Wine で:
xvfb-run -s "-screen 0 1600x900x24" wine build-win/RealJapan.exe --state title --screenshot shot.png
```
オプション：`--state title|game|pause|phone:map|phone:town|phone:clock|phone:wallet|settings|credits`、
`--time YYYY-MM-DDTHH:MM`（日本時間）、`--pos 緯度,経度`、`--yaw 度`、`--pitch 度`、`--fly --alt m`、`--third-person`、`--lang ja|en`。
