# PROJECT: REAL JAPAN — アーキテクチャと設計決定

仕様書 §40 末尾で求められた 20 項目の決定事項をまとめる。**実装済みのものと未実装のものを区別して書く**。
実装状況の一覧は [STATUS.md](STATUS.md)、ビルド方法は [BUILD.md](BUILD.md) を参照。

凡例: ✅ 実装済み（テスト／実機で動作確認済み） / 🟡 一部実装 / ⬜ 未実装（NOT IMPLEMENTED・設計のみ）

---

## 0. 「不可能な要求」の分析（仕様書 §40 の手順に沿って）

| 要求 | 技術的制約 | データ・権利上の制約 | 品質を落とさない代替方式 | アーキテクチャへの反映 |
|---|---|---|---|---|
| 全建物に入れる | 全国の建物数は数千万棟。手作業モデリングは不可能 | 内部図面の大半は非公開。捏造は仕様で禁止 | 公開資料がある建物（駅・商業施設・PLATEAU 地下街 LOD4 等）は資料から再構築し VERIFIED/PARTIAL。資料がない建物は「入場不可」か「架空であることを明示した生成内部」（FICTIONAL_DISCLOSED） | `rj::verify` の InteriorStatus と Presentation 規則（✅）、`InteriorStreamer`（✅ コア）|
| フォトリアル | OpenGL 3.3 フォワードでは不可。RT/GI/仮想化ジオメトリが必要 | 実写テクスチャは PLATEAU の航空写真由来テクスチャのみ利用可（Google 等は抽出禁止） | 描画をバックエンド差し替え可能に分離し、Phase 14 で D3D12(+DXR) バックエンドを追加 | §8 参照。現状は ⬜ |
| 47都道府県を同品質で | 全国 37.8 万 km²。1 km²≈18 MB（本ビルド実測）× 全域は非現実的 | PLATEAU 整備済み都市は一部（約 300 都市）。それ以外は OSM・基盤地図情報・国土数値情報 | 品質を段階化：PLATEAU 都市は LOD2＋写真、未整備地域は基盤地図情報の建物外形＋推定高さ（UNVERIFIED と明示）、山間部は地形のみ | 階層ストリーミング（✅）＋ Verification ラベル（✅）|
| 実在ブランド・楽曲・人物 | — | 許諾が必要 | 許諾が取れるまで汎用名称。PLATEAU の名称属性は公的データとしてそのまま表示 | 出典管理（✅ sources.json / 建物情報パネル）|
| 全国の NPC をフル AI | CPU 予算的に不可能 | — | Simulation LOD＋「スケジュールから状態を導出」方式 | `rj::sim::SimLodScheduler`（✅）|
| 実在の時刻表・運行 | — | 事業者の時刻表は権利処理が必要（GTFS-JP 公開事業者は利用可） | GTFS-JP 公開路線から着手、それ以外は運行パターンのみ推定（明示） | §12 ⬜ |

---

## 1. 技術スタック

| 層 | 採用技術 | 状態 |
|---|---|---|
| ゲームクライアント | C++20、raylib 5.5（プラットフォーム層：ウィンドウ・入力・OpenGL 3.3・画像デコード）、自前レンダラ／UI | ✅ |
| シミュレーションコア `rjcore` | C++20、外部依存なし。クライアントとヘッドレスのシミュレーションサーバの両方でリンク | ✅ |
| Windows ビルド | MinGW-w64（GCC 13, POSIX threads）で x64 クロスコンパイル、C/C++ ランタイム静的リンク | ✅ |
| データパイプライン | Python 3.11 + numpy / Pillow / mapbox-earcut | ✅ |
| 実行時ワールドデータ | RJCELL（JIS 3次メッシュ単位のバイナリパッケージ） | ✅ |
| 制作用 World Database | PostgreSQL 16 + PostGIS 3（全国マスタ・版管理） | ⬜ 設計のみ（[sql/worlddb_schema.sql](sql/worlddb_schema.sql)） |
| 会話生成 | Claude API をサーバ側で呼び出し、World DB の事実で根拠付け（tool use） | ⬜ |
| ヘッドレス遠距離シミュレーション | `rjcore` を使う常駐プロセス（LOD2/LOD3） | 🟡 `rj_slice_sim` ツールのみ |
| CI | GitHub Actions（Linux ビルド＋テスト＋Windows クロスビルド） | ⬜ |

