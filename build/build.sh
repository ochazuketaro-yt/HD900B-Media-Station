#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build dist/hdms (static MIPS32r2 o32 executable for the HD900B) from source.   Optional: WITH_E2FS=1 also builds mke2fs/e2fsck.
#   build/build.sh                     fetch the toolchain, build
#   TOOLCHAIN_DIR=/opt/mips32el--uclibc--stable-2024.05-1 build/build.sh      reuse an extracted toolchain
# Host needs: bash, make, gcc (only for WITH_E2FS), python3, xz, tar, sha256sum.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); ROOT=$(dirname "$HERE"); . "$HERE/config.sh"
DL_DIR=${DL_DIR:-$HERE/dl}; WORK=${WORK:-$HERE/work}; export DL_DIR; mkdir -p "$WORK" "$ROOT/dist"
export TOOLCHAIN_DIR=${TOOLCHAIN_DIR:-}
"$HERE/fetch.sh"
if [ -z "$TOOLCHAIN_DIR" ]; then TOOLCHAIN_DIR=$WORK/$TOOLCHAIN_NAME; [ -d "$TOOLCHAIN_DIR" ] || tar -C "$WORK" -xf "$DL_DIR/${TOOLCHAIN_NAME}.tar.xz"; fi
export PATH=$TOOLCHAIN_DIR/bin:$PATH
CC=mipsel-linux-gcc; CXX=mipsel-linux-g++; STRIP=mipsel-linux-strip
command -v "$CC" >/dev/null || { echo "cross compiler not found in $TOOLCHAIN_DIR/bin" >&2; exit 1; }
"$CC" --version | head -1
F="$HD_CFLAGS -ffile-prefix-map=$ROOT=/src -ffile-prefix-map=$WORK=/build"
mkdir -p "$WORK/o"
python3 "$ROOT/tools/embed.py" "$ROOT/www/index.html" "$ROOT/src/hdms/ui_html.h"
$CC $F -no-pie -Wall -Wno-missing-attributes -Wno-nonnull-compare -c "$ROOT/src/compat/hd900b_compat.c" -o "$WORK/o/hd900b_compat.o"
# _FILE_OFFSET_BITS=64: without it stat() fails with EOVERFLOW on files over 2 GiB
$CXX -std=c++17 -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE $F -no-pie -static -Wall -Wno-misleading-indentation -Wno-unused-parameter \
  "$ROOT/src/hdms/hdms.cpp" "$WORK/o/hd900b_compat.o" -o "$WORK/hdms.unstripped" -Wl,-u,__xstat64_conv
$STRIP -o "$ROOT/dist/hdms" "$WORK/hdms.unstripped"
( cd "$ROOT/dist" && sha256sum hdms > SHA256SUMS && cat SHA256SUMS )
"$ROOT/tools/check_newsyscalls.sh" "$WORK/hdms.unstripped" || true
if [ -n "${WITH_E2FS:-}" ]; then
  echo "== e2fsprogs $E2FS_VER (mke2fs, e2fsck)"
  [ -d "$WORK/e2fsprogs-$E2FS_VER" ] || tar -C "$WORK" -xf "$DL_DIR/e2fsprogs-${E2FS_VER}.tar.xz"
  mkdir -p "$WORK/e2" && cd "$WORK/e2"
  PKG_CONFIG="$HERE/fake-pkg-config" CC=$CC AR=mipsel-linux-ar RANLIB=mipsel-linux-ranlib "$WORK/e2fsprogs-$E2FS_VER/configure" --host=mipsel-linux CFLAGS="$HD_CFLAGS" LDFLAGS=-static \
    --disable-nls --disable-debugfs --disable-imager --disable-resizer --disable-defrag --disable-fuse2fs --disable-uuidd --enable-libuuid --enable-libblkid \
    --disable-elf-shlibs --disable-bsd-shlibs --disable-e2initrd-helper --prefix=/ >configure.log 2>&1
  make -j"$(nproc)" >make.log 2>&1
  $STRIP -o "$ROOT/dist/mke2fs" misc/mke2fs; $STRIP -o "$ROOT/dist/e2fsck" e2fsck/e2fsck
  $CC $F -static -o "$ROOT/dist/rrpart" "$ROOT/src/hdms/rrpart.c"; $STRIP "$ROOT/dist/rrpart"
  ls -la "$ROOT/dist"
fi
echo "done: $ROOT/dist/hdms"
