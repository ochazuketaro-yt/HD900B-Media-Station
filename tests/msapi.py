"""tiny client for the hdms API (used by the on-device test scripts)"""
import json, time, urllib.request, urllib.parse, hashlib, os, sys
B = os.environ["MS"]   # e.g. MS=http://<player-ip>:8820
def get(path, timeout=30): return json.load(urllib.request.urlopen(B + path, timeout=timeout))
def post(path, data=None, timeout=60):
    body = urllib.parse.urlencode(data or [], doseq=True).encode()
    req = urllib.request.Request(B + path, data=body, method="POST", headers={"X-MS": "1"})
    try: return json.load(urllib.request.urlopen(req, timeout=timeout))
    except urllib.error.HTTPError as e: return json.loads(e.read().decode())
def put(v, p, data, timeout=300):
    req = urllib.request.Request(B + "/api/put?v=%s&p=%s" % (v, urllib.parse.quote(p)), data=data, method="PUT", headers={"X-MS": "1", "Content-Type": "application/octet-stream"})
    try: return json.load(urllib.request.urlopen(req, timeout=timeout))
    except urllib.error.HTTPError as e: return json.loads(e.read().decode())
def wait(jid, show=True, poll=1.0, timeout=3600):
    t0 = time.time(); last = ""
    while time.time() - t0 < timeout:
        j = get("/api/job?id=" + jid)
        line = "%-9s %-8s %5.1f%% files %d/%d  copy %.1f/%.1f MB verify %.1f MB  %.2f MB/s eta %ss  %s" % (
            j["state"], j["phase"], 100.0 * (j["bytes_done"] + j["vbytes_done"]) / max(1, j["bytes_total"] * (1 if j["verify"] == "none" else 2)),
            j["files_done"], j["files_total"], j["bytes_done"] / 1e6, j["bytes_total"] / 1e6, j["vbytes_done"] / 1e6, j["speed"] / 1e6, int(j["eta"]), j["cur"][-40:])
        if show and line != last: print(line); last = line
        if j["state"] in ("done", "done_with_errors", "error", "cancelled"): return j
        time.sleep(poll)
    raise TimeoutError(jid)