## 2. ゲームエンジン選定

**決定：自社エンジン（`rjcore` ＋ `client/`）。プラットフォーム層に raylib を使い、描画・ストリーミング・シミュレーションは自前。**

| 評価軸 | UE5 | Unity | Godot 4 | 自社（採用） |
|---|---|---|---|---|
| フォトリアル到達性 | ◎（Nanite/Lumen） | ○（HDRP） | △ | △→○（Phase 14 で新バックエンドが必要） |
| 国土スケールのストリーミング設計の自由度 | ○（World Partition） | ○ | ○ | ◎（JIS メッシュ・測地系を中核に設計） |
| 実データ・出典管理との一体化 | △（プラグイン開発） | △ | △ | ◎ |
| ライセンス | ロイヤリティ 5%（売上閾値超） | 有償プラン | MIT | 自社＋zlib |
| **本プロジェクトの開発環境で Windows EXE をビルド・検証できるか** | ✕（エディタ／SDK が Linux コンテナに導入不可、GPU なし） | ✕ | △（公式バイナリ取得不可） | ◎（MinGW で EXE 生成、Wine で起動検証済み） |

判断理由：
1. ユーザー要件「実際に起動可能な Windows 版 EXE までビルドする」を、この開発環境で満たせるのは自社エンジンだけだった。
2. このゲームの核心は「実在データを正しく扱うこと」（測地系・JIS メッシュ・出典管理・検証状態）で、これはどのエンジンにも無い。コアをエンジン非依存にしたため、**将来 UE5 クライアントへ差し替える選択肢も残る**（その場合は Windows のビルドマシンが必要）。
3. 弱点（正直に）：現在の描画は OpenGL 3.3 のフォワードレンダラで、フォトリアルには程遠い。§8 のとおり Phase 14 で D3D12 バックエンド（RT/GI/仮想化ジオメトリ）を追加する計画で、これは大きな作業になる。

## 3. Repository 構造

```
REAL-JAPAN/
├─ core/                    rjcore（エンジン非依存シミュレーションコア, C++20）
│  ├─ include/rj/geo/       測地系（GRS80, 平面直角座標系 I〜XIX, JIS メッシュ, ENU, 浮動原点, ジオイド）
│  ├─ include/rj/world/     47都道府県・地域区分・階層アドレス
│  ├─ include/rj/stream/    階層ストリーミング（セル／建物内部）
│  ├─ include/rj/sim/       暦・祝日, Simulation LOD, NPC, 1日の予定, 記憶と人間関係
│  ├─ include/rj/interact/  Universal Interaction System（UIS）
│  ├─ include/rj/econ/      複式簿記の経済台帳
│  ├─ include/rj/verify/    出典・ライセンス・検証状態
│  ├─ include/rj/env/       天文計算（太陽位置）
│  ├─ include/rj/nav/       歩行者経路探索（グリッド A*）
│  ├─ tests/                48 テストケース（PROJ で生成した基準値を含む）
│  └─ tools/rj_slice_sim.cpp  ヘッドレス住民シミュレーション
├─ client/                  ネイティブゲームクライアント（Windows x64 / Linux）
│  ├─ src/world/            RJCELL 読み込み, ストリーミング, 衝突, ピッキング
│  ├─ src/render/           レンダラ（空, 太陽, 影, 霧, 写真アトラス）
│  ├─ src/game/             プレイヤー, 住民シミュレーション連携
│  ├─ src/ui/ src/i18n/     UI, 日本語／英語
│  ├─ src/save/ src/config/ セーブ, 設定
│  ├─ res/                  アイコン, バージョン情報, マニフェスト
│  └─ cmake/                MinGW-w64 ツールチェーン
├─ pipeline/                実データ → ゲームデータ（Python）
│  ├─ fetch_plateau.py      PLATEAU カタログ API からメッシュ単位で取得
│  ├─ cook_slice.py         CityGML＋DEM → RJCELL, 地面テクスチャ, 住民配置
│  ├─ realjapan_pipeline/   CityGML パーサ, 三角形分割, DEM, アトラス, RJCELL 形式
│  └─ slices/shibuya.json   Vertical Slice 定義
├─ game/data/               ゲームデータ（言語, 既定設定, アイコン。街データ／フォントは生成物）
├─ packaging/windows/       配布物の README（日英）, データ出典
├─ tools/                   依存取得, アイコン生成, Windows パッケージ作成
└─ docs/                    本書, STATUS, BUILD, SQL スキーマ
```

