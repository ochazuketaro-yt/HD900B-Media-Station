# HDD を ext4 にする(実機で実施した手順)

**ディスクの内容が全て消えます。デバイス名を必ず確認してください。内蔵フラッシュ(mtd*)には絶対に実行しないこと。**
以下は HD900B の SATA HDD(`sdb`、Hitachi 300GB)で実施した手順です。スクリプト化はしていません(誤消去を避けるため手で実行)。

1. `WITH_E2FS=1 build/build.sh` で `dist/mke2fs` `dist/rrpart` を作り、端末の `/tmp/ms` へ転送。
2. 識別: `cat /proc/partitions`、`cat /sys/block/sdb/device/model`、`readlink /sys/block/sdb`(`sata_rtk` を含む)、`cat /sys/block/sdb/size`。
3. MBR(1 パーティション、2048 セクタ開始、type 0x83、末尾まで)を PC で作成して `dd if=mbr.bin of=/dev/block/sdb bs=512 count=1`。
4. `./rrpart /dev/block/sdb`(BLKRRPART。**MIPS では ioctl 番号に 0x20000000 が付く**: `0x2000125f`)。`/proc/partitions` に `sdb1` が出る。
5. `./mke2fs -F -q -t ext4 -O ^metadata_csum,^64bit,^orphan_file,uninit_bg -b 4096 -i 262144 -m 1 -J size=128 -L MEDIA -E lazy_itable_init=1,lazy_journal_init=1 /dev/block/sdb1`
   (Linux 2.6.34 が理解できる機能だけにする。300GB で約 1 分)
6. `hdms` が数秒以内に `/tmp/usbmounts/sdb1` へ自動マウントします。

ext4 は PC の Linux では読めますが、Windows/macOS では追加ソフトが必要です。PC とも直接やりとりしたい媒体は FAT32(4GB 制限)または exFAT/NTFS にしてください(`mkfs` はこのプレーヤー上では用意されていません)。
