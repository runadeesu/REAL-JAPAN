# PROJECT: REAL JAPAN

「現実世界そのものをゲームにする」——実在の日本を、公的な地理・建物データから再構築して、その中で生活できるオープンワールドゲーム。

**現在：v0.2.0 渋谷 Vertical Slice（開発ビルド）/ Windows 10・11 x64 ネイティブ EXE**

| | |
|---|---|
| ![駅前（ハチ公前広場 → スクランブル交差点）晴れ・目線 1.7 m](docs/screenshots/eye_station_day.jpg) | ![明治通りの歩道・雨の夕方（傘・濡れた路面）](docs/screenshots/eye_avenue_rain.jpg) |
| ![宇田川町の商店街・夜](docs/screenshots/eye_shops_night.jpg) | ![道玄坂の路地・夜（電柱・防犯灯は推定配置）](docs/screenshots/eye_alley_night.jpg) |

改善前後の比較（目線 1.7 m、同じ場所・時刻）：[昼](docs/screenshots/compare_day.jpg) ／ [夜](docs/screenshots/compare_night.jpg)

※ スクリーンショットは同じソースからビルドした Linux 版、または Windows 版 EXE（Wine）を、ソフトウェア OpenGL（Mesa llvmpipe）で実行して撮影したもの（GPU 実機ではない）。

- 国土交通省 PLATEAU（渋谷区 2025 年度）の実在建物 **9,535 棟**（LOD2 3,064 棟、航空写真由来の屋根・壁テクスチャ）
- PLATEAU の実在道路域から作った段差のある歩道・縁石・中央帯、実在の街路樹、都市設備（調査済みの道路）
- PLATEAU LOD4 の渋谷駅周辺地下街（検証済み内部）へ階段から歩いて入れる
- 国土地理院の標高による地形、JGD2011・JIS メッシュの階層ストリーミングと浮動原点
- 物理ベース描画（影・SSAO・SSR・ブルーム・自動露出）、実際の太陽位置、10 種の天気（濡れた路面・水たまり・傘）、夜の街明かり
- 実在の車道網を走る車（左側通行・信号で停止）、住民（シミュレーション）＋来街者（統計的に生成）の歩行者が歩道と横断歩道を歩き赤信号で待つ
- **推定（実データではない）と明記したもの**：PLATEAU に無い道路の横断歩道・車線・信号・街路灯・電柱と電線、建物近景（店構え・窓・看板帯。看板文字は実在の店名ではない汎用模様）、夜の点灯窓
- スマートフォン、セーブ／ロード、設定ファイル、日本語／英語、開発者表示（F3）

**まだ無いもの**：鉄道・飛行機・船、プレイヤーの運転、店・仕事・会話、音、地下街以外の建物内部（公開資料が無いので捏造せず入場不可）、渋谷以外、GI/レイトレーシング。
すべての項目の正直な状況は [docs/STATUS.md](docs/STATUS.md)。

## ドキュメント

| 文書 | 内容 |
|---|---|
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 技術スタック・エンジン選定・World DB・パイプライン・ストリーミング・描画・各シミュレーション・性能予算・ロードマップ（仕様書が求める 20 項目） |
| [docs/STATUS.md](docs/STATUS.md) | 実装済み／一部／未実装の一覧、権利処理の未完了事項、既知の問題 |
| [docs/BUILD.md](docs/BUILD.md) | データ取得・クック・ビルド・Windows 配布 ZIP の作成手順 |
| [docs/sql/worlddb_schema.sql](docs/sql/worlddb_schema.sql) | 制作用 World Database（PostGIS）スキーマ案（未実行） |

## 構成

```
core/       rjcore：エンジン非依存のシミュレーションコア（C++20, テスト 49 件）
client/     ネイティブゲームクライアント（C++20 + raylib, Windows x64 / Linux）
pipeline/   実データ → ゲームデータ（PLATEAU CityGML, 国土地理院 DEM）
game/data/  言語ファイル・既定設定（街データとフォントはスクリプトで生成／取得）
packaging/  Windows 配布物の README とデータ出典
tools/      依存取得・アイコン生成・配布 ZIP 作成・品質確認スクリーンショット（shots.sh）
```

## データ出典

- 「3D都市モデル（Project PLATEAU）渋谷区（2025年度）」（国土交通省）（https://www.geospatial.jp/ckan/dataset/plateau-13113-shibuya-ku-2025）を加工して作成
- 出典：国土地理院 標高タイル（https://maps.gsi.go.jp/development/ichiran.html）を加工して作成 ※焼き込み配布に関する測量法上の手続き要否は確認中

Google Maps / Earth 等の著作物は抽出していません。PLATEAU の写真テクスチャ（航空写真由来）には実在の看板・広告が写り込んでおり、第三者の商標・著作物の扱いは確認中です（ゲーム内の設定で写真テクスチャをオフにできます）。
