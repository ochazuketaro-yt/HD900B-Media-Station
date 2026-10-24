#!/bin/sh
# Restore the Media Station after a power cycle straight from the HDD (no PC needed):
#   sh /tmp/usbmounts/sdb1/.mediastation/bundle/install_from_hdd.sh
B=$(dirname "$0"); MS=/tmp/ms
mkdir -p $MS/tv $MS/jobs
cp -f $B/hdms $MS/hdms && cp -f $B/ms_boot.sh $MS/ms_boot.sh && cp -f $B/ms_ui.rss $B/ms_menu.rss $MS/tv/
chmod +x $MS/hdms
sh $MS/ms_boot.sh
