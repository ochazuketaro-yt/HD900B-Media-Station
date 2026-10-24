# 技術メモ(HD900B 実機で確認した事実)

## 旧カーネル(2.6.34) × uClibc-ng 1.0.50 の落とし穴
- `rename()` は `renameat2`(4351)を呼び ENOSYS → `syscall(4038)` を直接使用。
- `utime()/utimes()` は 64bit の `time_t` を 32bit ABI に渡し、ファイルの時刻が 1970 年になる → `syscall(4030)` を直接使用。
- `_FILE_OFFSET_BITS=64` が必須。無いと `lstat` が 2GiB 以上で失敗する。
- `O_DIRECT` は tmpfs で EINVAL(HDD 上では可。失敗時は fadvise にフォールバック)。
- `stat/statx/getrlimit/clock_gettime` 等は `src/compat`(hd900b-youtube-relay と共通)で補正。
- MIPS の ioctl 番号は `_IO(type,nr)` に `0x20000000` が付く(BLKRRPART = `0x2000125f`)。
- BusyBox 1.18.4 に `tail wc sort nohup setsid mkfs.* fdisk` は無い。`wget`(`--post-data`/`--post-file`)、`sha256sum`、`mount --bind`、`umount -l` はある。

## 構成・性能(実測)
- 接続: HDD は `sata_rtk`(SATA)、SD は `rtksdio`(ネイティブ MMC)、USB フラッシュは ehci。DVD ドライブは Linux 上では USB 接続の `sr0`。
- 速度: SD 読出し 約 9.4 MB/s、HDD 書込み 約 19〜22 MB/s(ext4、`nobarrier`)、HDD 読出し 約 48 MB/s、BusyBox の `sha256sum` 約 10 MB/s。
  → 検証は既定で XXH64(98MB で +2.4 秒)、SHA-256 は +約 17 秒。1525MB の SD 取り込み(重複 6 件スキップ、15 ファイル検証)が 263 秒。
- メモリ: 大容量ジョブ中の空きは約 33MB まで低下(buffers/cache を除く)したが異常なし。
- ディスクが無い DVD ドライブは `sysfs` のサイズが 2097151 を示すので、メディア有無は 2KiB の read で判定。

## 試験した範囲
アップロード(日本語名・多階層)、USB→HDD コピー(内容重複の検出)、SD→HDD(検証なし/XXH64/SHA-256)、SD 全体取り込み(`sha256sum` で独立確認)、取消(一時ファイルが残らない)、
SD 再挿入での自動取り込み、容量不足、FAT32 の 4GB 超、パストラバーサル、シンボリックリンク経由のアクセス、`X-MS` 無しの POST、Range ダウンロード、テレビ画面での進捗表示。
**未検証**: DVD データディスク、PIN 認証の実ブラウザ操作、ブラウザ上のコピー/ドラッグ&ドロップ操作の目視、長時間連続運用、電源断中のコピー。
