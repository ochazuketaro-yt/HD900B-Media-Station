# 互換性

- 動作確認: **HiMedia HD900B**(Realtek RTD1186、Linux 2.6.34-VENUS)のみ。
- 同じ RTD1186 系の他機種は、ブロックデバイス名(`sd*`/`mmcblk*`/`sr*`)と sysfs の構造、Premium Apps の RSS 構造が同じなら動く可能性があります(未検証)。
  インストーラは想定外のカーネルで警告を出し、`menu.rss` のパッチはアンカーが見つからなければ停止します。
- 動作報告は歓迎です(機種、ファームウェア、`uname -a`、`/proc/partitions`、`/tmp/ms/hdms.log`)。
