# NOTICE — サードパーティ・ソフトウェアとライセンス

`hd900b-media-station` は **GNU General Public License v3**(`LICENSE`)で配布されます。

| コンポーネント | ライセンス | 備考 |
|---|---|---|
| hdms(`src/hdms`、`www`、`tools`、`device`) | GPL-3.0-or-later | 本リポジトリ。SHA-256 と XXH64 は本リポジトリ内の独自実装(XXH64 はアルゴリズム仕様に基づく自前実装で、公式テストベクタ 3 件との一致を確認) |
| 互換層 `src/compat/hd900b_compat.c` | GPL-3.0-or-later | hd900b-youtube-relay と同じもの |
| uClibc-ng 1.0.50(C ライブラリ、`dist/hdms` に静的リンク) | **LGPL-2.1**(`licenses/LGPL-2.1.txt`) | Bootlin の GCC 13.3 ツールチェーンに同梱。LGPL-2.1 §6 のため、完全なソースとビルド手順(`src/`、`build/`)を公開しています。別の C ライブラリで再リンクしたい場合は `build/build.sh` のコンパイラを差し替えてください |
| libstdc++ / libgcc | GPL-3.0 + GCC Runtime Library Exception 3.1 | 例外条項により、リンクしたプログラムのライセンスに追加の制約は生じません |
| e2fsprogs 1.47.2(`mke2fs`/`e2fsck`) | GPL-2.0(一部 LGPL-2.0/BSD/MIT) | **配布物には含みません**。`WITH_E2FS=1 build/build.sh` が kernel.org から取得(SHA-256 検証)してビルドします。ビルドしたバイナリを再配布する場合は e2fsprogs のライセンスに従ってください |

## 配布していないもの
- **ファームウェアの `menu.rss` / `youtube_menu.rss`**: メーカー/Realtek の著作物。インストーラが**お使いの端末から**読み出し、手元でパッチします(`tools/make_tv_rss.py`)。
- **Bootlin のクロスツールチェーン**、**e2fsprogs のソース**: 公式サイトから取得します。

## 商標・非関与
HiMedia、Realtek、その他の名称は各社の商標です。本プロジェクトはこれらの会社と提携・承認関係になく、非公式です。
