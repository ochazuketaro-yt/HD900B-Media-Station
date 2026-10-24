# ビルド

```sh
bash build/build.sh                                  # ツールチェーン(Bootlin mips32el uclibc 2024.05-1)を取得して dist/hdms を作る
TOOLCHAIN_DIR=/opt/mips32el--uclibc--stable-2024.05-1 bash build/build.sh   # 展開済みを再利用
WITH_E2FS=1 bash build/build.sh                      # + dist/mke2fs, dist/e2fsck, dist/rrpart(HDD を ext4 にする場合)
```
ホストに必要なもの: bash, python3, xz, tar, sha256sum(`WITH_E2FS=1` では make, gcc も)。ネットワークは取得時のみ。
重要なコンパイル条件(`build/build.sh` に反映済み): `-D_FILE_OFFSET_BITS=64`(無いと 2GB 超で stat が EOVERFLOW)、互換層を先にリンクし `-Wl,-u,__xstat64_conv`。
Web UI は `tools/embed.py` が `www/index.html` を `src/hdms/ui_html.h`(生成物、git 管理外)に変換して内蔵します。
`tools/check_newsyscalls.sh dist/…` は旧カーネルに無い syscall の直接呼出し(statx/prlimit64/getrandom/renameat2)の混入を静的に調べます。

ホスト試験: `g++ -std=c++17 tests/hash_test.cpp -o /tmp/hash_test && /tmp/hash_test`(XXH64 のテストベクタ、`file` 引数で SHA-256 と XXH64 を表示)。
実機試験: `MS=http://<ip>:8820 python3 tests/t1_basic.py`(アップロード)、`t2_copy.py`、`t3_security.py`。`tests/syscall_probe.c` と `mtime_probe.c` は旧カーネルでの libc の挙動確認用(互換層とリンクして実機で実行)。
注意: 試験スクリプトは**接続中の媒体に書き込みます**(`usb1` に `/T1`、`hdd1` に `/Tests` を作成)。
