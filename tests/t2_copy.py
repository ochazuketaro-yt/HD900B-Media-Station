import sys, os, json
sys.path.insert(0, os.path.dirname(__file__))
from msapi import *
r = post("/api/copy", [("src", "usb1:/T1"), ("dst", "hdd1"), ("dstp", "/Tests/run1"), ("verify", sys.argv[1] if len(sys.argv) > 1 else "fast"), ("label", "t2")])
print(r); j = wait(r["id"])
print(json.dumps({k: j[k] for k in ("state", "files_total", "copied", "skipped", "dups", "renamed", "verified", "errors_n", "errors")}, ensure_ascii=False))
print(get("/api/job?id=%s&log=1" % r["id"]) if False else urllib.request.urlopen(B + "/api/job?id=%s&log=1" % r["id"]).read().decode())