## 4. World Database 設計

2 層構成。

**(a) 制作用マスタ（PostgreSQL + PostGIS, ⬜ 設計のみ）** — 全国の正規データ。全実体は共通の出典列を持つ：
`source_ids[]`, `license_ids[]`, `last_verified`, `accuracy_m`, `confidence`, `geometry_source`, `interior_source`, `verification`。
主なテーブル：`prefecture`, `municipality`, `district`, `mesh_cell`, `road_link`, `road_node`, `lane`, `building`, `building_part`, `interior_space`, `business`, `station`, `railway_line`, `track_segment`, `airport`, `runway`, `port`, `poi`, `npc`, `vehicle`, `weather_region`, `economy_account`, `event`, `source`, `license`。
DDL の叩き台：[sql/worlddb_schema.sql](sql/worlddb_schema.sql)。

**(b) 実行時パッケージ RJCELL（✅）** — JIS 3次メッシュ（約 1 km）単位にクックしたバイナリ。形式は `pipeline/realjapan_pipeline/rjcell.py` に定義し、C++ 側 `client/src/world/cell.cpp` が同じ形式を読む。
内容：セル原点（測地座標）、建物テーブル（ID・名称・用途・高さ・階数・LOD・検証状態・出典番号・外形）、メッシュチャンク（≤65535 頂点、位置/法線/色/UV）、地形高さグリッド、地面テクスチャ（PNG）、写真アトラス（JPEG）。
ゲームは SQL を実行時に引かない。マスタ → クック → パッケージ、の一方向。

## 5. GIS Pipeline

| 項目 | 方針 | 状態 |
|---|---|---|
| 測地系 | JGD2011。入力：EPSG:6697（PLATEAU, 緯度経度＋T.P.標高）, 6668, 平面直角座標系 6669–6687 | ✅ 変換を実装（PROJ と 1 mm 以内で一致をテスト） |
| 分割単位 | JIS X 0410 地域メッシュ（1次〜1/8）。PLATEAU のファイル分割と一致 | ✅ |
| 座標系（実行時） | セルごとのローカル ENU＋浮動原点。単一の投影法を使わない（日本は東西に長く TM 1 帯では歪む） | ✅ |
| 標高 | 国土地理院 標高タイル DEM5A→5B→10B のフォールバック | ✅（※権利確認中、STATUS 参照） |
| ジオイド | GSIGEO2011 グリッド読み込み I/F | 🟡（I/F のみ。データ未同梱のためセル内相対配置のみ正確） |
| 行政界 | 国土数値情報 N03 | ⬜ |
| 道路トポロジー・交通規制 | PLATEAU tran LOD3（車線）＋ OSM（接続・規制、ODbL） | ⬜（OSM API 疎通は確認済み） |
| 未整備地域の建物 | 基盤地図情報 建築物の外周線＋推定高さ（UNVERIFIED 表示） | ⬜ |

## 6. PLATEAU Pipeline（✅ Phase 1 範囲）

1. `fetch_plateau.py`：データカタログ API `.../datacatalog/citygml/m:{mesh}` でメッシュ単位のファイル URL を取得（650 MB の ZIP 全体は不要）。
2. `citygml.py`：iterparse によるストリーミング解析。建物（LOD0 外形, LOD1 立体, LOD2 屋根・壁・付属物, 属性：用途・計測高さ・階数・名称・建物 ID）、外観（リング ID → 画像＋テクスチャ座標）、道路（LOD1 道路域, LOD2 車道・歩道・島）。
3. `triangulate.py`：多角形の最適平面へ投影して earcut（穴あき対応）。
4. `atlas.py`：航空写真由来の屋根・壁テクスチャを取得し、セル単位で 4096² アトラスにパック（一様縮小）。UV を再マップ。
5. `cook_slice.py`：セル ENU への変換、用途別の色、簡易遮蔽グラデーション、地盤下スカート、地面テクスチャ（実在道路域のラスタ化）、地形グリッド、RJCELL 書き出し、出典 `sources.json`。

