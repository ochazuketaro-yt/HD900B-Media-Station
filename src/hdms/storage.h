// SPDX-License-Identifier: GPL-3.0-or-later
// hdms storage: detect USB / SD / SATA / DVD volumes, mount them, resolve client paths safely.
#pragma once
#include "common.h"
#include <sys/statfs.h>
#include <sys/mount.h>
#include <limits.h>

namespace ms {

struct Vol {
    std::string id;        // "hdd1" "usb1" "sd1" "dvd1" -- the only name clients use
    std::string kind;      // HDD USB SD DVD
    std::string name;      // "SATA HDD" "USB FLASH" "SD CARD" "DVD DRIVE"
    std::string model;     // vendor + product from sysfs
    std::string dev;       // "sdb1"
    std::string label;     // volume label (ext/FAT)
    std::string fs;        // vfat ext4 ... (mounted fs type or sniffed)
    std::string mnt;       // mount point ("" if not mounted)
    std::string note;      // human-readable state: "no disc", "unformatted", ...
    unsigned long long size = 0, total = 0, freeb = 0;   // bytes
    bool present = true, mounted = false, rw = false, hasmedia = true;
};

static const char *MNT_BASE = "/tmp/usbmounts";    // same place the stock firmware uses

struct MountEnt { std::string dev, mnt, fs, opts; };

inline std::vector<MountEnt> read_mounts() {
    std::vector<MountEnt> v; std::string s; read_file("/proc/mounts", s, 1 << 16);
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find('\n', i); if (j == std::string::npos) j = s.size();
        std::string line = s.substr(i, j - i); i = j + 1;
        char a[256], b[512], c[64], d[512];
        if (sscanf(line.c_str(), "%255s %511s %63s %511s", a, b, c, d) == 4) v.push_back({a, b, c, d});
    }
    return v;
}

inline bool is_dir(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }

// the bus a block device hangs off, from the sysfs symlink
inline std::string bus_of(const std::string &disk) {
    char b[512]; ssize_t n = readlink(("/sys/block/" + disk).c_str(), b, sizeof b - 1);
    if (n <= 0) return "";
    b[n] = 0; std::string l = b;
    if (l.find("sata_rtk") != std::string::npos || l.find("/ata") != std::string::npos) return "SATA";
    if (l.find("/usb") != std::string::npos) return "USB";
    if (l.find("mmc") != std::string::npos) return "SD";
    return "";
}

inline std::string sysfs_text(const std::string &p) { return read_small(p); }

// read the first bytes of a (not mounted) partition and recognise ext2/3/4, FAT, NTFS, exFAT, ISO9660/UDF (+ label)
inline std::string sniff_fs(const std::string &node, std::string *label = nullptr, bool disc = false) {
    int fd = open(node.c_str(), O_RDONLY | O_NONBLOCK); if (fd < 0) return "";
    unsigned char b[4096]; memset(b, 0, sizeof b);
    ssize_t n = pread(fd, b, sizeof b, 0);
    std::string fs;
    if (n >= 1100 && b[1080] == 0x53 && b[1081] == 0xEF) {
        fs = "ext4";   // ext2/3/4 share the magic; the kernel ext4 driver mounts all of them
        if (label) { char l[17] = {0}; memcpy(l, b + 1024 + 120, 16); *label = l; }
    } else if (n >= 512 && memcmp(b + 3, "NTFS    ", 8) == 0) fs = "ntfs";
    else if (n >= 512 && memcmp(b + 3, "EXFAT   ", 8) == 0) fs = "exfat";
    else if (n >= 512 && b[510] == 0x55 && b[511] == 0xAA && (memcmp(b + 82, "FAT32", 5) == 0 || memcmp(b + 54, "FAT", 3) == 0)) {
        fs = "vfat";
        if (label) { const unsigned char *p = memcmp(b + 82, "FAT32", 5) == 0 ? b + 71 : b + 43; std::string l((const char *)p, 11); while (!l.empty() && (l.back() == ' ' || l.back() == 0)) l.pop_back(); if (l != "NO NAME") *label = l; }
    }
    if (fs.empty() && disc) {
        unsigned char v[8]; if (pread(fd, v, 8, 32768) == 8 && memcmp(v + 1, "CD001", 5) == 0) fs = "iso9660";
        else if (pread(fd, v, 8, 32768 + 2048) == 8 && (memcmp(v + 1, "BEA01", 5) == 0 || memcmp(v + 1, "NSR0", 4) == 0)) fs = "udf";
        else if (pread(fd, v, 8, 256 * 2048) >= 0) fs = "udf";   // UDF anchor area readable but unknown: try udf
    }
    close(fd);
    return fs;
}

inline std::string dev_node(const std::string &dev) { return "/dev/block/" + dev; }

