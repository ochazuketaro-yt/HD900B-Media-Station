# トラブルシューティング

| 症状 | 確認 / 対処 |
|---|---|
| ブラウザから開けない | プレーヤーで `pidof hdms`、`cat /tmp/ms/hdms.log`、`cat /tmp/ms/stdout.log`。PC が同じ LAN か(プライベートアドレス以外は 403)。PIN を付けた場合は Basic 認証 |
| ストレージが出ない / マウントされない | `cat /proc/partitions`、`dmesg`。FS が無い媒体は「unformatted」と表示。ログに `mount … failed` が出ていれば原因あり(30 秒ごとに再試行) |
| テレビにタイルが出ない | `grep menu.rss /proc/mounts`(bind が 1 つあるか)。`sh /tmp/ms/ms_boot.sh` を再実行。Premium Apps を一度閉じて開き直す |
| テレビの進捗が古い | 仕様。左メニューの `PROGRESS` を選び直す |
| 「No such storage」「No writable SATA HDD」 | 対象媒体が未接続/未マウント、または HDD が無い・読み取り専用 |
| コピーが `VERIFY MISMATCH` | 媒体や配線の不良の可能性。一時ファイルは削除済み。別の媒体で再試験 |
| 電源再投入後に動かない | `/tmp` が消えるため。[電源を入れ直したとき](../README.md#電源を入れ直したとき) |
| 完全に元に戻したい | `sh /tmp/ms/uninstall.sh`(フラッシュは変更していないので他に戻すものは無い) |
| DvdPlayer(テレビ画面)が固まった | `MAIN` プロセスと `DvdPlayer` を `kill -9`(RootApp が再起動、`/tmp` と bind は残る) |
