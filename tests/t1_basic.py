import sys, os, hashlib, json
sys.path.insert(0, os.path.dirname(__file__))
from msapi import *
rnd = lambda n: os.urandom(n)
files = {"/T1/a.txt": b"hello media station\n", "/T1/sub/b.bin": rnd(3_000_000), "/T1/sub/deep/c.bin": rnd(700_000), "/T1/日本語/写真.jpg": rnd(250_000), "/T1/dup_of_b.bin": None}
files["/T1/dup_of_b.bin"] = files["/T1/sub/b.bin"]
for p, d in files.items():
    r = put("usb1", p, d); print("PUT", p, r.get("ok"), r.get("xxh64"), r.get("error", ""))
print(json.dumps(get("/api/ls?v=usb1&p=/T1"), ensure_ascii=False)[:300])
