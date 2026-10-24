# Changelog

## 0.4.0 — 初回公開

- HD900B(Realtek RTD1186、Linux 2.6.34)で動作確認。
- ストレージ自動検出(USB/SD/SATA/DVD)、自動マウント、LAN の Web UI、JSON API、Premium Apps 用 RSS 画面。
- コピーエンジン: 読み戻し検証(XXH64 / SHA-256)、原子的確定、重複排除、衝突回避、種類別分類、マニフェスト、取消、容量事前検査、SD 挿入時の自動取り込み。
- 旧カーネル(2.6.34)向けの回避: `rename`(renameat2 → 旧 syscall)、`utime`(64bit time_t 不整合)、2GB 超ファイル(`_FILE_OFFSET_BITS=64`)。
- インストーラ(`tools/install.py`)、復元スクリプト、HDD の ext4 化手順。
- 既知の制限は README を参照(自動起動なし、DVD データディスクは未検証、など)。
