#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Download (and SHA-256 verify) the toolchain [and e2fsprogs with WITH_E2FS=1] into $DL_DIR (default build/dl).
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); . "$HERE/config.sh"
DL_DIR=${DL_DIR:-$HERE/dl}; mkdir -p "$DL_DIR"
download() {   # download <sha256> <file> <url>
  local sha=$1 file=$2 url=$3
  if [ -f "$DL_DIR/$file" ] && echo "$sha  $DL_DIR/$file" | sha256sum -c --status; then echo "ok (cached)  $file"; return; fi
  echo "fetching $url"
  python3 -c 'import sys,urllib.request;urllib.request.urlretrieve(sys.argv[1],sys.argv[2])' "$url" "$DL_DIR/$file"
  echo "$sha  $DL_DIR/$file" | sha256sum -c --status || { echo "SHA-256 mismatch for $file" >&2; rm -f "$DL_DIR/$file"; exit 1; }
  echo "ok  $file"
}
[ -n "${TOOLCHAIN_DIR:-}" ] || download "$TOOLCHAIN_SHA256" "${TOOLCHAIN_NAME}.tar.xz" "$TOOLCHAIN_URL"
[ -z "${WITH_E2FS:-}" ] || download "$E2FS_SHA256" "e2fsprogs-${E2FS_VER}.tar.xz" "$E2FS_URL"
