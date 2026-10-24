import sys, os, socket, json, urllib.request, urllib.parse
sys.path.insert(0, os.path.dirname(__file__))
from msapi import *
ok = True
def check(name, cond, info=""):
    global ok; ok &= bool(cond); print(("PASS " if cond else "FAIL ") + name, info)
def raw(req):
    s = socket.create_connection((os.environ["MS"].split("//")[1].split(":")[0], 8820), timeout=10); s.sendall(req.encode()); d = b""
    while True:
        c = s.recv(65536)
        if not c: break
        d += c
    return d.decode("latin1")
# path traversal
for p in ("/../../etc", "/a/../../..", "/%2e%2e/%2e%2e", "../../data"):
    r = get("/api/ls?v=usb1&p=" + urllib.parse.quote(p)) if False else None
    try: j = get("/api/ls?v=usb1&p=" + urllib.parse.quote(p)); check("traversal blocked " + p, False, str(j)[:80])
    except urllib.error.HTTPError as e: check("traversal blocked " + p, e.code in (400, 404))
try: get("/api/ls?v=nosuch&p=/"); check("unknown volume", False)
except urllib.error.HTTPError as e: check("unknown volume", e.code == 404)
# system paths are not volumes
try: get("/api/ls?v=..&p=/"); check("vol=..", False)
except urllib.error.HTTPError as e: check("vol=..", e.code == 404)
# put traversal
r = put("usb1", "/../evil.txt", b"x"); check("PUT traversal", not r.get("ok"), r)
r = put("usb1", "/T1/.x.mstmp", b"x"); check("PUT reserved name", not r.get("ok"), r)
# CSRF header
rq = "POST /api/mkdir?v=usb1&p=/csrf HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
check("POST without X-MS is refused", " 403 " in raw(rq).split("\r\n")[0], raw(rq).split("\r\n")[0])
# delete guards
r = post("/api/rm", [("v", "usb1"), ("p", "/")]); check("rm root refused", not r.get("ok"), r)
r = post("/api/rm", [("v", "usb1"), ("p", "/T1x"), ("confirm", "yes")]); check("rm missing", r.get("ok") is not None)
r = post("/api/umount", [("v", "hdd1")]); check("unmount HDD refused w/o force", not r.get("ok"), r)
# symlink escape: make one on the FAT? (FAT has no symlinks) -> on HDD ext4 via device shell
# oversized header
r = raw("GET /" + "a" * 40000 + " HTTP/1.1\r\n\r\n"); check("oversized request handled", "200" not in r[:15])
# range download
b = os.urandom(100000); put("usb1", "/T1/range.bin", b)
req = urllib.request.Request(B + "/dl?v=usb1&p=/T1/range.bin", headers={"Range": "bytes=1000-1999"}); rr = urllib.request.urlopen(req)
check("range 206", rr.status == 206 and rr.read() == b[1000:2000])
rr = urllib.request.urlopen(B + "/dl?v=usb1&p=/T1/range.bin"); check("full download identical", rr.read() == b)
print("ALL OK" if ok else "SOME FAILED")