// Is there a readable disc in the drive?  (read 2 KiB; ENOMEDIUM / EIO otherwise)
inline bool disc_present(const std::string &sr) {
    static std::map<std::string, std::pair<long long, bool>> cache;      // the drive is only asked every few seconds
    long long t = now_ms(); auto it = cache.find(sr);
    if (it != cache.end() && t - it->second.first < 4000) return it->second.second;
    bool r = false;
    int fd = open(dev_node(sr).c_str(), O_RDONLY | O_NONBLOCK);
    if (fd >= 0) { unsigned char b[2048]; ssize_t n = pread(fd, b, sizeof b, 0); close(fd); r = n > 0; }
    cache[sr] = {t, r};
    return r;
}

inline void load_module(const char *name) {
    std::string cmd = strf("grep -q '^%s ' /proc/modules || insmod /lib/modules/2.6.34-VENUS/kernel/fs/%s/%s.ko 2>/dev/null", name, strcmp(name, "isofs") == 0 ? "isofs" : name, name);
    if (system(cmd.c_str()) != 0) {}
}

inline bool do_mount(const std::string &dev, const std::string &fs, const std::string &mnt, std::string &err, bool ro = false) {
    mkdir_p(mnt, 0755);
    std::string opts, type = fs;
    if (fs == "vfat") opts = "uid=1000,gid=1015,fmask=0000,dmask=0000,codepage=cp437,iocharset=utf8,shortname=mixed,utf8,errors=remount-ro,noatime";
    else if (fs == "ext4") opts = "noatime,nobarrier";
    else if (fs == "iso9660") { opts = "ro"; ro = true; load_module("isofs"); }
    else if (fs == "udf") { opts = "ro"; ro = true; load_module("crc-itu-t"); load_module("udf"); }
    else if (fs == "ntfs" || fs == "exfat") { type = "ufsd"; opts = "force,noatime,iocharset=utf8"; }
    unsigned long fl = MS_NOATIME | (ro ? MS_RDONLY : 0);
    if (mount(dev_node(dev).c_str(), mnt.c_str(), type.c_str(), fl, opts.c_str()) == 0) return true;
    err = strerror(errno);
    if (fs == "ntfs") {                // fall back to the read-only kernel NTFS driver
        if (mount(dev_node(dev).c_str(), mnt.c_str(), "ntfs", MS_RDONLY | MS_NOATIME, "iocharset=utf8") == 0) return true;
    }
    return false;
}

inline std::vector<Vol> scan_volumes(bool sniff = true) {
    std::vector<Vol> out;
    std::vector<MountEnt> mts = read_mounts();
    std::map<std::string, int> cnt;
    DIR *d = opendir("/sys/block"); if (!d) return out;
    std::vector<std::string> disks;
    while (dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (starts_with(n, "sd") || starts_with(n, "mmcblk") || starts_with(n, "sr")) disks.push_back(n);
    }
    closedir(d);
    std::sort(disks.begin(), disks.end());
    for (auto &disk : disks) {
        bool isdvd = starts_with(disk, "sr");
        std::string bus = isdvd ? "DVD" : bus_of(disk);
        long long sectors = read_ll("/sys/block/" + disk + "/size", 0);
        std::string vendor = trim(sysfs_text("/sys/block/" + disk + "/device/vendor")), model = trim(sysfs_text("/sys/block/" + disk + "/device/model"));
        std::string dm = trim(vendor + " " + model);
        if (dm.empty() && starts_with(disk, "mmcblk")) dm = trim(sysfs_text("/sys/block/" + disk + "/device/name"));
        bool removable = read_ll("/sys/block/" + disk + "/removable", 0) != 0;
        std::string kind = isdvd ? "DVD" : bus == "SATA" ? "HDD" : bus == "SD" ? "SD" : bus == "USB" ? "USB" : "OTHER";
        if (kind == "OTHER") continue;
        // partitions
        std::vector<std::string> parts;
        if (!isdvd) {
            DIR *pd = opendir(("/sys/block/" + disk).c_str());
            while (pd && (true)) { dirent *e = readdir(pd); if (!e) break; std::string n = e->d_name; if (starts_with(n, disk) && n.size() > disk.size()) parts.push_back(n); }
            if (pd) closedir(pd);
            std::sort(parts.begin(), parts.end());
        }
        std::vector<std::pair<std::string, long long>> units;   // (device name, sectors)
        for (auto &p : parts) units.push_back({p, read_ll("/sys/block/" + disk + "/" + p + "/size", 0)});
        if (units.empty()) units.push_back({disk, sectors});
        if (isdvd && !disc_present(disk)) {
            Vol v; v.kind = kind; v.dev = disk; v.model = dm; v.name = "DVD DRIVE"; v.id = strf("dvd%d", ++cnt[kind]);
            v.hasmedia = false; v.note = "no disc"; v.size = 0; out.push_back(v); continue;
        }
        if (isdvd) { sectors = 0; units[0].second = 0; }
        for (auto &u : units) {
            Vol v; v.kind = kind; v.dev = u.first; v.model = dm;
            v.size = (unsigned long long)u.second * 512ULL;
            if (isdvd) { long long s = read_ll("/sys/block/" + disk + "/size", 0); v.size = s == 2097151 ? 0 : (unsigned long long)s * 512ULL; }
            int k = ++cnt[kind];
            v.id = (kind == "HDD" ? "hdd" : kind == "USB" ? "usb" : kind == "SD" ? "sd" : "dvd") + strf("%d", k);
            v.name = kind == "HDD" ? "SATA HDD" : kind == "SD" ? "SD CARD" : kind == "DVD" ? "DVD DISC" : (removable ? "USB FLASH" : "USB DISK");
            // mounted where?  prefer our own /tmp/usbmounts/<dev>, else any mount of the device node
            const MountEnt *m = nullptr;
            for (auto &x : mts) if (x.dev == dev_node(v.dev) || x.dev == "/dev/" + v.dev) { if (!m || starts_with(x.mnt, MNT_BASE)) m = &x; }
            if (m && !m->mnt.empty()) {
                v.mounted = true; v.mnt = m->mnt; v.fs = m->fs; v.rw = m->opts.find("rw") == 0 || m->opts.find(",rw") != std::string::npos;
                struct statfs sf; if (statfs(v.mnt.c_str(), &sf) == 0) { v.total = (unsigned long long)sf.f_blocks * sf.f_bsize; v.freeb = (unsigned long long)sf.f_bavail * sf.f_bsize; }
                if (sniff) { std::string l; sniff_fs(dev_node(v.dev), &l); v.label = l; }
            } else if (sniff) {
                v.fs = sniff_fs(dev_node(v.dev), &v.label, isdvd);
                v.note = v.fs.empty() ? (isdvd ? "disc not readable as data" : "no filesystem (unformatted)") : "not mounted";
            }
            out.push_back(v);
        }
    }
    return out;
}