6. 都市設備 frn LOD3：道路標示（横断歩道・区画線・停止線）は地面テクスチャへ、信号機・照明・柱・柵・標識は 3D メッシュ化して地形に接地（✅）。

未対応（⬜）：交通 LOD3（車線）、地下街 ubld LOD4（**公的な内部データ → VERIFIED_INTERIOR 候補**）、橋梁 brid、植生 veg、土地利用・都市計画（用途地域）、都市設備の写真テクスチャ。

## 7. World Streaming（✅ コア / 🟡 クライアント）

`rj::stream::HierarchicalStreamer`：
- 論理階層 Japan → Region → Prefecture → Municipality → District は常駐メタデータ。物理階層は JIS メッシュのレイヤ（1次 80 km 遠景地形 → 2次 10 km → 3次 1 km 都市 → 1/2 500 m 詳細 → 1/8 125 m 小物）。
- 規則：親セルが常駐していなければ子を読まない（穴を作らない）／ロード半径＜アンロード半径（ヒステリシス）／速度方向の先読み（電車・飛行機）／メモリ予算と LRU 的退避（子が常駐する親は退避しない）。
- `InteriorStreamer`：外装セルが常駐し、入口から一定距離内の建物だけ内部を読む。
- クライアントの現状：3次メッシュ 1 レイヤのみ（収録データが 4 セルのため）。読み込みはワーカースレッドで解析、GPU 転送は 1 フレーム 1 セルまで。

## 8. Rendering Architecture

**現状（✅）**：OpenGL 3.3 フォワード。天文計算の太陽方向・色、空のグラデーション＋太陽円盤、距離霧、指向性シャドウマップ（4096², PCF）、線形空間ライティング＋ACES トーンマップ、PLATEAU 写真アトラス、実在道路域の地面テクスチャ。

**フォトリアル目標（⬜ Phase 14）**：描画 API を D3D12（Windows 主対象）へ移行し、
GPU 駆動描画（メッシュレット／クラスタ LOD による Nanite 相当の仮想化ジオメトリ）、仮想テクスチャ、
GI（プローブ GI → ReSTIR GI）、RT 反射・RT 影・コンタクトシャドウ、パストレースモード、
物理ベースの大気散乱と体積雲、FFT 海洋・水面、濡れ路面・積雪蓄積・泥汚れのマテリアルレイヤ、
植生と風、SSS・髪・布。**「実写に見せかけた偽スクリーンショット」は作らない**（仕様 §36）。

## 9. Interior Streaming（✅ コア / ⬜ コンテンツ）

- 検証状態：`VERIFIED` / `PARTIAL` / `UNKNOWN` / `FICTIONAL_DISCLOSED`。建物ラベル：`VERIFIED_INTERIOR` / `PARTIAL_INTERIOR` / `VERIFIED_EXTERIOR` / `UNVERIFIED`。
- 規則（`rj::verify::presentation`）：内部資料の出典が無ければ VERIFIED/PARTIAL にならない。架空の内部は UI に「架空」と明示しなければ入場させない。
- 本ビルドの全建物は `VERIFIED_EXTERIOR`（外観＝PLATEAU）／内部 `UNKNOWN` → **入場不可**。
- 最初の実在内部候補：PLATEAU 地下街モデル LOD4（渋谷区 53393596 に 2 地物）。

## 10. NPC Simulation（🟡）

