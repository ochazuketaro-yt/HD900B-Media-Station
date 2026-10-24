// SPDX-License-Identifier: GPL-3.0-or-later
// hdms copy engine: scan, space check, copy with inline hashing, read-back verification, dedupe, progress file.
// Each job runs in its own process (fork) so a crash or a pulled card never takes the web server down.
#pragma once
#include "storage.h"
#include <sys/file.h>
#include <sys/wait.h>
#include <signal.h>
#include <utime.h>

namespace ms {

static const char *STATE_DIR = "/tmp/ms";
inline std::string jobs_dir() { return std::string(STATE_DIR) + "/jobs"; }

struct JobSpec {
    std::string id, label;
    std::vector<std::pair<std::string, std::string>> src;   // (volume id, relative path)
    std::string dstVol, dstRel = "/";
    std::string verify = "fast";      // none | fast (XXH64 read-back) | sha256
    std::string policy = "smart";     // smart (skip identical, rename on conflict) | skip | overwrite
    bool sort = false;                // put files in Photos/Videos/Music/... sub folders
    bool dedupe = true;               // skip content that already exists in the destination library
    bool manifest = true;
    int tz = 9;                       // hours east of UTC for folder names
};

inline std::string spec_ser(const JobSpec &j) {
    std::string o = "id=" + url_enc(j.id) + "\nlabel=" + url_enc(j.label) + "\ndst=" + url_enc(j.dstVol) + "\ndstrel=" + url_enc(j.dstRel) +
                    "\nverify=" + j.verify + "\npolicy=" + j.policy + "\nsort=" + (j.sort ? "1" : "0") + "\ndedupe=" + (j.dedupe ? "1" : "0") +
                    "\nmanifest=" + (j.manifest ? "1" : "0") + strf("\ntz=%d\n", j.tz);
    for (auto &s : j.src) o += "src=" + url_enc(s.first) + ":" + url_enc(s.second) + "\n";
    return o;
}
inline bool spec_load(const std::string &path, JobSpec &j) {
    std::string s; if (!read_file(path, s)) return false;
    size_t i = 0;
    while (i < s.size()) {
        size_t e = s.find('\n', i); if (e == std::string::npos) e = s.size();
        std::string l = s.substr(i, e - i); i = e + 1; size_t q = l.find('='); if (q == std::string::npos) continue;
        std::string k = l.substr(0, q), v = l.substr(q + 1);
        if (k == "id") j.id = url_decode(v, false); else if (k == "label") j.label = url_decode(v, false);
        else if (k == "dst") j.dstVol = url_decode(v, false); else if (k == "dstrel") j.dstRel = url_decode(v, false);
        else if (k == "verify") j.verify = v; else if (k == "policy") j.policy = v;
        else if (k == "sort") j.sort = v == "1"; else if (k == "dedupe") j.dedupe = v == "1"; else if (k == "manifest") j.manifest = v == "1";
        else if (k == "tz") j.tz = atoi(v.c_str());
        else if (k == "src") { size_t c = v.find(':'); if (c != std::string::npos) j.src.push_back({url_decode(v.substr(0, c), false), url_decode(v.substr(c + 1), false)}); }
    }
    return !j.id.empty() && !j.dstVol.empty();
}

inline std::string category_of(const std::string &name) {
    size_t d = name.rfind('.'); if (d == std::string::npos) return "Other";
    std::string e = lower(name.substr(d + 1));
    static const char *photo[] = {"jpg","jpeg","png","gif","bmp","heic","heif","tif","tiff","raw","cr2","cr3","nef","arw","dng","orf","rw2","webp",nullptr};
    static const char *video[] = {"mp4","mkv","avi","mov","mpg","mpeg","ts","m2ts","mts","vob","wmv","flv","webm","3gp","m4v","divx","iso",nullptr};
    static const char *music[] = {"mp3","flac","wav","aac","m4a","ogg","wma","ape","opus","mid",nullptr};
    static const char *docs[] = {"pdf","doc","docx","xls","xlsx","ppt","pptx","txt","odt","ods","rtf","csv","md","epub",nullptr};
    static const char *arch[] = {"zip","rar","7z","tar","gz","bz2","xz","tgz",nullptr};
    auto in = [&](const char **t) { for (; *t; t++) if (e == *t) return true; return false; };
    if (in(photo)) return "Photos"; if (in(video)) return "Videos"; if (in(music)) return "Music"; if (in(docs)) return "Documents"; if (in(arch)) return "Archives";
    return "Other";
}

struct FileEnt { std::string abs, rel, srcvol; unsigned long long size = 0; time_t mtime = 0; mode_t mode = 0644; bool isdir = false; };

struct Progress {
    JobSpec spec;
    std::string state = "queued", phase, cur, srcDesc;
    long long started = 0, updated = 0, finished = 0, last_write = 0;
    unsigned long long files_total = 0, bytes_total = 0, files_done = 0, bytes_done = 0, vbytes_done = 0, cur_done = 0, cur_size = 0;
    unsigned long long n_copied = 0, n_skipped = 0, n_dup = 0, n_renamed = 0, n_verified = 0, n_errors = 0, bytes_skipped = 0;
    double speed = 0, copy_speed = 0, eta = -1;
    std::vector<std::string> errors, recent;
    std::string dstDesc, note;
    // speed estimation
    long long w_t = 0; unsigned long long w_b = 0; double sp_ema = 0;
};

inline std::string progress_json(const Progress &p) {
    std::string o = "{";
    o += "\"id\":" + jstr(p.spec.id) + ",\"label\":" + jstr(p.spec.label) + ",\"state\":" + jstr(p.state) + ",\"phase\":" + jstr(p.phase);
    o += ",\"src\":" + jstr(p.srcDesc) + ",\"dst\":" + jstr(p.dstDesc) + ",\"verify\":" + jstr(p.spec.verify) + ",\"sort\":" + (p.spec.sort ? "true" : "false") + ",\"dedupe\":" + (p.spec.dedupe ? "true" : "false");
    o += strf(",\"started\":%lld,\"updated\":%lld,\"finished\":%lld", p.started, p.updated, p.finished);
    o += strf(",\"files_total\":%llu,\"bytes_total\":%llu,\"files_done\":%llu,\"bytes_done\":%llu,\"vbytes_done\":%llu", p.files_total, p.bytes_total, p.files_done, p.bytes_done, p.vbytes_done);
    o += strf(",\"cur\":%s,\"cur_done\":%llu,\"cur_size\":%llu", jstr(p.cur).c_str(), p.cur_done, p.cur_size);
    o += strf(",\"copied\":%llu,\"skipped\":%llu,\"dups\":%llu,\"renamed\":%llu,\"verified\":%llu,\"errors_n\":%llu,\"bytes_skipped\":%llu", p.n_copied, p.n_skipped, p.n_dup, p.n_renamed, p.n_verified, p.n_errors, p.bytes_skipped);
    o += strf(",\"speed\":%.0f,\"eta\":%.0f", p.speed, p.eta);
    o += ",\"note\":" + jstr(p.note) + ",\"errors\":[";
    for (size_t i = 0; i < p.errors.size(); i++) o += (i ? "," : "") + jstr(p.errors[i]);
    o += "],\"recent\":[";
    for (size_t i = 0; i < p.recent.size(); i++) o += (i ? "," : "") + jstr(p.recent[i]);
    o += "]}";
    return o;
}

inline void job_write(Progress &p, bool force = false) {
    long long t = now_ms();
    if (!force && t - p.last_write < 250) return;
    p.last_write = t; p.updated = t / 1000;
    write_atomic(jobs_dir() + "/" + p.spec.id + ".json", progress_json(p));
}
inline void job_log(Progress &p, const std::string &line) {
    int fd = open((jobs_dir() + "/" + p.spec.id + ".log").c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { std::string l = line + "\n"; if (write(fd, l.data(), l.size()) < 0) {} close(fd); }
    p.recent.push_back(line); if (p.recent.size() > 8) p.recent.erase(p.recent.begin());
}
inline void job_error(Progress &p, const std::string &msg) {
    p.n_errors++; if (p.errors.size() < 40) p.errors.push_back(msg);
    job_log(p, "ERROR " + msg);
}

inline bool cancelled(const Progress &p) { return access((jobs_dir() + "/" + p.spec.id + ".cancel").c_str(), F_OK) == 0; }

struct Idx { unsigned long long size; std::string hash, path; };

struct Runner {
    Progress P;
    Vol dst; std::string dstRoot;              // absolute destination directory
    std::vector<Vol> vols;
    std::unordered_map<unsigned long long, std::vector<Idx>> index;   // size -> entries
    std::string indexPath;
    std::vector<std::string> newIndexLines;
    std::vector<std::pair<std::string, std::string>> manifest;        // (hash, path relative to dstRoot)
    unsigned char *buf = nullptr; static const size_t BUFSZ = 1 << 20;
    bool dstCaseInsens = false;

    void speed_tick(unsigned long long processed) {
        long long t = now_ms();
        if (P.w_t == 0) { P.w_t = t; P.w_b = processed; return; }
        if (t - P.w_t >= 1000) {
            double inst = (double)(processed - P.w_b) * 1000.0 / (double)(t - P.w_t);
            P.sp_ema = P.sp_ema == 0 ? inst : P.sp_ema * 0.6 + inst * 0.4;
            P.speed = P.sp_ema; P.w_t = t; P.w_b = processed;
            unsigned long long total = P.bytes_total * (P.spec.verify == "none" ? 1 : 2);
            unsigned long long done = P.bytes_done + P.vbytes_done + P.bytes_skipped * (P.spec.verify == "none" ? 1 : 2);
            P.eta = (P.speed > 1 && total > done) ? (double)(total - done) / P.speed : -1;
        }
    }

    // ---------------------------------------------------------------- scan
    void walk(const std::string &absdir, const std::string &rel, const std::string &volid, std::vector<FileEnt> &out, int depth) {
        if (depth > 40) { job_error(P, "too deep: " + rel); return; }
        DIR *d = opendir(absdir.c_str());
        if (!d) { job_error(P, "cannot open " + rel + ": " + strerror(errno)); return; }
        std::vector<std::string> names;
        while (dirent *e = readdir(d)) { std::string n = e->d_name; if (n != "." && n != "..") names.push_back(n); }
        closedir(d);
        std::sort(names.begin(), names.end());
        for (auto &n : names) {
            if (hidden_name(n)) continue;
            std::string a = absdir + "/" + n, r = rel.empty() ? n : rel + "/" + n;
            struct stat st; if (lstat(a.c_str(), &st) != 0) continue;
            if (S_ISLNK(st.st_mode)) continue;
            FileEnt f; f.abs = a; f.rel = r; f.srcvol = volid; f.size = st.st_size; f.mtime = st.st_mtime; f.mode = st.st_mode & 0777;
            if (S_ISDIR(st.st_mode)) { f.isdir = true; out.push_back(f); walk(a, r, volid, out, depth + 1); }
            else if (S_ISREG(st.st_mode)) out.push_back(f);
            if (out.size() > 300000) return;
        }
    }

    // ---------------------------------------------------------------- hashing helpers
    struct H { Xxh64 x; Sha256 s; bool use_sha; H(bool sha) : use_sha(sha) {} void upd(const void *p, size_t n) { x.update(p, n); if (use_sha) s.update(p, n); } std::string id() { return use_sha ? s.final_hex() : x.hex(); } };

    bool hash_file(const std::string &path, bool sha, std::string &out, bool direct = false, unsigned long long *counter = nullptr, bool *cancel = nullptr) {
        int fl = O_RDONLY | (direct ? O_DIRECT : 0);
        int fd = open(path.c_str(), fl);
        if (fd < 0 && direct) fd = open(path.c_str(), O_RDONLY);
        if (fd < 0) return false;
        H h(sha); ssize_t n; unsigned long long tot = 0;
        while ((n = read(fd, buf, BUFSZ)) > 0) {
            h.upd(buf, n); tot += n;
            if (counter) { *counter += n; speed_tick(P.bytes_done + P.vbytes_done); job_write(P); }
            if (cancel && cancelled(P)) { *cancel = true; close(fd); return false; }
        }
        bool ok = n == 0;
        posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        close(fd);
        if (ok) out = h.id();
        return ok;
    }

    // ---------------------------------------------------------------- dedupe index
    void index_load() {
        std::string s; if (!read_file(indexPath, s, 64 << 20)) return;
        size_t i = 0;
        while (i < s.size()) {
            size_t e = s.find('\n', i); if (e == std::string::npos) e = s.size();
            std::string l = s.substr(i, e - i); i = e + 1;
            size_t a = l.find('\t'), b = l.find('\t', a + 1); if (a == std::string::npos || b == std::string::npos) continue;
            Idx x; x.size = strtoull(l.substr(0, a).c_str(), nullptr, 10); x.hash = l.substr(a + 1, b - a - 1); x.path = l.substr(b + 1);
            index[x.size].push_back(x);
        }
    }
    void index_save() {
        if (newIndexLines.empty()) return;
        mkdir_p(dst.mnt + "/.mediastation");
        int fd = open(indexPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644); if (fd < 0) return;
        for (auto &l : newIndexLines) { std::string x = l + "\n"; if (write(fd, x.data(), x.size()) < 0) break; }
        fsync(fd); close(fd);
    }
    bool index_has(unsigned long long size, const std::string &xh, std::string &where) {
        auto it = index.find(size); if (it == index.end()) return false;
        for (auto &x : it->second) if (x.hash == xh) {
            struct stat st; std::string a = dst.mnt + x.path;
            if (stat(a.c_str(), &st) == 0 && (unsigned long long)st.st_size == size) { where = x.path; return true; }
        }
        return false;
    }

    // ---------------------------------------------------------------- destination naming
    std::string unique_name(const std::string &path) {
        size_t sl = path.rfind('/'), dot = path.rfind('.');
        std::string base = path, ext;
        if (dot != std::string::npos && dot > sl + 1) { base = path.substr(0, dot); ext = path.substr(dot); }
        for (int i = 1; i < 10000; i++) { std::string c = base + strf(" (%d)", i) + ext; if (!exists(c)) return c; }
        return path + ".copy";
    }

    // ---------------------------------------------------------------- one file
    // returns: 0 ok/skipped, 1 error (continue), 2 fatal (stop job), 3 cancelled
    int copy_one(const FileEnt &f, const std::string &destPath0) {
        std::string destPath = destPath0;
        bool sha = P.spec.verify == "sha256";
        P.cur = f.rel; P.cur_size = f.size; P.cur_done = 0; P.phase = "copy";
        job_write(P, true);
        // 1. conflicts
        if (exists(destPath)) {
            struct stat ds; bool same = false;
            if (stat(destPath.c_str(), &ds) == 0 && S_ISREG(ds.st_mode) && (unsigned long long)ds.st_size == f.size) {
                if (std::abs((long long)ds.st_mtime - (long long)f.mtime) <= 2) same = true;
                else { std::string a, b; bool cx = false; if (hash_file(f.abs, false, a, false, nullptr, &cx) && hash_file(destPath, false, b) && a == b) same = true; }
            }
            if (P.spec.policy == "overwrite") { /* replaced by rename below */ }
            else if (same || P.spec.policy == "skip") {
                P.n_skipped++; P.files_done++; P.bytes_skipped += f.size; job_log(P, "SKIP (already there) " + f.rel);
                return 0;
            } else { destPath = unique_name(destPath); P.n_renamed++; }
        }
        // 2. dedupe by content: only pre-hash when a file of the same size is already known
        std::string pre_xx;
        if (P.spec.dedupe && index.count(f.size) && f.size > 0) {
            P.phase = "dedupe-check";
            bool cx = false;
            if (hash_file(f.abs, false, pre_xx, false, nullptr, &cx)) {
                std::string where;
                if (index_has(f.size, pre_xx, where)) {
                    P.n_dup++; P.files_done++; P.bytes_skipped += f.size; job_log(P, "DUPLICATE of " + where + " : " + f.rel);
                    return 0;
                }
            } else if (cx) return 3;
            P.phase = "copy";
        }
        // 3. copy to a temp name in the destination folder
        size_t sl = destPath.rfind('/');
        std::string dir = destPath.substr(0, sl), name = destPath.substr(sl + 1);
        if (!mkdir_p(dir)) { job_error(P, "mkdir " + dir + ": " + strerror(errno)); return 1; }
        std::string tmp = dir + "/." + name + ".mstmp";
        int in = open(f.abs.c_str(), O_RDONLY);
        if (in < 0) { job_error(P, "open " + f.rel + ": " + strerror(errno)); return 1; }
        int out = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out < 0) { int e = errno; close(in); job_error(P, "create " + name + ": " + strerror(e)); return 1; }
        H h(sha); ssize_t n; unsigned long long tot = 0; int ticks = 0;
        long long tcopy0 = now_ms();
        auto bail = [&](int r) { if (in >= 0) close(in); if (out >= 0) close(out); unlink(tmp.c_str()); return r; };
        while ((n = read(in, buf, BUFSZ)) > 0) {
            h.upd(buf, n);
            ssize_t off = 0;
            while (off < n) {
                ssize_t w = write(out, buf + off, n - off);
                if (w < 0) { if (errno == EINTR) continue; int e = errno; job_error(P, "write " + name + ": " + strerror(e)); return bail(e == ENOSPC ? 2 : 1); }
                off += w;
            }
            tot += n; P.bytes_done += n; P.cur_done = tot;
            speed_tick(P.bytes_done + P.vbytes_done);
            job_write(P);
            if ((++ticks & 3) == 0 && cancelled(P)) return bail(3);
        }
        if (n < 0) { job_error(P, "read " + f.rel + ": " + strerror(errno)); return bail(1); }
        if (tot != f.size) { /* file changed while copying: accept what we read */ }
        if (fsync(out) != 0) { job_error(P, "fsync " + name + ": " + strerror(errno)); return bail(1); }
        {
            double secs = (now_ms() - tcopy0) / 1000.0; if (secs > 0.05 && tot > (1 << 20)) P.copy_speed = tot / secs;
        }
        posix_fadvise(in, 0, 0, POSIX_FADV_DONTNEED);
        posix_fadvise(out, 0, 0, POSIX_FADV_DONTNEED);
        close(in); close(out); in = out = -1;
        ms_utime(tmp.c_str(), (long long)f.mtime, (long long)f.mtime);
        // 4. verify by reading the copy back from the medium (O_DIRECT, falls back to fadvise-evicted reads)
        std::string want = h.id(), xhash = h.x.hex();
        if (P.spec.verify != "none") {
            P.phase = "verify";
            std::string got; bool cx = false; unsigned long long before = P.vbytes_done;
            if (!hash_file(tmp, sha, got, true, &P.vbytes_done, &cx)) {
                if (cx) { unlink(tmp.c_str()); return 3; }
                job_error(P, "verify read failed " + f.rel); unlink(tmp.c_str()); P.vbytes_done = before + f.size; return 1;
            }
            if (got != want) { job_error(P, "VERIFY MISMATCH " + f.rel + " (" + want.substr(0, 16) + " != " + got.substr(0, 16) + ")"); unlink(tmp.c_str()); return 1; }
            P.n_verified++;
        }
        // 5. publish (atomic rename), index, manifest
        if (ms_rename(tmp.c_str(), destPath.c_str()) != 0) { job_error(P, "rename " + name + ": " + strerror(errno)); unlink(tmp.c_str()); return 1; }
        P.n_copied++; P.files_done++;
        {
            std::string relInDst = destPath.substr(dstRoot.size() + (dstRoot.size() < destPath.size() ? 1 : 0));
            manifest.push_back({want, relInDst});
            std::string relInVol = destPath.substr(dst.mnt.size());
            newIndexLines.push_back(strf("%llu\t%s\t%s", (unsigned long long)tot, xhash.c_str(), relInVol.c_str()));
            index[tot].push_back({tot, xhash, relInVol});
            job_log(P, strf("OK %s  %s  %s", f.rel.c_str(), human((double)tot).c_str(), want.substr(0, 16).c_str()));
        }
        return 0;
    }

    // ---------------------------------------------------------------- the job
    int run(const JobSpec &spec) {
        P.spec = spec; P.started = now_ms() / 1000; P.state = "queued";
        mkdir_p(jobs_dir());
        job_write(P, true);
        buf = nullptr;
        if (posix_memalign((void **)&buf, 4096, BUFSZ) != 0) { P.state = "error"; job_error(P, "out of memory"); job_write(P, true); return 1; }
        // serialise jobs: one at a time (they all fight for the same slow disks)
        int lk = open((std::string(STATE_DIR) + "/jobs.lock").c_str(), O_RDWR | O_CREAT, 0644);
        while (lk >= 0 && flock(lk, LOCK_EX | LOCK_NB) != 0) {
            if (cancelled(P)) { P.state = "cancelled"; P.finished = now_ms() / 1000; job_write(P, true); return 0; }
            P.note = "waiting for the previous job"; job_write(P); usleep(300000);
        }
        P.note.clear();
        P.state = "scanning"; P.phase = "scan"; job_write(P, true);
        vols = scan_volumes(false);
        const Vol *dv = find_vol(vols, spec.dstVol);
        if (!dv || !dv->mounted) return fail("destination " + spec.dstVol + " is not available");
        dst = *dv;
        if (!dst.rw) return fail("destination " + dst.name + " is read-only");
        if (dst.kind == "DVD") return fail("a DVD cannot be a destination");
        std::string dabs, drel;
        std::string dr; if (!clean_rel(spec.dstRel, dr)) return fail("bad destination path");
        {
            std::string root = canon(dst.mnt);
            if (root.empty()) return fail("destination not mounted");
            dst.mnt = root;
            std::string full = root + (dr == "/" ? "" : dr);
            if (!mkdir_p(full)) return fail("cannot create destination folder: " + std::string(strerror(errno)));
            dstRoot = canon(full);
            if (dstRoot.empty() || !(dstRoot == root || starts_with(dstRoot, root + "/"))) return fail("destination escapes the volume");
        }
        P.dstDesc = dst.name + ":" + (dr == "/" ? "" : dr);
        indexPath = dst.mnt + "/.mediastation/index.tsv";
        if (spec.dedupe) index_load();

        // gather the file list
        std::vector<FileEnt> list; std::string sd;
        for (auto &s : spec.src) {
            const Vol *sv = find_vol(vols, s.first);
            if (!sv || !sv->mounted) { job_error(P, "source " + s.first + " is not available"); continue; }
            std::string abs, rel;
            if (!resolve(*sv, s.second, abs, rel)) { job_error(P, "bad source path " + s.first + ":" + s.second); continue; }
            sd += (sd.empty() ? "" : ", ") + sv->name + ":" + rel;
            struct stat st; if (lstat(abs.c_str(), &st) != 0) { job_error(P, "missing " + rel); continue; }
            if (sv->id == dst.id) {
                if (S_ISDIR(st.st_mode) && (dstRoot == abs || starts_with(dstRoot, abs + "/"))) return fail("cannot copy a folder into itself (" + rel + ")");
            }
            if (S_ISDIR(st.st_mode)) {
                std::string prefix = rel == "/" ? "" : rel.substr(rel.rfind('/') + 1);
                if (!prefix.empty()) { FileEnt f; f.abs = abs; f.rel = prefix; f.srcvol = sv->id; f.isdir = true; list.push_back(f); }
                walk(abs, prefix, sv->id, list, 0);
            } else if (S_ISREG(st.st_mode)) {
                FileEnt f; f.abs = abs; f.rel = rel.substr(rel.rfind('/') + 1); f.srcvol = sv->id; f.size = st.st_size; f.mtime = st.st_mtime; f.mode = st.st_mode & 0777; list.push_back(f);
            }
            if (cancelled(P)) return cancel_out();
        }
        P.srcDesc = sd;
        unsigned long long need = 0, nfiles = 0;
        for (auto &f : list) if (!f.isdir) { need += (f.size + 4095) & ~4095ULL; nfiles++; P.bytes_total += f.size; }
        P.files_total = nfiles;
        if (nfiles == 0) { P.state = P.n_errors ? "error" : "done"; P.note = "nothing to copy"; P.finished = now_ms() / 1000; job_write(P, true); return 0; }
        struct statfs sf; unsigned long long freeb = 0, reserve = 0;
        if (statfs(dst.mnt.c_str(), &sf) == 0) { freeb = (unsigned long long)sf.f_bavail * sf.f_bsize; reserve = std::max<unsigned long long>(32ULL << 20, (unsigned long long)sf.f_blocks * sf.f_bsize / 200); }
        if (need + reserve > freeb) return fail(strf("not enough space on %s: need %s, free %s", dst.name.c_str(), human((double)need).c_str(), human((double)freeb).c_str()));
        if (dst.fs == "vfat") {
            for (auto &f : list) if (!f.isdir && f.size >= 4294967296ULL) { job_error(P, "FAT32 cannot hold files over 4 GB: " + f.rel); }
        }
        P.state = "copying"; job_write(P, true);
        int ret = 0;
        for (auto &f : list) {
            if (cancelled(P)) return cancel_out();
            if (!f.isdir && dst.fs == "vfat" && f.size >= 4294967296ULL) { P.files_done++; P.bytes_skipped += f.size; continue; }   // reported once during the pre-check
            std::string cat = spec.sort && !f.isdir ? category_of(f.rel) : "";
            std::string dp = dstRoot + "/" + (cat.empty() ? "" : cat + "/") + f.rel;
            if (f.isdir) { if (!spec.sort) mkdir_p(dp); continue; }
            int r = copy_one(f, dp);
            if (r == 3) return cancel_out();
            if (r == 2) { ret = 2; break; }
            job_write(P);
        }
        index_save();
        if (spec.manifest && !manifest.empty()) {
            std::string m = std::string("# Media Station manifest ") + spec.id + (spec.verify == "sha256" ? " (sha256sum -c compatible)\n" : " (XXH64)\n");
            for (auto &e : manifest) m += e.first + (spec.verify == "sha256" ? "  " : "  ") + e.second + "\n";
            write_atomic(dstRoot + "/MANIFEST-" + spec.id + ".txt", m);
        }
        sync();
        P.cur.clear(); P.phase = "finished"; P.finished = now_ms() / 1000; P.eta = 0;
        P.state = (ret == 2) ? "error" : (P.n_errors ? "done_with_errors" : "done");
        job_write(P, true);
        if (lk >= 0) close(lk);
        return 0;
    }
    int fail(const std::string &msg) { job_error(P, msg); P.state = "error"; P.finished = now_ms() / 1000; job_write(P, true); return 1; }
    int cancel_out() { P.state = "cancelled"; P.cur.clear(); P.finished = now_ms() / 1000; index_save(); job_write(P, true); return 0; }
};

} // namespace ms
