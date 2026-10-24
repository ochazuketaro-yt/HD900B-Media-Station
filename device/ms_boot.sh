#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Media Station start script.  Non-persistent: everything lives in /tmp plus ONE bind mount over Premium Apps' menu.rss.  Idempotent.
#   sh /tmp/ms/ms_boot.sh [hdms options]   start hdms and (re)apply the menu that contains the HD900B_MEDIA tile
#   sh /tmp/ms/ms_boot.sh stop             stop hdms and remove the menu bind mount (stock menu again)
# Coexistence with hd900b-youtube-relay: if /data/hd/boot.sh exists it is run first (when its relay is not running), and
# install.py builds the menu from whatever menu is on screen at install time, so the YouTube tile stays.
MS=/tmp/ms
MENU=/system/rtk_rootfs/usr/local/bin/scripts/menu.rss
mkdir -p $MS/tv $MS/jobs
unmenu() { while grep -q "scripts/menu.rss" /proc/mounts; do umount $MENU || break; done; }
for p in $(pidof hdms); do kill $p 2>/dev/null; done
if [ "$1" = "stop" ]; then
  unmenu
  echo "hdms stopped; stock menu restored (re-run /data/hd/boot.sh if you use the YouTube relay alone)"; exit 0
fi
if [ -x /data/hd/boot.sh ] && [ -z "$(pidof hdrelay)" ]; then sh /data/hd/boot.sh >/dev/null 2>&1; fi
sleep 1
chmod +x $MS/hdms
( cd $MS && trap '' HUP && ./hdms -p 8820 "$@" </dev/null >$MS/stdout.log 2>&1 & )
unmenu
mount --bind $MS/tv/ms_menu.rss $MENU
sleep 2
echo "hdms pid: $(pidof hdms)  menu bind: $(grep -c 'scripts/menu.rss' /proc/mounts)"
echo "web UI: http://$(ifconfig eth0 2>/dev/null | sed -n 's/.*inet addr:\([0-9.]*\).*/\1/p'):8820/"
