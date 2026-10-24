# hd900b-media-station

**2011 年発売のメディアプレーヤー HiMedia HD900B(Realtek RTD1186)を、USB メモリ・SD カード・SATA HDD・DVD ドライブをまとめて扱える「Media Station」にする、小さなデーモンとインストーラです。**

プレーヤー**自身の中**で動く単一の静的バイナリ `hdms` が、接続されている記録媒体を自動検出し、
**LAN 上の PC のブラウザ**と、**テレビ側の純正 Premium Apps 画面**の両方から、一覧・閲覧・コピー・取り込み・検証・進捗表示を提供します。

> ⚠️ **非公式・自己責任のプロジェクトです。** 接続した記録媒体にデータを書き込み、ファイルを削除できる機能があります。[免責](#免責)を必ずお読みください。

## 目次
- [できること](#できること) / [動作確認環境](#動作確認環境) / [他機種の可能性](#他の-rtd1186-搭載機での可能性)
- [インストール](#インストール) / [使い方](#使い方) / [電源を入れ直したとき](#電源を入れ直したとき)
- [HDD の準備](#hdd-の準備) / [設定](#設定) / [アンインストール](#アンインストール)
- [制限事項](#制限事項) / [ソースからビルド](#ソースからビルドする) / [構成](#リポジトリの構成) / [ライセンス](#ライセンス) / [免責](#免責)

## できること

- **ストレージ一覧**: USB / SD / SATA HDD / DVD を自動検出し、種別・容量・空き・ファイルシステム・ラベル・読み書き可否・マウント状態を表示。未マウントの ext4 / FAT / NTFS・exFAT(ufsd) は自動マウント、データ DVD は読取専用でマウント(ディスクの無い状態の表示のみ確認)。
- **取り込み・コピー**: 任意の媒体間(例: SD → HDD)でフォルダ構造を保ったままコピー。
  - コピー後に媒体から**読み戻して検証**(既定は XXH64、SHA-256 も選択可)
  - 一時ファイルに書いて検証後に原子的に確定(電源断・抜去でも壊れたファイルを残さない)
  - 同名ファイルは上書きせず ` (1)` を付与、同一ファイルはスキップ、**内容が同じファイルの重複排除**
  - 空き容量の事前検査、FAT32 の 4GB 超ファイルの事前検出、取消、種類別(写真/動画/音楽…)自動分類、`sha256sum -c` 互換マニフェスト
  - SD カード**挿入時の自動取り込み**(任意)
- **LAN の Web UI**(`http://<プレーヤーのIP>:8820/`): ストレージカード、2 ペインのブラウザ(コピー先へドラッグ)、PC からのファイル/フォルダのドラッグ&ドロップアップロード、ダウンロード(Range 対応)、フォルダ作成、削除、安全な取り外し、コピー進捗(速度・残り時間・検証状況・ログ)。日本語/English 切替。
- **テレビ側**(Premium Apps の `HD900B_MEDIA` タイル): ストレージ一覧、SD/USB/DVD からの取り込み開始、進捗(`PROGRESS` を選び直すと更新)、取消、HDD 上の最近のメディアを純正プレーヤーで再生、PC 接続先 URL の表示、各種設定の切替。
- ファームウェア(フラッシュ・`/system`・`/data`)は**書き換えません**。`/tmp`(RAM)と、メニューの bind mount 1 つだけを使います。

## 動作確認環境

| 項目 | 内容 |
|---|---|
| 端末 | **HiMedia HD900B**(Realtek RTD1186、MIPS 24Kc、RAM 約 470 MB、Linux 2.6.34-VENUS) |
| 接続機器 | USB フラッシュ 32GB(FAT32)、SD 8GB(FAT32)、SATA HDD 300GB(ext4)、DVD-RW ドライブ(Linux 上は USB 接続の sr0) |
| PC(インストール時のみ) | Python 3.8 以降(標準ライブラリのみ)。Debian Linux で確認、macOS/Windows は未確認 |
| 確認した内容 | `docs/TECHNICAL-NOTES.md` と `CHANGELOG.md` を参照(実機でのコピー・検証・自動取り込み・取消・容量不足・セキュリティ試験、テレビ画面での進捗表示) |

動作確認は **HD900B 1 台のみ**です。**DVD データディスクの取り込みは、ディスクが無かったため未検証**です。

## 他の RTD1186 搭載機での可能性

同じ SoC のプレーヤーはファームウェアが近いため動く可能性はありますが、**未検証**です。`docs/COMPATIBILITY.md` を参照してください。

## インストール

必要なもの: プレーヤーと PC が同じ LAN にあること、プレーヤーに Telnet(`root`、パスワード空)で入れること、PC に Python 3.8+。

```sh
python3 tools/install.py --host <プレーヤーのIP> --yes
# 復元用バンドルをディスクにも残す場合(電源再投入後に PC 無しで復元できる):
python3 tools/install.py --host <プレーヤーのIP> --yes --bundle-dir /tmp/usbmounts/sdb1
```

`dist/hdms` はビルド済みです(`dist/SHA256SUMS`)。自分でビルドする場合は[こちら](#ソースからビルドする)。
インストーラはプレーヤーから**現在表示中の `menu.rss`** と純正の `youtube_menu.rss` を読み出して手元でパッチし(メーカーのファイルは再配布しません)、`/tmp/ms` に配置して `ms_boot.sh` を実行します。
[hd900b-youtube-relay](../hd900b-youtube-relay)(YouTube タイル)を使っている場合は、**先にそちらを起動してから**このインストーラを実行すると、両方のタイルが並びます。

## 使い方

- **PC**: ブラウザで `http://<プレーヤーのIP>:8820/` を開く。LAN 内のプライベートアドレスからのみ接続できます。
- **テレビ**: ホーム → Premium Apps → `HD900B_MEDIA`(タイルのアイコンは純正の VOD の絵を流用)。左メニュー:
  `STORAGE` / `IMPORT: SD CARD|USB|DVD to HDD` / `PROGRESS` / `CANCEL TRANSFER` / `RECENT MEDIA` / `NETWORK ACCESS` / `AUTO-IMPORT` / `VERIFY MODE` / `SORT BY TYPE`。
  進捗は自動更新されないので、`PROGRESS` を選び直して更新します。
- 取り込み先は、書き込み可能な最初の SATA HDD の `Imports/<日付_時刻> <媒体名>/` です。

## 電源を入れ直したとき

`/tmp` と bind mount は消えます。Telnet で、HDD をマウントしてから(`mkdir -p /tmp/usbmounts/sdb1; mount -t ext4 /dev/block/sdb1 /tmp/usbmounts/sdb1`)、
`--bundle-dir` で保存したバンドルを使う: `sh /tmp/usbmounts/sdb1/.mediastation/bundle/install_from_hdd.sh`、または PC からインストーラを再実行。
起動時の自動実行は**行いません**(永続的なシステム変更が必要になるため。`docs/ARCHITECTURE.md`)。

## HDD の準備

新品/未フォーマットの HDD を ext4 にする手順(実機で実施した手順)は `docs/FORMAT-HDD.md`。**ディスクの内容が全て消えます。**

## 設定

`hdms` のオプション: `-p ポート`(既定 8820)、`-b 待受アドレス`、`-P PIN`(指定すると LAN からの接続に Basic 認証。ユーザー名は任意)、`-z 時差`(フォルダ名用、既定 +9)、`-A`(自動取り込み)。
`ms_boot.sh` の引数はそのまま `hdms` に渡されます(例: `sh /tmp/ms/ms_boot.sh -P 1234`)。Web UI/テレビからも検証方式・自動取り込み・分類・重複排除を切り替えられます(`/tmp/ms/config`)。

## アンインストール

```sh
sh /tmp/ms/ms_boot.sh stop      # hdms を止めてメニューを純正に戻す
sh /tmp/ms/uninstall.sh         # hdms 停止・メニューを元に戻す・/tmp/ms を削除
```
再起動でも消えます。DvdPlayer が固まったときの復旧は hd900b-youtube-relay の `recover_dvdplayer.sh`、または `docs/TROUBLESHOOTING.md`。

## 制限事項

- 起動時の自動実行なし。電源再投入ごとに復元が必要。
- 転送速度は機器の実力: SD の読出し約 9 MB/s、HDD 書込み約 20 MB/s、検証読出し約 40 MB/s(HD900B 実測)。1.5 GB の SD を検証付きで約 4.5 分。
- テレビ側の進捗は手動更新。テレビからの閲覧/個別選択コピーは未対応(取り込みは媒体丸ごと)。
- DVD は一般的なデータディスクのみが対象(DRM 解除・市販映像の複製機能はありません)。**データディスクの取り込みは未検証**。
- 削除を含む操作は確認ダイアログのみで、元に戻せません。HDD のアンマウントは `force=1` が必要。
- 認証は LAN 内のプライベートアドレス制限と任意の PIN(平文の HTTP)。インターネットへ公開しないでください。
- FAT 媒体へは 4GB 超のファイルをコピーできません(事前にエラー表示)。

## ソースからビルドする

`docs/BUILD.md`。`build/build.sh`(Bootlin GCC 13.3 + uClibc-ng のツールチェーンを取得、SHA-256 検証)。`WITH_E2FS=1` で `mke2fs`/`e2fsck`/`rrpart` もビルド。

## リポジトリの構成

```
src/hdms/       hdms 本体(common.h storage.h jobs.h hdms.cpp)と rrpart.c
src/compat/     旧カーネル向け互換層(hd900b-youtube-relay と同じもの)
www/index.html  Web UI(ビルド時にバイナリへ埋め込み)
device/         端末で動かすスクリプト(ms_boot.sh、install_from_hdd.sh、uninstall.sh)
tools/          install.py、make_tv_rss.py、embed.py、hdtelnet.py ほか
build/          ビルド一式   dist/  ビルド済みバイナリ   tests/  試験   docs/  文書
```

## ライセンス

**GPL-3.0**(`LICENSE`)。サードパーティは `NOTICE.md`。

## 免責

本ソフトウェアは現状のまま無保証で提供されます。記録媒体のデータ消失・破損、機器の不具合について作者は責任を負いません。
重要なデータのバックアップを取ってから使ってください。HD900B、HiMedia、Realtek、Google 等とは無関係の非公式プロジェクトです。
