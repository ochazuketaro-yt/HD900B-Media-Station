# 実演(撮影)の手順例

準備: HDD(ext4)、SD カード(コピー元のデータ入り)、USB メモリを接続。PC とプレーヤーを同じ LAN に。`sh /tmp/ms/ms_boot.sh`。

1. テレビ: Premium Apps → `HD900B_MEDIA` → `STORAGE` で 4 系統の認識結果を見せる。
2. PC のブラウザで `http://<ip>:8820/` を開き、同じ一覧(容量バー・空き)を見せる。
3. 「Import all → HDD」(SD カード)、またはテレビの `IMPORT: SD CARD to HDD`。
4. 進捗: ブラウザの転送カード(コピー/検証の二層バー、速度、残り時間、ログ)とテレビの `PROGRESS`。
5. 完了後: 検証済み件数・重複スキップ件数を見せる。`Imports/<日時> SD CARD/` と `MANIFEST-*.txt` を 2 ペインで開く。
6. PC からフォルダをドラッグ&ドロップでアップロード、HDD のファイルをダウンロード。
7. 追加: 「Auto-import on insert」を ON にして SD カードを挿し直す、テレビの `RECENT MEDIA` で HDD の動画を純正プレーヤーで再生。
注意: SD の読出しは約 9 MB/s なので、大きなデータは編集で短縮するか小さめのデータを用意すると良い。