- ✅ 住民：名前・年齢・性格（Big Five）・自宅・職場／学校・収入・趣味・好きな音楽／食べ物・睡眠傾向・健康・感情・家族／友人。51 職業（仕様書の職業＋学生＋退職者）。
- ✅ 1 日の予定：平日／休日／祝日、シフト（日勤・早番・遅番・夜勤・フレックス・学校）、7 日勤務者の休日、病気（通院）、雨で屋外趣味を中止、友人の誘い。日付と NPC の種から決定的に生成（遠距離 LOD で状態を再計算できる）。
- ✅ 記憶と関係：Unknown / Acquaintance / Friend / CloseFriend / Partner / Family / Enemy。親密度は会うほど上がり、会わないと半減期で下がる（「常連を覚える店員」テスト済み）。
- ✅ 本ビルド：実在の住宅に住む架空の住民 3,000 人の予定をシミュレートし、スマホ「街の様子」に表示。
- ✅ 徒歩移動中の住民・域外通勤者の 3D 表示：`rj::nav::GridNav`（実在の建物外形をラスタ化した 2 m グリッド、道路網の最大連結成分へのスナップ、A*＋視線平滑化）。出現位置は予定から導出、以後は現実の歩行速度。
- ⬜ 歩道ネットワーク（歩道中心線・横断歩道・階段・駅コンコース）、群衆の GPU インスタンシング／スキニング、回避、LOD0 の効用 AI、会話。
- ⬜ 会話生成：LLM には「その NPC が知り得る World DB の事実（場所・時刻・天気・関係・記憶）」だけを渡し、事実にない固有名詞は出させない。応答は事実照合を通してから表示し、失敗時はテンプレート応答。

## 11. Traffic Simulation（⬜）

道路グラフ：PLATEAU tran LOD3（車線）＋ OSM（接続・一方通行・右左折規制）。
LOD0/1：IDM 追従＋MOBIL 車線変更、信号現示（公開データが無い交差点は推定値であることを明示）。LOD2：メゾスコピック（リンク待ち行列）。LOD3：OD 統計フロー。
車両物理：Pacejka タイヤモデル、サスペンション、駆動方式（FF/FR/AWD/4WD）、ABS/TCS/ESC、路面摩擦（乾燥・雨・雪・凍結）。

## 12. Railway Simulation（⬜）

線路形状：国土数値情報（鉄道）＋ OSM。運行：GTFS-JP 公開事業者から着手（公開されていない時刻表は権利処理が必要）。
列車は線路スプライン上の編成として走り、駅は「改札（UIS の Use/Enter）→ ホーム → 乗車 → 車内移動 → 降車」を実体で行う（仕様 §4、メニュー式のファストトラベルは作らない）。

## 13. Aviation Simulation（⬜）

空港・滑走路：国土数値情報（空港）・公開 AIP。旅客動線（ターミナル → 搭乗手続き → 保安検査 → 搭乗口 → 搭乗）は UIS で実装。
飛行：6 自由度の飛行モデル（観光客モードは自動操縦、操縦モードは任意）。遠距離の便は LOD3 の統計イベント。

## 14. Economy Simulation（🟡）

- ✅ 複式簿記の台帳：お金は口座間を移動するだけで、外部口座（輸出入）以外から生まれない（総和 0 をテスト）。定期支払い（家賃・光熱費・通信）、滞納記録、源泉徴収（**ゲームルール。税法の再現ではない**）。
- ⬜ 企業・雇用・価格・家賃（地価公示などで較正）・保険・税制、店舗の在庫と売上。

## 15. Universal Interaction System（✅）

物体は Interactable Component の組み合わせ。動詞：Open, Close, Sit, StandUp, LieDown, PickUp, Carry, Drop, Push, Pull, Use, Eat, Drink, Buy, Sell, Repair, Break, Clean, Cook, Drive, Ride, Read, Watch, Play, Work, Talk, Enter。
部品：Openable, Seat, Bed, Pickable, Merchandise, Checkout, Consumable, Cookware, Vehicle, Media, Workstation, Talkable, Tool, Condition, Movable, Portal。
プレイヤーも NPC AI も同じ `options()` / `interact()` を使う。利用不可の理由はローカライズキーで返す。
コンビニの「入店 → 棚から取る → レジで払う → 食べる」、未払いで退店すると万引きイベント、をテスト済み。
**3D クライアントへの接続は未実装**（実在店舗の内部資料が無いため、店内は作っていない）。

## 16. Verification Database（✅）

`rj::verify::Provenance`：Source（ID・名称・URL・ライセンス・帰属表示・取得日・用途＝素材／参照専用）、`last_verified`、`accuracy_m`、`confidence`、`geometry_source`、`interior_source`、検証状態。
`validate()` が拒否するもの：未確認ライセンス、帰属表示の欠落、参照専用ソース（Google 等）由来のジオメトリ／内部、出典のない VERIFIED。
クライアントの建物情報パネルは検証ラベル・内部状態・出典を常に表示する。

