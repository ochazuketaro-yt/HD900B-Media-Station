#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""install.py -- install hd900b-media-station on a HiMedia HD900B over the LAN (Telnet + a temporary HTTP server on this PC).

    python3 tools/install.py --host <player-ip>

What it does (NOTHING is written to the firmware flash, /system or /data; everything goes to /tmp = RAM, plus one bind mount):
  1. logs in over Telnet (root, empty password) and checks that this looks like an RTD1186 / Linux 2.6.34 player
  2. starts a temporary HTTP server on this PC (the player's wget downloads from it)
  3. makes the player upload the menu.rss that is currently shown and its own stock youtube_menu.rss, patches them here (tools/make_tv_rss.py)
  4. downloads dist/hdms and the patched files to /tmp/ms on the player (SHA-256 checked)
  5. optionally keeps a copy on an attached disk (--bundle-dir) so that it can be restored after a power cycle without a PC
  6. runs /tmp/ms/ms_boot.sh  -> starts hdms (port 8820) and adds the "HD900B_MEDIA" tile to Premium Apps
Everything is gone after a power cycle: run the restore command printed at the end.   Python 3.8+, standard library only.
"""
import argparse
import hashlib
import http.server
import os
import socket
import socketserver
import sys
import tempfile
import threading

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from hdtelnet import Device          # noqa: E402
import make_tv_rss                   # noqa: E402

SCRIPTS = "/system/rtk_rootfs/usr/local/bin/scripts"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def local_ip_towards(host):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((host, 9))
        return s.getsockname()[0]
    finally:
        s.close()


class Handler(http.server.SimpleHTTPRequestHandler):
    serve_dir = "."

    def __init__(self, *a, **k):
        super().__init__(*a, directory=Handler.serve_dir, **k)

    def log_message(self, *a):
        pass

    def do_POST(self):          # the player uploads its RSS files here:  wget --post-file=...
        name = os.path.basename(self.path)
        if not name.endswith(".rss"):
            self.send_response(400); self.end_headers(); return
        n = int(self.headers.get("Content-Length", "0"))
        with open(os.path.join(Handler.serve_dir, "upload_" + name), "wb") as f:
            f.write(self.rfile.read(n))
        self.send_response(200); self.send_header("Content-Length", "0"); self.end_headers()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True, help="IP address of the HD900B")
    ap.add_argument("--user", default="root"); ap.add_argument("--password", default="")
    ap.add_argument("--pc-ip", help="this PC's IP as seen from the player (auto-detected)")
    ap.add_argument("--port", type=int, default=8899, help="temporary HTTP server port on this PC (default 8899)")
    ap.add_argument("--hdms", default=os.path.join(ROOT, "dist", "hdms"), help="daemon binary (default: dist/hdms)")
    ap.add_argument("--bundle-dir", help="mounted disk on the player (e.g. /tmp/usbmounts/sdb1): also store a restore bundle in <dir>/.mediastation/bundle")
    ap.add_argument("--yes", action="store_true", help="do not ask for confirmation")
    a = ap.parse_args()

    if not os.path.isfile(a.hdms):
        sys.exit("%s がありません。dist/hdms を用意するか、build/build.sh でビルドしてください。" % a.hdms)
    pc_ip = a.pc_ip or local_ip_towards(a.host)
    print("== HD900B (%s) に接続します / PC は %s:%d" % (a.host, pc_ip, a.port))
    dev = Device(a.host, user=a.user, password=a.password)
    rc, out = dev.run("uname -a; grep -m1 'cpu model' /proc/cpuinfo; cat /proc/partitions")
    print(out)
    if "2.6.34" not in out or "mips" not in out.lower():
        print("!! 想定外のカーネルです(動作確認は HD900B / Linux 2.6.34-VENUS / MIPS 24Kc のみ)。")
        if not a.yes and input("続行しますか? [y/N] ").lower() != "y":
            sys.exit(1)
    elif not a.yes:
        print("このあと /tmp/ms にファイルを置き、Premium Apps に HD900B_MEDIA タイルを追加します(フラッシュ・/data は変更しません)。")
        if input("続行しますか? [y/N] ").lower() != "y":
            sys.exit(1)

    tmp = tempfile.mkdtemp(prefix="msinstall_")
    Handler.serve_dir = tmp
    for name in ("ms_boot.sh", "install_from_hdd.sh", "uninstall.sh"):
        with open(os.path.join(ROOT, "device", name), "rb") as f, open(os.path.join(tmp, name), "wb") as g:
            g.write(f.read().replace(b"\r\n", b"\n"))
    with open(a.hdms, "rb") as f, open(os.path.join(tmp, "hdms"), "wb") as g:
        g.write(f.read())

    socketserver.TCPServer.allow_reuse_address = True
    httpd = socketserver.ThreadingTCPServer(("0.0.0.0", a.port), Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    base = "http://%s:%d" % (pc_ip, a.port)
    try:
        print("== 端末の menu.rss(現在表示中のもの)と純正 youtube_menu.rss を取得します")
        for n in ("menu.rss", "youtube_menu.rss"):
            rc, out = dev.run("wget -q -O /dev/null --post-file=%s/%s %s/upload/%s" % (SCRIPTS, n, base, n))
            if rc != 0 or not os.path.isfile(os.path.join(tmp, "upload_" + n)):
                sys.exit("%s を取得できませんでした: %s" % (n, out))
        rd = lambda p: open(p, encoding="utf-8", newline="").read()
        wr = lambda p, t: open(p, "w", encoding="utf-8", newline="").write(t)
        wr(os.path.join(tmp, "ms_menu.rss"), make_tv_rss.patch_menu(rd(os.path.join(tmp, "upload_menu.rss"))))
        wr(os.path.join(tmp, "ms_ui.rss"), make_tv_rss.patch_ui(rd(os.path.join(tmp, "upload_youtube_menu.rss"))))
        print("   パッチ適用済み: ms_menu.rss ms_ui.rss")

        print("== /tmp/ms にファイルを配置します")
        dev.run("mkdir -p /tmp/ms/tv /tmp/ms/jobs")
        dest = {"hdms": "/tmp/ms/hdms", "ms_boot.sh": "/tmp/ms/ms_boot.sh", "uninstall.sh": "/tmp/ms/uninstall.sh", "ms_ui.rss": "/tmp/ms/tv/ms_ui.rss", "ms_menu.rss": "/tmp/ms/tv/ms_menu.rss"}
        dev.run("for p in $(pidof hdms); do kill $p; done; sleep 1")
        for n, d in dest.items():
            rc, out = dev.run("wget -q -O %s %s/%s" % (d, base, n), timeout=180)
            if rc != 0:
                sys.exit("%s のダウンロードに失敗: %s" % (n, out))
        dev.run("chmod +x /tmp/ms/hdms /tmp/ms/ms_boot.sh /tmp/ms/uninstall.sh")
        rc, out = dev.run("sha256sum %s" % " ".join(dest.values()))
        have = {l.split()[1]: l.split()[0] for l in out.splitlines() if len(l.split()) == 2}
        for n, d in dest.items():
            ok = have.get(d) == sha256(os.path.join(tmp, n))
            print("   %-12s %s" % (n, "OK" if ok else "MISMATCH"))
            if not ok:
                sys.exit("ハッシュが一致しません。中止します。")

        if a.bundle_dir:
            b = a.bundle_dir.rstrip("/") + "/.mediastation/bundle"
            print("== 復元用バンドルを %s に保存します" % b)
            dev.run("mkdir -p %s" % b)
            for n in ("hdms", "ms_boot.sh", "install_from_hdd.sh", "ms_ui.rss", "ms_menu.rss"):
                rc, out = dev.run("wget -q -O %s/%s %s/%s" % (b, n, base, n), timeout=180)
                if rc != 0:
                    print("   !! %s を保存できませんでした: %s" % (n, out))

        print("== ms_boot.sh を実行します")
        rc, out = dev.run("sh /tmp/ms/ms_boot.sh", timeout=90)
        print(out)
        rc, out = dev.run("wget -q -O - http://127.0.0.1:8820/health")
        print("   health:", out.strip() or "(応答なし)")
        if out.strip() != "ok":
            print("!! hdms が応答しません。/tmp/ms/stdout.log と /tmp/ms/hdms.log を確認してください。")
    finally:
        httpd.shutdown()
        dev.close()
    print("\n完了。PC のブラウザで  http://%s:8820/  を開いてください。" % a.host)
    print("テレビ: ホーム → Premium Apps → HD900B_MEDIA タイル。")
    print("電源を入れ直した後は Telnet で:  sh /tmp/ms/ms_boot.sh が消えています。--bundle-dir を使った場合は")
    print("  sh <dir>/.mediastation/bundle/install_from_hdd.sh   (それ以外はこのインストーラをもう一度実行)")


if __name__ == "__main__":
    main()
