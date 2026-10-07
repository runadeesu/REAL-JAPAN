日本の風景を参考にした**架空の国「秋津国」**と、国土交通省 PLATEAU・国土地理院のデータで作った**渋谷**を、歩いて・乗り物で巡りながら暮らすオープンワールドゲームです。

## ダウンロード

| ファイル | 内容 |
|---|---|
| `RealJapan-{VER}-win64-setup.exe` | **Windows 10/11（64 ビット）のインストーラ**。これ 1 つで全部入り、管理者権限は不要 |
| `RealJapan-{VER}-win64.zip` | 同じ内容の ZIP。展開して `RealJapan.exe` を実行 |
| `RealJapan-{VER}-android-arm64-full.apk` | **Android 7.0 以上（arm64・OpenGL ES 3.0）**。これ 1 つで全部入り（約 580 MB）。試作 |
| `RealJapan-{VER}-all.zip` | Windows のインストーラと Android の APK をまとめたもの |

Windows はコード署名をしていないため、「Windows によって PC が保護されました」と出たら「詳細情報」→「実行」を選んでください。

## 遊べること

- **秋津国**：首都と地方の町・農村・温泉町・雪国。新幹線と在来線（駅の改札から歩いて乗る、座る、環状線は運転もできる）、フェリー、旅客機と小型機（操縦）、高速道路（料金所・パーキングエリア）、車の運転（燃料・損傷・ガソリンスタンド）
- **暮らし**：仕事と趣味、店での買い物、借りる部屋、スマホのアプリ 19 種、空腹とのどの渇き、ギターとバンド・路上ライブ、学校
- **街と人**：住民の一日、通行人との会話（定型文）、天気と季節（桜・梅雨・台風・雪）、海（波・潮・泳ぐ）
- **自分の 3D キャラクター**（新）：リグ付きの FBX（Mixamo 形式のボーン）を `characters` フォルダに入れて起動すると、ゲームが変換して三人称（V キー）の自分の姿や近くの人に使います。フォルダは設定の「キャラクターを追加」で開けます。キャラクターのデータはこの配布物には入っていません
- **ゲームコントローラー**（Windows・Android）、**日本語／英語**

## 注意

- Windows 版は Wine 上で起動と自動テスト（40 項目）を確認しています。**実機の Windows での確認はしていません**。
- **Android 版は試作で、実機・エミュレータでの動作は未確認です**。
- 秋津国は架空の国で、実在の場所・店・人物とは関係ありません。料金・家賃・運賃などはすべてゲーム上の値です。会話・SNS・放送は定型文で、AI や音声ではありません。
- 渋谷ワールドは PLATEAU（国土交通省）と国土地理院のデータを加工して作成しています。建物の写真テクスチャには実在の看板が写り込んでいます（設定でオフにできます）。権利処理の状況はリポジトリの docs/STATUS.md を参照してください。
- 実装済み・未実装の全項目は [docs/STATUS.md](https://github.com/runadeesu/REAL-JAPAN/blob/v{VER}/docs/STATUS.md) にあります。

---

*English*: an open-world game set in the fictional country of Akitsu (modelled on Japanese landscapes) and in Shibuya built from PLATEAU / GSI open data. Windows 10/11 x64 installer or ZIP, Android 7.0+ arm64 APK (experimental, not yet tested on a device). Not code-signed. Your own rigged characters (FBX) can be added through the `characters` folder; none are included.
