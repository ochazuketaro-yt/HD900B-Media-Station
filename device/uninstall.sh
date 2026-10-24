#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Undo everything: stop hdms, remove the menu bind mount, delete /tmp/ms.  (The firmware was never modified; files you copied stay on your disks.)
for p in $(pidof hdms 2>/dev/null); do kill $p 2>/dev/null; done
while grep -q "scripts/menu.rss" /proc/mounts; do umount /system/rtk_rootfs/usr/local/bin/scripts/menu.rss || break; done
rm -rf /tmp/ms
echo "removed. Restore bundles on disks (<disk>/.mediastation) are left in place; delete them by hand if you want."
