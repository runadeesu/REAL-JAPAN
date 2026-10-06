PROJECT: REAL JAPAN  —  開発ビルド v{VER}（Windows 版と Android 版をひとつにまとめたもの）

中身
  Windows/RealJapan-{VER}-win64-setup.exe   Windows 10/11 (64 ビット) 用のインストーラ（これ 1 つで全部入り）
  Android/RealJapan-{VER}-android-arm64.apk  Android 7.0 以上（arm64、OpenGL ES 3.0）用の APK（これ 1 つで全部入り）

Windows
  1. setup.exe を実行します（管理者権限は不要。%LOCALAPPDATA%\Programs\RealJapan に入ります）。
     未署名のため「Windows によって PC が保護されました」と出たら「詳細情報」→「実行」。
  2. スタートメニューかデスクトップの「PROJECT REAL JAPAN」から起動します。
  アンインストールは「設定 → アプリ」から。セーブと設定（%APPDATA%\RealJapan）は残ります。
  ZIP 版が良い場合は GitHub のリリースにある RealJapan-{VER}-win64.zip を展開して RealJapan.exe を実行。

Android（試作・実機での動作は未確認）
  APK をスマホにコピーして開き、「提供元不明のアプリ」のインストールを許可してインストールします。
  または PC から: adb install Android/RealJapan-{VER}-android-arm64.apk
  約 560 MB あります。空き容量に注意してください。

注意
  自分の 3D キャラクター（FBX）は、インストール先の characters フォルダに入れると使えます（設定から開けます）。
  秋津国は日本の風景を参考にした架空の国です。実在の場所・店・人物とは関係ありません。
  価格・運賃・家賃などはすべてゲーム上の値です。会話・SNS・放送は定型文で、AI や音声ではありません。
  渋谷ワールドは PLATEAU（国土交通省）・国土地理院のデータを使っています（LICENSES を参照）。
