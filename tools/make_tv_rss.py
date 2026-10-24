#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""make_tv_rss.py -- derive the two Premium Apps files for the Media Station from the player's OWN stock firmware files.

The stock files are firmware components and are never shipped in this repository: install.py reads them from your player.

  ms_menu.rss   the menu.rss that is on screen + one new tile "HD900B_MEDIA" (opens ms_ui.rss).  If the menu already contains
                the tile it is left alone, and tiles added by other add-ons (e.g. hd900b-youtube-relay) are preserved.
  ms_ui.rss     the stock YouTube list screen with its <submenu> list replaced by the Media Station sections; the right-hand
                list is served by hdms (http://127.0.0.1:8820/tv/*.rss); item descriptions are shown under the titles.

Both edits are plain text edits with asserted anchors; on firmware that differs from the tested one the script stops instead of guessing."""
import sys

HOST = "127.0.0.1:8820"
TILE = "HD900B_MEDIA"
UI_PATH = "rss_file:///tmp/ms/tv/ms_ui.rss"
ICON = "image/vod"          # artwork already present in the firmware (used as the tile icon)


def patch_menu(s: str) -> str:
    if TILE in s:
        return s
    a = s.index('title == "Shoutcast"')
    k = s.index('\telse\n\t{\n\t\tnull;\n\t}', a)
    s = s[:k] + '\telse if (title == "%s")\n\t{\n\t\t"%s";\n\t}\n' % (TILE, UI_PATH) + s[k:]
    m = s.index('itemTitleArray, "Shoutcast");')
    m = s.index("}\n", m) + 2
    tile = ('\n{\n\titemTitleArray = pushBackStringArray(itemTitleArray, "%s");\n'
            '\tfocusiconArray = pushBackStringArray(focusiconArray, "%s_focus.fsp");\n'
            '\tunfocusiconArray = pushBackStringArray(unfocusiconArray, "%s_unfocus.fsp");\n'
            '\titemSize = Add(itemSize, 1);\n}\n') % (TILE, ICON, ICON)
    return s[:m] + tile + s[m:]


def action(title, cmd, then):
    return ('<submenu>\n<title>%s</title>\n<onClick>\n  <script>\n    HIMEDIA_System("wget -q -O /dev/null http://%s/tv/do?c=%s");\n'
            '    url = "http://%s/tv/%s.rss";\n  </script>\n</onClick>\n<media:thumbnail url="image/youtube.gif" width="120" height="90" />\n</submenu>\n') % (title, HOST, cmd, HOST, then)


def link(title, page):
    return '<submenu>\n<title>%s</title>\n<link>http://%s/tv/%s.rss</link>\n<media:thumbnail url="image/youtube.gif" width="120" height="90" />\n</submenu>\n' % (title, HOST, page)


def patch_ui(s: str) -> str:
    a = s.index("<submenu>")
    b = s.rindex("</submenu>") + len("</submenu>")
    subs = (link("STORAGE", "storage") + action("IMPORT: SD CARD to HDD", "import_sd", "progress") + action("IMPORT: USB to HDD", "import_usb", "progress") +
            action("IMPORT: DVD disc to HDD", "import_dvd", "progress") + link("PROGRESS (select to refresh)", "progress") + action("CANCEL TRANSFER", "cancel", "progress") +
            link("RECENT MEDIA (play from HDD)", "recent") + link("NETWORK ACCESS (PC)", "network") +
            action("AUTO-IMPORT on/off", "auto_toggle", "network") + action("VERIFY MODE", "verify_next", "network") + action("SORT BY TYPE on/off", "sort_toggle", "network"))
    s = s[:a] + subs + s[b:]
    i = s.index('<text offsetXPC=22.02 offsetYPC=67 widthPC=35')
    j = s.index("</itemDisplay>")
    desc = ('<text offsetXPC=22.02 offsetYPC=42 widthPC=74 heightPC=50 fontSize=12 lines=3 backgroundColor=-1:-1:-1 foregroundColor=0:0:0>\n'
            '\t\t<script>\n\t\t\ttmp = getItemInfo("description");\n\t\t\tif (tmp != null) { tmp; } else { ""; }\n\t\t</script>\n\t</text>\n\n')
    s = s[:i] + desc + s[j:]
    for old, new in (("<bookmark> Youtube_menu </bookmark>", "<bookmark> MediaStation_menu </bookmark>"), ("itemPerPage=4", "itemPerPage=3")):
        assert old in s, "anchor not found: " + old
        s = s.replace(old, new)
    return s


if __name__ == "__main__":
    if len(sys.argv) != 5:
        sys.exit("usage: make_tv_rss.py current_menu.rss stock_youtube_menu.rss out_ms_menu.rss out_ms_ui.rss")
    rd = lambda p: open(p, encoding="utf-8", newline="").read()
    wr = lambda p, t: open(p, "w", encoding="utf-8", newline="").write(t)
    wr(sys.argv[3], patch_menu(rd(sys.argv[1])))
    wr(sys.argv[4], patch_ui(rd(sys.argv[2])))
    print("wrote", sys.argv[3], sys.argv[4])
