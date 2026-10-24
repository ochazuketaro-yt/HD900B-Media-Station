// SPDX-License-Identifier: GPL-3.0-or-later
// hdms -- HD900B Media Station daemon.  One static MIPS binary: storage detection, LAN web UI + JSON API, copy/verify jobs,
// RSS pages for the TV.  Fork-per-connection, fork-per-job, no threads (uClibc-ng on Linux 2.6.34 + the hd900b compat layer).
#include "jobs.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/ioctl.h>
#include "ui_html.h"
#include <net/if.h>

using namespace ms;

static const char *VERSION = "hdms 0.4";

struct Config {
    int port = 8820;
    std::string bind = "0.0.0.0";
    std::string pin;                 // empty = LAN open (private addresses only)
    bool autoimport = false;
    std::string verify = "fast";
    bool sort = false;
    bool dedupe = true;
    int tz = 9;
};
static Config CFG;
static long long START_MS;
static int NCHILD = 0;

static void logmsg(const char *fmt, ...) {
    char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    time_t t = time(nullptr); struct tm tm; gmtime_r(&t, &tm);
    char ts[32]; strftime(ts, sizeof ts, "%m-%d %H:%M:%S", &tm);
    int fd = open((std::string(STATE_DIR) + "/hdms.log").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { std::string l = std::string(ts) + " " + b + "\n"; if (write(fd, l.data(), l.size()) < 0) {} close(fd); }
}

// ---------------------------------------------------------------------------------------------- HTTP plumbing
struct Req {
    std::string method, target, path, query, body_prefix;
    std::map<std::string, std::string> h;
    Params q;
    long long clen = 0;
    std::string peer; bool loopback = false;
    int fd = -1;
};

static bool send_all(int fd, const char *p, size_t n) {
    while (n) { ssize_t w = send(fd, p, n, MSG_NOSIGNAL); if (w <= 0) { if (w < 0 && errno == EINTR) continue; return false; } p += w; n -= w; }
    return true;
}
static bool respond(const Req &r, int code, const std::string &ctype, const std::string &body, const std::string &extra = "") {
    const char *msg = code == 200 ? "OK" : code == 201 ? "Created" : code == 206 ? "Partial Content" : code == 400 ? "Bad Request" : code == 401 ? "Unauthorized" :
                      code == 403 ? "Forbidden" : code == 404 ? "Not Found" : code == 409 ? "Conflict" : code == 413 ? "Payload Too Large" : code == 416 ? "Range Not Satisfiable" :
                      code == 500 ? "Internal Server Error" : code == 503 ? "Service Unavailable" : "OK";
    std::string h = strf("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n%s\r\n", code, msg, ctype.c_str(), body.size(), extra.c_str());
    if (!send_all(r.fd, h.data(), h.size())) return false;
    if (r.method != "HEAD" && !body.empty()) return send_all(r.fd, body.data(), body.size());
    return true;
}
static bool jerr(const Req &r, int code, const std::string &msg) { return respond(r, code, "application/json; charset=utf-8", "{\"ok\":false,\"error\":" + jstr(msg) + "}"); }
static bool jok(const Req &r, const std::string &body) { return respond(r, 200, "application/json; charset=utf-8", body); }

static bool private_ip(uint32_t ip) {   // host order
    return (ip >> 24) == 127 || (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 || (ip >> 16) == 0xA9FE;
}

static std::string ctype_of(const std::string &name) {
    size_t d = name.rfind('.'); std::string e = d == std::string::npos ? "" : lower(name.substr(d + 1));
    static const std::map<std::string, std::string> m = {
        {"html", "text/html; charset=utf-8"}, {"htm", "text/html; charset=utf-8"}, {"txt", "text/plain; charset=utf-8"}, {"log", "text/plain; charset=utf-8"},
        {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"png", "image/png"}, {"gif", "image/gif"}, {"bmp", "image/bmp"}, {"webp", "image/webp"}, {"svg", "image/svg+xml"},
        {"mp4", "video/mp4"}, {"m4v", "video/mp4"}, {"mkv", "video/x-matroska"}, {"webm", "video/webm"}, {"avi", "video/x-msvideo"}, {"mov", "video/quicktime"}, {"ts", "video/mp2t"}, {"mpg", "video/mpeg"},
        {"mp3", "audio/mpeg"}, {"flac", "audio/flac"}, {"wav", "audio/wav"}, {"m4a", "audio/mp4"}, {"aac", "audio/aac"}, {"ogg", "audio/ogg"},
        {"pdf", "application/pdf"}, {"json", "application/json"}, {"zip", "application/zip"}};
    auto it = m.find(e); return it == m.end() ? "application/octet-stream" : it->second;
}

static bool read_request(int fd, Req &r) {
    std::string buf; char tmp[4096];
    while (buf.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        buf.append(tmp, n);
        if (buf.size() > 32768) return false;
    }
    size_t he = buf.find("\r\n\r\n");
    std::string head = buf.substr(0, he); r.body_prefix = buf.substr(he + 4);
    size_t le = head.find("\r\n"); std::string rl = head.substr(0, le == std::string::npos ? head.size() : le);
    size_t s1 = rl.find(' '), s2 = rl.rfind(' '); if (s1 == std::string::npos || s2 == s1) return false;
    r.method = rl.substr(0, s1); r.target = rl.substr(s1 + 1, s2 - s1 - 1);
    size_t qm = r.target.find('?'); r.path = url_decode(r.target.substr(0, qm), false); r.query = qm == std::string::npos ? "" : r.target.substr(qm + 1);
    r.q = parse_params(r.query);
    size_t i = le == std::string::npos ? head.size() : le + 2;
    while (i < head.size()) {
        size_t e = head.find("\r\n", i); if (e == std::string::npos) e = head.size();
        std::string l = head.substr(i, e - i); i = e + 2; size_t c = l.find(':'); if (c == std::string::npos) continue;
        r.h[lower(l.substr(0, c))] = trim(l.substr(c + 1));
    }
    if (r.h.count("content-length")) r.clen = atoll(r.h["content-length"].c_str());
    r.fd = fd; return true;
}

static std::string b64dec(const std::string &s) {
    std::string o; int val = 0, bits = -8;
    for (unsigned char c : s) {
        int d = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (d < 0) continue; val = (val << 6) + d; bits += 6; if (bits >= 0) { o += (char)((val >> bits) & 0xFF); bits -= 8; }
    }
    return o;
}

// body of a small POST (form-encoded or JSON-ish query) -- limited to 64 KiB
static bool read_body(Req &r, std::string &out) {
    out = r.body_prefix;
    if (r.clen > 65536) return false;
    while ((long long)out.size() < r.clen) {
        char t[4096]; ssize_t n = recv(r.fd, t, sizeof t, 0); if (n <= 0) return false; out.append(t, n);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------- helpers
static std::string vol_json(const Vol &v) {
    std::string o = "{";
    o += "\"id\":" + jstr(v.id) + ",\"kind\":" + jstr(v.kind) + ",\"name\":" + jstr(v.name) + ",\"model\":" + jstr(v.model) + ",\"dev\":" + jstr(v.dev) +
         ",\"label\":" + jstr(v.label) + ",\"fs\":" + jstr(v.fs) + ",\"mounted\":" + (v.mounted ? "true" : "false") + ",\"rw\":" + (v.rw ? "true" : "false") +
         ",\"media\":" + (v.hasmedia ? "true" : "false") + ",\"note\":" + jstr(v.note) +
         strf(",\"size\":%llu,\"total\":%llu,\"free\":%llu", v.size, v.total, v.freeb) + "}";
    return o;
}
static std::string volumes_json(const std::vector<Vol> &vs) {
    std::string o = "[";
    for (size_t i = 0; i < vs.size(); i++) o += (i ? "," : "") + vol_json(vs[i]);
    return o + "]";
}

static std::string job_new_id() {
    static int seq = 0; time_t t = time(nullptr) + CFG.tz * 3600; struct tm tm; gmtime_r(&t, &tm);
    char b[32]; strftime(b, sizeof b, "%Y%m%d-%H%M%S", &tm);
    return strf("%s-%02d", b, (seq++ + getpid()) % 100);
}

static void prune_jobs() {
    DIR *d = opendir(jobs_dir().c_str()); if (!d) return;
    std::set<std::string> ids; while (dirent *e = readdir(d)) { std::string n = e->d_name; size_t p = n.find('.'); if (p != std::string::npos && n[0] == '2') ids.insert(n.substr(0, p)); }
    closedir(d);
    if (ids.size() <= 40) return;
    size_t del = ids.size() - 40;
    for (auto &id : ids) { if (!del--) break; for (const char *ext : {".json", ".spec", ".log", ".cancel"}) unlink((jobs_dir() + "/" + id + ext).c_str()); }
}

static bool start_job(JobSpec spec, std::string &err) {
    std::vector<Vol> vols = scan_volumes(false);
    const Vol *dv = find_vol(vols, spec.dstVol);
    if (!dv) { err = "unknown destination volume"; return false; }
    if (!dv->mounted) { err = "destination is not mounted"; return false; }
    if (!dv->rw) { err = "destination is read-only"; return false; }
    if (spec.src.empty()) { err = "no source selected"; return false; }
    for (auto &s : spec.src) {
        const Vol *sv = find_vol(vols, s.first);
        if (!sv || !sv->mounted) { err = "source " + s.first + " is not available"; return false; }
        std::string c; if (!clean_rel(s.second, c)) { err = "bad source path"; return false; }
        s.second = c;
    }
    std::string c; if (!clean_rel(spec.dstRel, c)) { err = "bad destination path"; return false; }
    spec.dstRel = c;
    if (spec.verify != "none" && spec.verify != "fast" && spec.verify != "sha256") spec.verify = "fast";
    if (spec.policy != "smart" && spec.policy != "skip" && spec.policy != "overwrite") spec.policy = "smart";
    spec.id = job_new_id(); spec.tz = CFG.tz;
    mkdir_p(jobs_dir()); prune_jobs();
    write_atomic(jobs_dir() + "/" + spec.id + ".spec", spec_ser(spec));
    Progress p; p.spec = spec; p.started = now_ms() / 1000; p.dstDesc = dv->name + ":" + spec.dstRel;
    job_write(p, true);
    pid_t pid = fork();
    if (pid < 0) { err = "fork failed"; return false; }
    if (pid == 0) {
        setsid(); signal(SIGCHLD, SIG_DFL); signal(SIGPIPE, SIG_IGN);
        for (int fd = 3; fd < 256; fd++) close(fd);
        int nul = open("/dev/null", O_RDWR); dup2(nul, 0); dup2(nul, 1); dup2(nul, 2);
        JobSpec s2; if (!spec_load(jobs_dir() + "/" + spec.id + ".spec", s2)) _exit(2);
        Runner r; r.run(s2);
        _exit(0);
    }
    logmsg("job %s started: %zu source(s) -> %s%s verify=%s", spec.id.c_str(), spec.src.size(), spec.dstVol.c_str(), spec.dstRel.c_str(), spec.verify.c_str());
    err = spec.id; return true;
}

static std::string local_stamp() {
    time_t t = time(nullptr) + CFG.tz * 3600; struct tm tm; gmtime_r(&t, &tm);
    char b[32]; strftime(b, sizeof b, "%Y-%m-%d_%H%M", &tm); return b;
}
static std::string safe_name(const std::string &s) {
    std::string o; for (unsigned char c : s) o += (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') ? '_' : (char)c;
    return o;
}
static bool start_import(const std::string &srcId, const std::string &dstId, std::string &out) {
    std::vector<Vol> vols = scan_volumes(true);
    const Vol *sv = find_vol(vols, srcId); const Vol *dv = find_vol(vols, dstId);
    if (!sv) { out = "unknown source"; return false; }
    if (!dv) { out = "unknown destination"; return false; }
    JobSpec s; s.src.push_back({srcId, "/"}); s.dstVol = dstId;
    std::string nm = sv->name + (sv->label.empty() ? "" : " " + sv->label);
    s.dstRel = "/Imports/" + local_stamp() + " " + safe_name(nm);
    s.label = "Import " + nm; s.verify = CFG.verify; s.sort = CFG.sort; s.dedupe = CFG.dedupe;
    return start_job(s, out);
}

static bool rm_tree(const std::string &p, int depth = 0) {
    struct stat st; if (lstat(p.c_str(), &st) != 0) return true;
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        if (depth > 40) return false;
        DIR *d = opendir(p.c_str()); if (!d) return false;
        std::vector<std::string> n; while (dirent *e = readdir(d)) { std::string s = e->d_name; if (s != "." && s != "..") n.push_back(s); }
        closedir(d);
        for (auto &s : n) if (!rm_tree(p + "/" + s, depth + 1)) return false;
        return rmdir(p.c_str()) == 0;
    }
    return unlink(p.c_str()) == 0;
}

static std::string info_json() {
    std::string mem; read_file("/proc/meminfo", mem);
    auto mi = [&](const char *k) { size_t p = mem.find(k); return p == std::string::npos ? 0LL : atoll(mem.c_str() + p + strlen(k)); };
    long long fr = mi("MemFree:"), bu = mi("Buffers:"), ca = mi("Cached:");
    std::string la = read_small("/proc/loadavg");
    int running = 0; DIR *d = opendir(jobs_dir().c_str());
    while (d) { dirent *e = readdir(d); if (!e) break; std::string n = e->d_name; if (ends_with(n, ".json")) { std::string j; read_file(jobs_dir() + "/" + n, j, 65536); if (j.find("\"state\":\"copying\"") != std::string::npos || j.find("\"state\":\"scanning\"") != std::string::npos || j.find("\"state\":\"queued\"") != std::string::npos) running++; } }
    if (d) closedir(d);
    return strf("{\"version\":%s,\"uptime\":%lld,\"mem_free_kb\":%lld,\"mem_avail_kb\":%lld,\"load\":%s,\"running\":%d,\"autoimport\":%s,\"verify\":%s,\"sort\":%s,\"dedupe\":%s,\"auth\":%s,\"port\":%d,\"time\":%lld,\"tz\":%d}",
                jstr(VERSION).c_str(), (now_ms() - START_MS) / 1000, fr, fr + bu + ca, jstr(la).c_str(), running, CFG.autoimport ? "true" : "false", jstr(CFG.verify).c_str(),
                CFG.sort ? "true" : "false", CFG.dedupe ? "true" : "false", CFG.pin.empty() ? "false" : "true", CFG.port, (long long)time(nullptr), CFG.tz);
}

static std::string jobs_json(int limit) {
    std::vector<std::string> ids; DIR *d = opendir(jobs_dir().c_str());
    while (d) { dirent *e = readdir(d); if (!e) break; std::string n = e->d_name; if (ends_with(n, ".json") && n[0] == '2') ids.push_back(n.substr(0, n.size() - 5)); }
    if (d) closedir(d);
    std::sort(ids.rbegin(), ids.rend());
    std::string o = "["; int c = 0;
    for (auto &id : ids) { if (c >= limit) break; std::string j; if (!read_file(jobs_dir() + "/" + id + ".json", j, 1 << 18) || j.empty()) continue; o += (c ? "," : "") + j; c++; }
    return o + "]";
}

// ---------------------------------------------------------------------------------------------- handlers
static void h_download(Req &r, const Vol &v) {
    std::string abs, rel;
    if (!resolve(v, pget(r.q, "p"), abs, rel)) { jerr(r, 404, "not found"); return; }
    struct stat st; if (stat(abs.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) { jerr(r, 404, "not a file"); return; }
    int fd = open(abs.c_str(), O_RDONLY); if (fd < 0) { jerr(r, 404, "cannot open"); return; }
    long long size = st.st_size, a = 0, b = size - 1; int code = 200;
    if (r.h.count("range") && starts_with(r.h["range"], "bytes=")) {
        std::string rg = r.h["range"].substr(6); size_t dash = rg.find('-');
        if (dash != std::string::npos && rg.find(',') == std::string::npos) {
            std::string s1 = rg.substr(0, dash), s2 = rg.substr(dash + 1);
            if (s1.empty()) { long long n = atoll(s2.c_str()); a = std::max(0LL, size - n); }
            else { a = atoll(s1.c_str()); if (!s2.empty()) b = std::min(size - 1, atoll(s2.c_str())); }
            if (a > b || a >= size) { close(fd); respond(r, 416, "text/plain", "", strf("Content-Range: bytes */%lld\r\n", size)); return; }
            code = 206;
        }
    }
    std::string name = rel.substr(rel.rfind('/') + 1);
    bool inl = pget(r.q, "inline") == "1";
    std::string ct = ctype_of(name);
    if (inl && ct.find("text/html") == 0) ct = "text/plain; charset=utf-8";   // never render uploaded HTML in our origin
    std::string extra = strf("Accept-Ranges: bytes\r\nContent-Disposition: %s; filename*=UTF-8''%s\r\n", inl ? "inline" : "attachment", url_enc(name).c_str());
    if (code == 206) extra += strf("Content-Range: bytes %lld-%lld/%lld\r\n", a, b, size);
    std::string h = strf("HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lld\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n%s\r\n", code, code == 206 ? "Partial Content" : "OK", ct.c_str(), b - a + 1, extra.c_str());
    if (!send_all(r.fd, h.data(), h.size()) || r.method == "HEAD") { close(fd); return; }
    lseek(fd, a, SEEK_SET);
    static char buf[256 * 1024]; long long left = b - a + 1;
    while (left > 0) {
        ssize_t n = read(fd, buf, (size_t)std::min<long long>(left, sizeof buf)); if (n <= 0) break;
        if (!send_all(r.fd, buf, n)) break; left -= n;
    }
    close(fd);
}

static void h_ls(Req &r, const Vol &v) {
    std::string abs, rel;
    if (!resolve(v, pget(r.q, "p", "/"), abs, rel)) { jerr(r, 404, "not found"); return; }
    struct stat st; if (stat(abs.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) { jerr(r, 404, "not a directory"); return; }
    DIR *d = opendir(abs.c_str()); if (!d) { jerr(r, 403, strerror(errno)); return; }
    struct Ent { std::string n; bool d; unsigned long long s; long long m; };
    std::vector<Ent> es;
    while (dirent *e = readdir(d)) {
        std::string n = e->d_name; if (n == "." || n == ".." || hidden_name(n)) continue;
        struct stat s; if (lstat((abs + "/" + n).c_str(), &s) != 0 || S_ISLNK(s.st_mode)) continue;
        es.push_back({n, S_ISDIR(s.st_mode), (unsigned long long)s.st_size, (long long)s.st_mtime});
        if (es.size() >= 5000) break;
    }
    closedir(d);
    std::sort(es.begin(), es.end(), [](const Ent &a, const Ent &b) { if (a.d != b.d) return a.d > b.d; return lower(a.n) < lower(b.n); });
    std::string o = "{\"ok\":true,\"v\":" + jstr(v.id) + ",\"p\":" + jstr(rel) + ",\"entries\":[";
    for (size_t i = 0; i < es.size(); i++) o += strf("%s{\"n\":%s,\"d\":%d,\"s\":%llu,\"m\":%lld}", i ? "," : "", jstr(es[i].n).c_str(), es[i].d ? 1 : 0, es[i].s, es[i].m);
    struct statfs sf; unsigned long long tot = 0, fr = 0; if (statfs(abs.c_str(), &sf) == 0) { tot = (unsigned long long)sf.f_blocks * sf.f_bsize; fr = (unsigned long long)sf.f_bavail * sf.f_bsize; }
    o += strf("],\"total\":%llu,\"free\":%llu,\"truncated\":%s}", tot, fr, es.size() >= 5000 ? "true" : "false");
    jok(r, o);
}

static void h_put(Req &r, const Vol &v) {
    if (!v.rw) { jerr(r, 403, "volume is read-only"); return; }
    std::string abs, rel;
    std::string p = pget(r.q, "p");
    if (p.empty() || p.back() == '/') { jerr(r, 400, "file path required"); return; }
    std::string crel; if (!clean_rel(p, crel) || crel == "/") { jerr(r, 400, "bad path"); return; }
    std::string name = crel.substr(crel.rfind('/') + 1), dir = crel.substr(0, crel.rfind('/')); if (dir.empty()) dir = "/";
    if (hidden_name(name)) { jerr(r, 403, "reserved name"); return; }
    std::string dabs, drel;
    if (pget(r.q, "mk", "1") == "1") {       // create missing parent folders (folder uploads)
        std::string root = canon(v.mnt); std::string cur = "";
        std::string rest = dir; size_t i = 1;
        while (i <= rest.size()) {
            size_t j = rest.find('/', i); if (j == std::string::npos) j = rest.size();
            cur = rest.substr(0, j);
            std::string a, rr;
            if (!cur.empty() && cur != "/") {
                if (!resolve(v, cur, a, rr, true)) { if (!resolve(v, cur, a, rr, false)) { jerr(r, 403, "bad path"); return; } if (mkdir(a.c_str(), 0755) != 0 && errno != EEXIST) { jerr(r, 500, std::string("mkdir: ") + strerror(errno)); return; } }
            }
            i = j + 1;
        }
    }
    if (!resolve(v, dir, dabs, drel, true) || !is_dir(dabs)) { jerr(r, 404, "folder does not exist"); return; }
    if (r.clen < 0) { jerr(r, 400, "Content-Length required"); return; }
    struct statfs sf; if (statfs(dabs.c_str(), &sf) == 0) { unsigned long long fr = (unsigned long long)sf.f_bavail * sf.f_bsize; if ((unsigned long long)r.clen + (16ULL << 20) > fr) { jerr(r, 413, "not enough space on the volume"); return; } }
    std::string dest = dabs + "/" + name;
    bool over = pget(r.q, "overwrite") == "1";
    if (exists(dest) && !over) {
        size_t dot = name.rfind('.'); std::string base = name, ext; if (dot != std::string::npos && dot > 0) { base = name.substr(0, dot); ext = name.substr(dot); }
        for (int i = 1; i < 10000; i++) { std::string c = dabs + "/" + base + strf(" (%d)", i) + ext; if (!exists(c)) { dest = c; break; } }
    }
    std::string tmp = dabs + "/." + dest.substr(dest.rfind('/') + 1) + ".mstmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { jerr(r, 500, std::string("create: ") + strerror(errno)); return; }
    Xxh64 x; long long got = 0; bool ok = true; std::string err;
    auto wr = [&](const char *b, size_t n) { x.update(b, n); size_t off = 0; while (off < n) { ssize_t w = write(fd, b + off, n - off); if (w < 0) { if (errno == EINTR) continue; err = strerror(errno); return false; } off += w; } got += n; return true; };
    if (!r.body_prefix.empty()) { size_t n = std::min<long long>(r.body_prefix.size(), r.clen); ok = wr(r.body_prefix.data(), n); }
    static char buf[128 * 1024];
    while (ok && got < r.clen) {
        ssize_t n = recv(r.fd, buf, (size_t)std::min<long long>(sizeof buf, r.clen - got), 0);
        if (n <= 0) { ok = false; err = "connection lost during upload"; break; }
        ok = wr(buf, n);
    }
    if (ok) { if (fsync(fd) != 0) { ok = false; err = strerror(errno); } }
    close(fd);
    if (!ok) { unlink(tmp.c_str()); jerr(r, 500, err); return; }
    if (ms_rename(tmp.c_str(), dest.c_str()) != 0) { err = strerror(errno); unlink(tmp.c_str()); jerr(r, 500, err); return; }
    jok(r, strf("{\"ok\":true,\"name\":%s,\"size\":%lld,\"xxh64\":\"%s\"}", jstr(dest.substr(dest.rfind('/') + 1)).c_str(), got, x.hex().c_str()));
}

static void api(Req &r) {
    const std::string &P = r.path;
    if (P == "/api/info") { jok(r, info_json()); return; }
    if (P == "/api/storage") {
        std::vector<Vol> vs = scan_volumes(true);
        jok(r, "{\"ok\":true,\"volumes\":" + volumes_json(vs) + ",\"time\":" + strf("%lld", (long long)time(nullptr)) + "}"); return;
    }
    if (P == "/api/jobs") { jok(r, "{\"ok\":true,\"jobs\":" + jobs_json(atoi(pget(r.q, "n", "20").c_str())) + "}"); return; }
    if (P == "/api/job") {
        std::string id = pget(r.q, "id"); for (char c : id) if (!((c >= '0' && c <= '9') || c == '-')) { jerr(r, 400, "bad id"); return; }
        std::string j; if (!read_file(jobs_dir() + "/" + id + ".json", j, 1 << 18)) { jerr(r, 404, "no such job"); return; }
        if (pget(r.q, "log") == "1") { std::string l; read_file(jobs_dir() + "/" + id + ".log", l, 1 << 20); respond(r, 200, "text/plain; charset=utf-8", l); return; }
        jok(r, j); return;
    }
    // everything below needs POST/PUT
    bool write_op = P == "/api/copy" || P == "/api/import" || P == "/api/cancel" || P == "/api/put" || P == "/api/mkdir" || P == "/api/rm" || P == "/api/umount" || P == "/api/mount" || P == "/api/config";
    if (write_op && r.method != "POST" && r.method != "PUT") { jerr(r, 405, "use POST"); return; }
    if (write_op && !r.loopback && r.h["x-ms"] != "1") { jerr(r, 403, "missing X-MS header"); return; }
    std::string body;
    if (r.method == "POST" && P != "/api/put") { if (!read_body(r, body)) { jerr(r, 400, "bad body"); return; } Params bp = parse_params(body); r.q.insert(r.q.end(), bp.begin(), bp.end()); }
    if (P == "/api/cancel") {
        std::string id = pget(r.q, "id"); for (char c : id) if (!((c >= '0' && c <= '9') || c == '-')) { jerr(r, 400, "bad id"); return; }
        int fd = open((jobs_dir() + "/" + id + ".cancel").c_str(), O_WRONLY | O_CREAT, 0644); if (fd >= 0) close(fd);
        logmsg("cancel requested for %s", id.c_str()); jok(r, "{\"ok\":true}"); return;
    }
    if (P == "/api/config") {
        std::string a = pget(r.q, "autoimport"), vf = pget(r.q, "verify"), so = pget(r.q, "sort"), dd = pget(r.q, "dedupe");
        if (!a.empty()) CFG.autoimport = a == "1";
        if (vf == "none" || vf == "fast" || vf == "sha256") CFG.verify = vf;
        if (!so.empty()) CFG.sort = so == "1"; if (!dd.empty()) CFG.dedupe = dd == "1";
        write_atomic(std::string(STATE_DIR) + "/config", strf("autoimport=%d\nverify=%s\nsort=%d\ndedupe=%d\n", CFG.autoimport, CFG.verify.c_str(), CFG.sort, CFG.dedupe));
        jok(r, info_json()); return;
    }
    if (P == "/api/copy") {
        JobSpec s; s.dstVol = pget(r.q, "dst"); s.dstRel = pget(r.q, "dstp", "/");
        for (auto &sv : pgetall(r.q, "src")) { size_t c = sv.find(':'); if (c == std::string::npos) { jerr(r, 400, "src must be volume:/path"); return; } s.src.push_back({sv.substr(0, c), sv.substr(c + 1)}); }
        s.verify = pget(r.q, "verify", CFG.verify); s.policy = pget(r.q, "policy", "smart"); s.sort = pget(r.q, "sort", CFG.sort ? "1" : "0") == "1"; s.dedupe = pget(r.q, "dedupe", CFG.dedupe ? "1" : "0") == "1";
        s.label = pget(r.q, "label", "Copy");
        std::string out; if (!start_job(s, out)) { jerr(r, 400, out); return; }
        jok(r, "{\"ok\":true,\"id\":" + jstr(out) + "}"); return;
    }
    if (P == "/api/import") {
        std::string out; if (!start_import(pget(r.q, "src"), pget(r.q, "dst"), out)) { jerr(r, 400, out); return; }
        jok(r, "{\"ok\":true,\"id\":" + jstr(out) + "}"); return;
    }
    if (P == "/api/umount" || P == "/api/mount") {
        std::vector<Vol> vs = scan_volumes(true);
        if (P == "/api/mount") {
            const Vol *v = find_vol(vs, pget(r.q, "v")); if (!v) { jerr(r, 404, "unknown volume"); return; }
            if (v->mounted) { jok(r, "{\"ok\":true}"); return; }
            if (v->fs.empty()) { jerr(r, 400, "no recognised filesystem"); return; }
            std::string err; if (!do_mount(v->dev, v->fs, std::string(MNT_BASE) + "/" + v->dev, err)) { jerr(r, 500, err); return; }
            jok(r, "{\"ok\":true}"); return;
        }
        const Vol *v = find_vol(vs, pget(r.q, "v")); if (!v || !v->mounted) { jerr(r, 404, "volume is not mounted"); return; }
        if (v->kind == "HDD" && pget(r.q, "force") != "1") { jerr(r, 403, "refusing to unmount the HDD without force=1"); return; }
        sync();
        if (umount(v->mnt.c_str()) != 0) { jerr(r, 409, std::string("busy: ") + strerror(errno)); return; }
        logmsg("unmounted %s", v->mnt.c_str()); jok(r, "{\"ok\":true,\"safe_to_remove\":true}"); return;
    }
    // volume-scoped file operations
    std::vector<Vol> vs = scan_volumes(false);
    const Vol *v = find_vol(vs, pget(r.q, "v"));
    if (!v) { jerr(r, 404, "unknown volume"); return; }
    if (!v->mounted) { jerr(r, 409, "volume is not mounted"); return; }
    if (P == "/api/ls") { h_ls(r, *v); return; }
    if (P == "/api/put") { h_put(r, *v); return; }
    if (P == "/api/mkdir") {
        if (!v->rw) { jerr(r, 403, "volume is read-only"); return; }
        std::string abs, rel; if (!resolve(*v, pget(r.q, "p"), abs, rel, false) || rel == "/") { jerr(r, 400, "bad path"); return; }
        if (hidden_name(rel.substr(rel.rfind('/') + 1))) { jerr(r, 403, "reserved name"); return; }
        if (mkdir(abs.c_str(), 0755) != 0 && errno != EEXIST) { jerr(r, 500, strerror(errno)); return; }
        jok(r, "{\"ok\":true}"); return;
    }
    if (P == "/api/rm") {
        if (!v->rw) { jerr(r, 403, "volume is read-only"); return; }
        if (pget(r.q, "confirm") != "yes") { jerr(r, 400, "confirm=yes required"); return; }
        std::string abs, rel; if (!resolve(*v, pget(r.q, "p"), abs, rel, true) || rel == "/") { jerr(r, 400, "refusing to delete the volume root"); return; }
        if (!rm_tree(abs)) { jerr(r, 500, strerror(errno)); return; }
        logmsg("deleted %s:%s", v->id.c_str(), rel.c_str()); sync(); jok(r, "{\"ok\":true}"); return;
    }
    jerr(r, 404, "unknown API");
}


// ---------------------------------------------------------------------------------------------- TV (Premium Apps RSS) pages
static std::string lan_ip() {
    int s = socket(AF_INET, SOCK_DGRAM, 0); if (s < 0) return "0.0.0.0";
    struct ifreq ifr; memset(&ifr, 0, sizeof ifr); strncpy(ifr.ifr_name, "eth0", IFNAMSIZ - 1);
    std::string ip = "0.0.0.0";
    if (ioctl(s, 0x8915 /*SIOCGIFADDR*/, &ifr) == 0) ip = inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr);
    close(s); return ip;
}
static double jnum(const std::string &j, const char *k) { std::string key = std::string("\"") + k + "\":"; size_t p = j.find(key); return p == std::string::npos ? 0 : atof(j.c_str() + p + key.size()); }
static std::string jtxt(const std::string &j, const char *k) {
    std::string key = std::string("\"") + k + "\":\""; size_t p = j.find(key); if (p == std::string::npos) return ""; p += key.size();
    std::string o; for (; p < j.size() && j[p] != '"'; p++) { if (j[p] == '\\' && p + 1 < j.size()) { p++; o += j[p] == 'n' ? ' ' : j[p]; } else o += j[p]; }
    return o;
}
static std::string rss_head(const std::string &title) {
    return "<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<rss version=\"2.0\" xmlns:media=\"http://search.yahoo.com/mrss/\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n<channel>\n<title>" + xml_esc(title) + "</title>\n";
}
static std::string rss_item(const std::string &title, const std::string &desc, const std::string &extra = "") {
    return "<item><title>" + xml_esc(title) + "</title><description>" + xml_esc(desc) + "</description><pubDate>Mon, 12 Oct 2026 00:00:00 GMT</pubDate>" + extra + "</item>\n";
}
static std::string short_dur(double s) { if (s < 0) return "--"; int t = (int)s; return t >= 3600 ? strf("%dh%02dm", t / 3600, (t / 60) % 60) : t >= 60 ? strf("%dm%02ds", t / 60, t % 60) : strf("%ds", t); }
static void tv_note(const std::string &msg) { write_atomic(std::string(STATE_DIR) + "/tv_last.txt", strf("%lld\n", now_ms() / 1000) + msg); }

static std::string tv_do(const std::string &c) {
    std::vector<Vol> vs = scan_volumes(true);
    auto first = [&](const char *kind) -> const Vol * { for (auto &v : vs) if (v.kind == kind && (v.mounted || std::string(kind) == "DVD")) return &v; return nullptr; };
    const Vol *hdd = nullptr; for (auto &v : vs) if (v.kind == "HDD" && v.mounted && v.rw) { hdd = &v; break; }
    if (c == "import_sd" || c == "import_usb" || c == "import_dvd") {
        const Vol *src = first(c == "import_sd" ? "SD" : c == "import_usb" ? "USB" : "DVD");
        if (!src) return "No such storage is connected.";
        if (!hdd) return "No writable SATA HDD is available.";
        if (!src->mounted) return src->kind == "DVD" ? "No readable data disc in the DVD drive." : "Storage is not mounted.";
        std::string out; if (!start_import(src->id, hdd->id, out)) return "Import failed: " + out;
        return "Import started: " + src->name + " -> " + hdd->name;
    }
    if (c == "cancel") {
        int n = 0; DIR *d = opendir(jobs_dir().c_str());
        while (d) { dirent *e = readdir(d); if (!e) break; std::string nme = e->d_name; if (ends_with(nme, ".json") && nme[0] == '2') {
            std::string j; read_file(jobs_dir() + "/" + nme, j, 1 << 16); std::string st = jtxt(j, "state");
            if (st == "copying" || st == "scanning" || st == "queued") { int fd = open((jobs_dir() + "/" + nme.substr(0, nme.size() - 5) + ".cancel").c_str(), O_WRONLY | O_CREAT, 0644); if (fd >= 0) close(fd); n++; } } }
        if (d) closedir(d); return n ? "Cancel requested." : "Nothing is running.";
    }
    if (c == "auto_toggle") { CFG.autoimport = !CFG.autoimport; write_atomic(std::string(STATE_DIR) + "/config", strf("autoimport=%d\nverify=%s\nsort=%d\ndedupe=%d\n", CFG.autoimport, CFG.verify.c_str(), CFG.sort, CFG.dedupe)); return std::string("Auto-import is now ") + (CFG.autoimport ? "ON: re-insert a card to import it." : "OFF."); }
    if (c == "verify_next") { CFG.verify = CFG.verify == "fast" ? "sha256" : CFG.verify == "sha256" ? "none" : "fast"; write_atomic(std::string(STATE_DIR) + "/config", strf("autoimport=%d\nverify=%s\nsort=%d\ndedupe=%d\n", CFG.autoimport, CFG.verify.c_str(), CFG.sort, CFG.dedupe)); return "Verification mode: " + CFG.verify; }
    if (c == "sort_toggle") { CFG.sort = !CFG.sort; write_atomic(std::string(STATE_DIR) + "/config", strf("autoimport=%d\nverify=%s\nsort=%d\ndedupe=%d\n", CFG.autoimport, CFG.verify.c_str(), CFG.sort, CFG.dedupe)); return std::string("Sort by file type: ") + (CFG.sort ? "ON" : "OFF"); }
    return "Unknown command.";
}

static void walk_recent(const std::string &dir, const std::string &rel, std::vector<std::pair<long long, std::string>> &out, int depth, int &budget) {
    if (depth > 8 || budget <= 0) return;
    DIR *d = opendir(dir.c_str()); if (!d) return;
    std::vector<std::string> names; while (dirent *e = readdir(d)) { std::string n = e->d_name; if (n != "." && n != "..") names.push_back(n); }
    closedir(d);
    for (auto &n : names) {
        if (hidden_name(n) || --budget <= 0) continue;
        struct stat st; std::string a = dir + "/" + n; if (lstat(a.c_str(), &st) != 0 || S_ISLNK(st.st_mode)) continue;
        if (S_ISDIR(st.st_mode)) walk_recent(a, rel + "/" + n, out, depth + 1, budget);
        else { std::string e = lower(n.substr(n.rfind('.') == std::string::npos ? n.size() : n.rfind('.') + 1)); std::string ct = ctype_of(n);
               if (ct.find("video/") == 0 || ct.find("audio/") == 0) out.push_back({(long long)st.st_mtime, rel + "/" + n}); }
    }
}

static void tv_handle(Req &r) {
    const std::string &P = r.path;
    if (P == "/tv/do") {
        if (!r.loopback) { respond(r, 403, "text/plain", "TV commands are local only\n"); return; }
        std::string res = tv_do(pget(r.q, "c")); tv_note(res); logmsg("tv command %s -> %s", pget(r.q, "c").c_str(), res.c_str());
        respond(r, 200, "text/plain; charset=utf-8", res + "\n"); return;
    }
    std::string o;
    if (P == "/tv/storage.rss") {
        std::vector<Vol> vs = scan_volumes(true);
        o = rss_head("Storage");
        for (auto &v : vs) {
            std::string t = v.name + (v.label.empty() ? "" : " \"" + v.label + "\"") + "   " + (v.hasmedia ? human((double)(v.size ? v.size : v.total)) : "--");
            std::string d = !v.hasmedia ? "No disc in the drive." : v.mounted ? strf("%s   Free %s   %s", v.fs.c_str(), human((double)v.freeb).c_str(), v.rw ? "read/write" : "read-only") : (v.note.empty() ? "not mounted" : v.note);
            d += "\n" + v.model + "  (" + v.dev + ")";
            o += rss_item(t, d);
        }
        if (vs.empty()) o += rss_item("No storage detected", "");
        respond(r, 200, "application/rss+xml; charset=utf-8", o + "</channel>\n</rss>\n"); return;
    }
    if (P == "/tv/progress.rss") {
        o = rss_head("Transfers");
        std::string note = read_small(std::string(STATE_DIR) + "/tv_last.txt");
        if (!note.empty()) { size_t nl = note.find('\n'); long long t = atoll(note.c_str()); if (nl != std::string::npos && time(nullptr) - t < 120) o += rss_item(trim(note.substr(nl + 1)), "Last command"); }
        std::string js = jobs_json(3); int shown = 0; size_t i = 0;
        while (i < js.size() && shown < 3) {
            size_t b = js.find("{\"id\"", i); if (b == std::string::npos) break;
            size_t e = js.find("{\"id\"", b + 5); std::string j = js.substr(b, e == std::string::npos ? std::string::npos : e - b); i = b + 5; shown++;
            std::string st = jtxt(j, "state"), ph = jtxt(j, "phase"), verify = jtxt(j, "verify");
            double bt = jnum(j, "bytes_total"), bd = jnum(j, "bytes_done"), vd = jnum(j, "vbytes_done"), bs = jnum(j, "bytes_skipped"), sp = jnum(j, "speed"), eta = jnum(j, "eta");
            double tot = bt * (verify == "none" ? 1 : 2), done = bd + vd + bs * (verify == "none" ? 1 : 2);
            int pct = (st == "done" || st == "done_with_errors") ? 100 : tot > 0 ? (int)(100.0 * done / tot) : 0; if (pct > 100) pct = 100;
            std::string head = st == "copying" ? (ph == "verify" ? "VERIFYING " : "COPYING ") + strf("%d%%", pct) : st == "done" ? (verify == "none" ? "DONE (not verified)" : "DONE - all verified") : st == "done_with_errors" ? "DONE WITH ERRORS" : st == "error" ? "ERROR" : st == "cancelled" ? "CANCELLED" : st == "queued" ? "QUEUED" : "SCANNING";
            std::string bar = "["; for (int k = 0; k < 20; k++) bar += k * 5 < pct ? "#" : "-"; bar += "]";
            std::string t = head + "   " + jtxt(j, "src") + " > " + jtxt(j, "dst");
            std::string d = bar + strf(" %d%%\n", pct) + strf("Files %d/%d (copied %d, skipped %d, dup %d)  %s / %s", (int)jnum(j, "files_done"), (int)jnum(j, "files_total"), (int)jnum(j, "copied"), (int)jnum(j, "skipped"), (int)jnum(j, "dups"), human(bd).c_str(), human(bt).c_str());
            if (st == "copying") d += strf("\n%s/s  ETA %s   Now: %s", human(sp).c_str(), short_dur(eta).c_str(), jtxt(j, "cur").c_str());
            else d += strf("\nVerified %d   Errors %d", (int)jnum(j, "verified"), (int)jnum(j, "errors_n"));
            o += rss_item(t, d);
        }
        if (!shown) o += rss_item("No transfers yet", "Choose IMPORT in the left menu.");
        o += rss_item("(select PROGRESS again to refresh)", "");
        respond(r, 200, "application/rss+xml; charset=utf-8", o + "</channel>\n</rss>\n"); return;
    }
    if (P == "/tv/network.rss") {
        std::string ip = lan_ip();
        o = rss_head("Network access");
        o += rss_item("Open on your PC:  http://" + ip + ":" + strf("%d", CFG.port) + "/", "Browse every storage, upload / download files, start copies. Works in any web browser on the same LAN.");
        o += rss_item(std::string("Access: LAN only") + (CFG.pin.empty() ? " (no PIN)" : " (PIN protected)"), "Only private addresses (192.168.x.x, 10.x.x.x, 172.16-31.x.x) are accepted.");
        o += rss_item(std::string("Auto-import: ") + (CFG.autoimport ? "ON" : "OFF") + "    Verify: " + CFG.verify + "    Sort: " + (CFG.sort ? "ON" : "OFF"), "Change with the AUTO-IMPORT / VERIFY / SORT entries in the left menu.");
        respond(r, 200, "application/rss+xml; charset=utf-8", o + "</channel>\n</rss>\n"); return;
    }
    if (P == "/tv/recent.rss") {
        std::vector<Vol> vs = scan_volumes(false); o = rss_head("Recent media on HDD");
        std::vector<std::pair<long long, std::string>> files; std::string vid;
        for (auto &v : vs) if (v.kind == "HDD" && v.mounted) { vid = v.id; int budget = 20000; walk_recent(v.mnt, "", files, 0, budget); break; }
        std::sort(files.rbegin(), files.rend());
        int n = 0;
        for (auto &f : files) {
            if (n++ >= 40) break;
            std::string name = f.second.substr(f.second.rfind('/') + 1), dir = f.second.substr(0, f.second.rfind('/'));
            std::string url = "http://127.0.0.1:" + strf("%d", CFG.port) + "/dl?v=" + vid + "&amp;p=" + xml_esc(url_enc(f.second)) + "&amp;inline=1";
            std::string ct = ctype_of(name); if (ct == "video/x-matroska" || ct == "video/x-msvideo") {}
            o += rss_item(name, "HDD " + dir, "<enclosure url=\"" + url + "\" type=\"" + ct + "\" length=\"0\" />");
        }
        if (!n) o += rss_item("No media files on the HDD yet", "Import something first.");
        respond(r, 200, "application/rss+xml; charset=utf-8", o + "</channel>\n</rss>\n"); return;
    }
    respond(r, 404, "text/plain", "not found\n");
}

static void load_config();
static void serve_conn(int fd, const std::string &peer, uint32_t ip) {
    struct timeval tv = {20, 0}; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    struct timeval tv2 = {60, 0}; setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv2, sizeof tv2);
    load_config();      // settings live in /tmp/ms/config (children and the monitor loop are separate processes)
    Req r; r.peer = peer; r.loopback = (ip >> 24) == 127;
    if (!read_request(fd, r)) return;
    if (!private_ip(ip)) { jerr(r, 403, "LAN clients only"); return; }
    if (!r.loopback && !CFG.pin.empty()) {
        std::string a = r.h["authorization"]; bool ok = false;
        if (starts_with(a, "Basic ")) { std::string cred = b64dec(a.substr(6)); size_t c = cred.find(':'); ok = c != std::string::npos && cred.substr(c + 1) == CFG.pin; }
        if (!ok) { respond(r, 401, "text/plain", "PIN required (any user name, password = PIN)\n", "WWW-Authenticate: Basic realm=\"HD900B Media Station\"\r\n"); return; }
    }
    if (r.method != "GET" && r.method != "HEAD" && r.method != "POST" && r.method != "PUT") { respond(r, 405, "text/plain", "method not allowed\n"); return; }
    if (r.path == "/" || r.path == "/index.html") { respond(r, 200, "text/html; charset=utf-8", std::string(UI_HTML)); return; }
    if (r.path == "/health") { respond(r, 200, "text/plain", "ok\n"); return; }
    if (r.path == "/dl") {
        std::vector<Vol> vs = scan_volumes(false); const Vol *v = find_vol(vs, pget(r.q, "v"));
        if (!v || !v->mounted) { jerr(r, 404, "unknown or unmounted volume"); return; }
        h_download(r, *v); return;
    }
    if (starts_with(r.path, "/api/")) { api(r); return; }
    if (starts_with(r.path, "/tv/")) { tv_handle(r); return; }
    respond(r, 404, "text/plain", "not found\n");
}

// ---------------------------------------------------------------------------------------------- main
static volatile sig_atomic_t g_chld = 0;
static void on_chld(int) { g_chld = 1; }

static void load_config() {
    std::string s; if (!read_file(std::string(STATE_DIR) + "/config", s)) return;
    size_t i = 0;
    while (i < s.size()) { size_t e = s.find('\n', i); if (e == std::string::npos) e = s.size(); std::string l = s.substr(i, e - i); i = e + 1; size_t q = l.find('='); if (q == std::string::npos) continue;
        std::string k = l.substr(0, q), v = l.substr(q + 1);
        if (k == "autoimport") CFG.autoimport = v == "1"; else if (k == "verify") CFG.verify = v; else if (k == "sort") CFG.sort = v == "1"; else if (k == "dedupe") CFG.dedupe = v == "1"; }
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-p" && i + 1 < argc) CFG.port = atoi(argv[++i]);
        else if (a == "-b" && i + 1 < argc) CFG.bind = argv[++i];
        else if (a == "-P" && i + 1 < argc) CFG.pin = argv[++i];
        else if (a == "-z" && i + 1 < argc) CFG.tz = atoi(argv[++i]);
        else if (a == "-A") CFG.autoimport = true;
        else if (a == "-V") { printf("%s\n", VERSION); return 0; }
        else if (a == "--scan") { for (auto &v : scan_volumes(true)) printf("%s\n", vol_json(v).c_str()); return 0; }
        else { fprintf(stderr, "usage: hdms [-p port] [-b addr] [-P pin] [-z tzhours] [-A autoimport] [-V] [--scan]\n"); return 2; }
    }
    umask(022);
    mkdir_p(STATE_DIR); mkdir_p(jobs_dir());
    load_config();
    START_MS = now_ms();
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_chld; sa.sa_flags = SA_RESTART | SA_NOCLDSTOP; sigaction(SIGCHLD, &sa, nullptr);
    int ls = socket(AF_INET, SOCK_STREAM, 0); int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sin; memset(&sin, 0, sizeof sin); sin.sin_family = AF_INET; sin.sin_port = htons(CFG.port); sin.sin_addr.s_addr = inet_addr(CFG.bind.c_str());
    if (bind(ls, (struct sockaddr *)&sin, sizeof sin) != 0) { perror("bind"); return 1; }
    listen(ls, 32);
    logmsg("%s listening on %s:%d (pin %s, autoimport %d)", VERSION, CFG.bind.c_str(), CFG.port, CFG.pin.empty() ? "off" : "on", CFG.autoimport);
    std::map<std::string, MountState> mst;
    std::set<std::string> seen_mounted; bool first = true; long long next_mon = 0;
    for (;;) {
        if (g_chld) { g_chld = 0; while (waitpid(-1, nullptr, WNOHANG) > 0) { if (NCHILD > 0) NCHILD--; } }
        struct pollfd pf = {ls, POLLIN, 0};
        int pr = poll(&pf, 1, 500);
        if (pr > 0 && (pf.revents & POLLIN)) {
            struct sockaddr_in ca; socklen_t cl = sizeof ca; int c = accept(ls, (struct sockaddr *)&ca, &cl);
            if (c >= 0) {
                if (NCHILD >= 24) { const char *m = "HTTP/1.1 503 Busy\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"; send(c, m, strlen(m), MSG_NOSIGNAL); close(c); }
                else {
                    pid_t p = fork();
                    if (p == 0) { close(ls); signal(SIGCHLD, SIG_DFL); serve_conn(c, inet_ntoa(ca.sin_addr), ntohl(ca.sin_addr.s_addr)); shutdown(c, SHUT_WR); close(c); _exit(0); }
                    if (p > 0) NCHILD++;
                    close(c);
                }
            }
        }
        long long t = now_ms();
        if (t >= next_mon) {
            next_mon = t + 3000; load_config();
            std::vector<std::string> lg; automount(mst, lg); std::set<std::string> fresh;
            for (auto &l : lg) { logmsg("%s", l.c_str()); if (starts_with(l, "mounted ")) { size_t e = l.find(' ', 8); fresh.insert(l.substr(8, e - 8)); } }
            std::vector<Vol> vs = scan_volumes(false);
            std::set<std::string> now_m; const Vol *hdd = nullptr;
            for (auto &v : vs) if (v.mounted) { now_m.insert(v.dev); if (v.kind == "HDD" && v.rw && !hdd) hdd = &v; }
            if (!first && CFG.autoimport && hdd) {
                for (auto &v : vs) if (v.mounted && v.kind != "HDD" && (!seen_mounted.count(v.dev) || fresh.count(v.dev))) {
                    std::string out; if (start_import(v.id, hdd->id, out)) logmsg("auto-import of newly inserted %s started: %s", v.dev.c_str(), out.c_str());
                    else logmsg("auto-import of %s failed: %s", v.dev.c_str(), out.c_str());
                }
            }
            seen_mounted = now_m; first = false;
        }
    }
}