// -------------------------------------------------------------------------------- mount manager (called periodically)
struct MountState { long long next_try = 0; std::string last_err; };

inline void automount(std::map<std::string, MountState> &st, std::vector<std::string> &log) {
    std::vector<Vol> vols = scan_volumes(true);
    long long t = now_ms();
    for (auto &v : vols) {
        if (v.mounted || v.fs.empty() || !v.present) continue;
        MountState &ms = st[v.dev];
        if (t < ms.next_try) continue;
        std::string mnt = std::string(MNT_BASE) + "/" + v.dev, err;
        bool ok = do_mount(v.dev, v.fs, mnt, err);
        if (ok) { log.push_back(strf("mounted %s (%s) at %s", v.dev.c_str(), v.fs.c_str(), mnt.c_str())); ms.next_try = 0; }
        else { log.push_back(strf("mount %s as %s failed: %s", v.dev.c_str(), v.fs.c_str(), err.c_str())); ms.next_try = t + 30000; ms.last_err = err; }
    }
}

// -------------------------------------------------------------------------------- safe path handling
inline const Vol *find_vol(const std::vector<Vol> &vols, const std::string &id) {
    for (auto &v : vols) if (v.id == id) return &v;
    return nullptr;
}

// Normalise a client-supplied relative path ("/a/b", "a//b"): reject "..", NUL, control chars.
inline bool clean_rel(const std::string &in, std::string &out) {
    out.clear();
    std::vector<std::string> parts; size_t i = 0;
    for (char c : in) if ((unsigned char)c < 0x20 || c == 0x7f) return false;
    while (i <= in.size()) {
        size_t j = in.find('/', i); if (j == std::string::npos) j = in.size();
        std::string seg = in.substr(i, j - i);
        if (seg == "..") return false;
        if (!seg.empty() && seg != ".") parts.push_back(seg);
        i = j + 1;
    }
    for (auto &p : parts) out += "/" + p;
    if (out.empty()) out = "/";
    return true;
}

inline std::string canon(const std::string &p) { char b[PATH_MAX]; return realpath(p.c_str(), b) ? std::string(b) : std::string(); }

// Resolve vol + relative path to an absolute path that is verified to stay inside the volume (symlinks included).
// mustexist=false: only the parent has to exist (for creating things).
inline bool resolve(const Vol &v, const std::string &rel_in, std::string &abs, std::string &rel, bool mustexist = true) {
    if (!v.mounted || v.mnt.empty()) return false;
    if (!clean_rel(rel_in, rel)) return false;
    std::string root = canon(v.mnt); if (root.empty()) return false;
    std::string full = root + (rel == "/" ? "" : rel);
    std::string c;
    if (mustexist) c = canon(full);
    else {
        size_t k = full.rfind('/'); std::string par = canon(full.substr(0, k)); if (par.empty()) return false;
        c = par + full.substr(k);
        if (exists(c)) { std::string cc = canon(c); if (cc.empty() || !(cc == root || starts_with(cc, root + "/"))) return false; }
    }
    if (c.empty()) return false;
    if (!(c == root || starts_with(c, root + "/"))) return false;
    abs = c; return true;
}

inline bool hidden_name(const std::string &n) {
    return n == "lost+found" || n == "System Volume Information" || n == "$RECYCLE.BIN" || n == ".Trash-0" || n == ".Spotlight-V100" ||
           n == ".fseventsd" || n == ".mediastation" || (starts_with(n, ".") && ends_with(n, ".mstmp"));
}

} // namespace ms
