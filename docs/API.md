# HTTP API(ポート 8820)

読み取りは GET、変更は POST/PUT + ヘッダ `X-MS: 1`(ループバックは不要)。応答は JSON(`{"ok":false,"error":"…"}` で失敗)。
ボリューム ID は `/api/storage` の `id`(`hdd1` `usb1` `sd1` `dvd1`)。パスはボリューム内の `/a/b`。

| メソッド/パス | 説明 |
|---|---|
| `GET /api/info` | バージョン、稼働時間、メモリ、実行中ジョブ数、設定 |
| `GET /api/storage` | ボリューム一覧(種別、名前、型番、FS、ラベル、容量、空き、rw、マウント状態) |
| `GET /api/ls?v=&p=` | ディレクトリ一覧(最大 5000 件) |
| `GET /dl?v=&p=[&inline=1]` | ダウンロード(Range 対応。`inline=1` で表示用、HTML は text/plain 扱い) |
| `PUT /api/put?v=&p=<file>[&mk=1&overwrite=1]` | 生のボディをファイルとして保存(親フォルダ作成、既定は上書きせず ` (1)`) |
| `POST /api/mkdir?v=&p=` / `POST /api/rm?v=&p=&confirm=yes` | フォルダ作成 / 削除(ルート不可) |
| `POST /api/copy` | `src=<vol>:<path>`(複数可) `dst=<vol>` `dstp=<path>` `verify=none\|fast\|sha256` `policy=smart\|skip\|overwrite` `sort=1` `dedupe=0\|1` → `{"id"}` |
| `POST /api/import?src=&dst=` | 媒体全体を `Imports/<日時> <名前>/` へ取り込み |
| `GET /api/jobs[?n=]` / `GET /api/job?id=[&log=1]` / `POST /api/cancel?id=` | ジョブ一覧・状態(進捗・速度・ETA・件数・エラー)・ログ・取消 |
| `POST /api/mount?v=` / `POST /api/umount?v=[&force=1]` | マウント / 安全な取り外し(HDD は force 必須) |
| `POST /api/config?autoimport=&verify=&sort=&dedupe=` | 設定 |
| `GET /tv/storage.rss` `progress.rss` `network.rss` `recent.rss` / `GET /tv/do?c=…` | テレビ用(`do` はループバックのみ) |