## 17. 東京 Vertical Slice 対象地域

**渋谷駅周辺：JIS 3次メッシュ 53393585 / 53393586 / 53393595 / 53393596（約 2.1 km × 1.85 km）**、品質集中域はその中の 1 km × 1 km。
選定理由：PLATEAU 2025 の渋谷区は建物 LOD2＋写真テクスチャ、交通 LOD3（車線）、地下街 LOD4（内部）、都市設備 LOD3、橋梁 LOD2 がそろう／鉄道 9 路線級の結節点／商業・業務・住宅が混在し「自宅」「仕事」「店」を 1 km 内で実証できる／谷地形（渋谷＝谷）で地形の効果が大きい。

## 18. 実装ロードマップ

| Phase | 内容 | 状態 |
|---|---|---|
| 0 | 基盤技術（測地系・メッシュ・ストリーミング・Sim LOD・UIS・経済・検証・暦・天文） | ✅ |
| 1 | 東京超高密度 Vertical Slice | 🟡 外装・道路・地形・昼夜・セーブ・日英は済。次：frn/tran LOD3、NPC 表示、店、仕事、電車、車 |
| 2 | 建物内部（地下街 LOD4 → 公開フロアマップのある駅・商業施設） | ⬜ |
| 3 | NPC 生活シミュレーション（3D 群衆・経路・会話） | 🟡 コアのみ |
| 4 | 車・交通 | ⬜ |
| 5 | 鉄道 | ⬜ |
| 6 | 職業・生活（UIS をクライアントへ） | 🟡 コアのみ |
| 7 | 都市拡張（渋谷区全域 → 23 区） | ⬜ |
| 8 | 関東地方 | ⬜ |
| 9 | 主要都市（PLATEAU 整備都市） | ⬜ |
| 10 | 47 都道府県 | ⬜ |
| 11 | 地方・山・海・島 | ⬜ |
| 12 | 全国交通（新幹線・高速道路・航空・フェリー） | ⬜ |
| 13 | 大規模経済・社会 AI | ⬜ |
| 14 | フォトリアル最終品質（D3D12/RT バックエンド） | ⬜ |
| 15 | 最適化 | ⬜ |

## 19. 性能予算

**実測（本ビルド）**：4 セルで建物 9,535 棟・約 90 万三角形。RJCELL は 1 セル 13〜15 MB（幾何）＋写真アトラス数 MB。

**目標（Phase 1, 1080p/60fps, GTX 1660 / RTX 3050 クラス）**

| 項目 | 予算 |
|---|---|
| CPU フレーム | 16.6 ms：Sim LOD0 2 ms / LOD1 1 ms / ストリーミング 1 ms / 物理 2 ms / 描画発行 3 ms / UI 1 ms / 余裕 6.6 ms |
| GPU フレーム | 影 2.5 ms / 不透明 6 ms / 空・霧・ポスト 1.5 ms / UI 0.5 ms / 余裕 6 ms |
| 常駐メモリ | ワールド 3 GB（ストリーマ予算）、VRAM 4 GB（テクスチャ 2.5 GB） |
| ディスク | PLATEAU LOD2 都市部で約 18 MB/km²（本ビルド実測）。LOD2 都市部 1 万 km² で約 180 GB → 圧縮（メッシュ量子化＋BC7）で 1/3〜1/4 を目標 |
| 遠距離シミュレーション | 全国の LOD3 更新はゲーム内 1 時間ごと、1 回 5 ms 以内に分割 |

## 20. 最初に実装したコード（本リポジトリの現状）

1. `core/`：測地系・JIS メッシュ・47 都道府県・階層ストリーミング・Sim LOD・暦と祝日・NPC と 1 日の予定・記憶と関係・UIS・経済台帳・検証・天文計算（48 テスト、Linux と Windows(Wine) の両方で合格）。
2. `pipeline/`：PLATEAU（建物 LOD1/LOD2＋写真、道路 LOD1/LOD2）と国土地理院 DEM から渋谷 4 セルをクック。
3. `client/`：Windows x64 ネイティブ EXE。歩行・飛行、建物情報と検証ラベル、スマホ、セーブ／ロード、設定、日英。
4. `tools/package_windows.sh`：Release ビルドと配布 ZIP。
