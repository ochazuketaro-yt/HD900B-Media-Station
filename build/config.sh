# SPDX-License-Identifier: GPL-3.0-or-later
TOOLCHAIN_NAME="mips32el--uclibc--stable-2024.05-1"
TOOLCHAIN_URL="https://toolchains.bootlin.com/downloads/releases/toolchains/mips32el/tarballs/${TOOLCHAIN_NAME}.tar.xz"
TOOLCHAIN_SHA256="ae4400a934074a3b1b39fb7ec79fb535da4e65b668f2330ca14576d072f6b13e"
# only needed to format a disk as ext4 on the player (see docs/FORMAT-HDD.md)
E2FS_VER="1.47.2"
E2FS_URL="https://mirrors.edge.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v${E2FS_VER}/e2fsprogs-${E2FS_VER}.tar.xz"
E2FS_SHA256="08242e64ca0e8194d9c1caad49762b19209a06318199b63ce74ae4ef2d74e63c"
HD_CFLAGS="-march=mips32r2 -mtune=24kc -mfp32 -mhard-float -fno-pie -O2"
