// SPDX-License-Identifier: GPL-3.0-or-later
// hdms common helpers: strings, JSON, files, hashes (SHA-256 + XXH64).  No threads: hdms is fork-based.
#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdarg>
#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/syscall.h>

namespace ms {

// uClibc-ng 1.0.50 implements rename() with renameat2 (syscall 4351), which Linux 2.6.34 does not have -> ENOSYS.
// Call the old rename syscall directly (MIPS o32: __NR_rename = 4038).
// uClibc-ng utime()/utimes() hand 64-bit time_t to a 32-bit kernel ABI (result: 1970).  Raw __NR_utime = 4030 with 32-bit fields.
inline int ms_utime(const char *path, long long atime, long long mtime) {
#if defined(__mips__)
    unsigned int kb[2] = {(unsigned int)atime, (unsigned int)mtime};
    return (int)syscall(4030, path, kb);
#else
    (void)path; (void)atime; (void)mtime; return 0;
#endif
}
inline int ms_rename(const char *a, const char *b) {
#if defined(__mips__)
    return (int)syscall(4038, a, b);
#else
    return rename(a, b);
#endif
}

inline std::string strf(const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < (int)sizeof buf) return std::string(buf, n < 0 ? 0 : n);
    std::string s(n + 1, '\0');
    va_start(ap, fmt); vsnprintf(&s[0], n + 1, fmt, ap); va_end(ap);
    s.resize(n);
    return s;
}

inline long long now_ms() {
    struct timeval tv; gettimeofday(&tv, nullptr);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

inline bool starts_with(const std::string &s, const std::string &p) { return s.compare(0, p.size(), p) == 0; }
inline bool ends_with(const std::string &s, const std::string &p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }
inline std::string lower(std::string s) { for (auto &c : s) if (c >= 'A' && c <= 'Z') c += 32; return s; }

inline std::string json_esc(const std::string &s) {
    std::string o; o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) o += strf("\\u%04x", c); else o += (char)c;
        }
    }
    return o;
}
inline std::string jstr(const std::string &s) { return "\"" + json_esc(s) + "\""; }

inline std::string xml_esc(const std::string &s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        default: if (c >= 0x20 || c == '\n' || c == '\t') o += (char)c;
        }
    }
    return o;
}

inline int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
inline std::string url_decode(const std::string &s, bool plus = true) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() + 0 && hexv(s[i + 1]) >= 0 && hexv(s[i + 2]) >= 0) { o += (char)(hexv(s[i + 1]) * 16 + hexv(s[i + 2])); i += 2; }
        else if (s[i] == '+' && plus) o += ' ';
        else o += s[i];
    }
    return o;
}
inline std::string url_enc(const std::string &s) {
    std::string o;
    for (unsigned char c : s) {
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') o += (char)c;
        else o += strf("%%%02X", c);
    }
    return o;
}

// query string / form body -> multimap (keeps repeated keys, e.g. several src=)
typedef std::vector<std::pair<std::string, std::string>> Params;
inline Params parse_params(const std::string &q) {
    Params p; size_t i = 0;
    while (i <= q.size()) {
        size_t j = q.find('&', i); if (j == std::string::npos) j = q.size();
        if (j > i) {
            std::string kv = q.substr(i, j - i); size_t e = kv.find('=');
            if (e == std::string::npos) p.push_back({url_decode(kv), ""});
            else p.push_back({url_decode(kv.substr(0, e)), url_decode(kv.substr(e + 1))});
        }
        i = j + 1;
    }
    return p;
}
inline std::string pget(const Params &p, const std::string &k, const std::string &def = "") {
    for (auto &kv : p) if (kv.first == k) return kv.second;
    return def;
}
inline std::vector<std::string> pgetall(const Params &p, const std::string &k) {
    std::vector<std::string> v; for (auto &kv : p) if (kv.first == k) v.push_back(kv.second); return v;
}

inline bool read_file(const std::string &path, std::string &out, size_t max = 1 << 20) {
    int fd = open(path.c_str(), O_RDONLY); if (fd < 0) return false;
    out.clear(); char b[4096]; ssize_t n;
    while ((n = read(fd, b, sizeof b)) > 0) { out.append(b, n); if (out.size() > max) break; }
    close(fd); return true;
}
inline std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t' || s.back() == '\0')) s.pop_back();
    size_t i = 0; while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}
inline std::string read_small(const std::string &path) { std::string s; if (!read_file(path, s, 8192)) return ""; return trim(s); }
inline long long read_ll(const std::string &path, long long def = -1) {
    std::string s = read_small(path); if (s.empty()) return def; return atoll(s.c_str());
}
// atomic replace: write to path.tmp then rename
inline bool write_atomic(const std::string &path, const std::string &data) {
    std::string tmp = path + ".tmp" + strf("%d", (int)getpid());
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0) return false;
    size_t off = 0;
    while (off < data.size()) { ssize_t n = write(fd, data.data() + off, data.size() - off); if (n <= 0) { close(fd); unlink(tmp.c_str()); return false; } off += n; }
    close(fd);
    if (ms_rename(tmp.c_str(), path.c_str()) != 0) { unlink(tmp.c_str()); return false; }
    return true;
}
inline bool mkdir_p(const std::string &path, mode_t mode = 0755) {
    std::string cur;
    for (size_t i = 0; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '/') {
            if (!cur.empty() && mkdir(cur.c_str(), mode) != 0 && errno != EEXIST) return false;
        }
        if (i < path.size()) cur += path[i];
    }
    struct stat st; return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
inline bool exists(const std::string &p) { struct stat st; return lstat(p.c_str(), &st) == 0; }

inline std::string human(double b) {
    const char *u[] = {"B", "KB", "MB", "GB", "TB"}; int i = 0;
    while (b >= 1000 && i < 4) { b /= 1000; i++; }
    return strf(i == 0 ? "%.0f %s" : (b >= 100 ? "%.0f %s" : "%.1f %s"), b, u[i]);
}

// ------------------------------------------------------------------------------------------ SHA-256
struct Sha256 {
    uint32_t h[8]; uint64_t len; unsigned char buf[64]; size_t bl;
    Sha256() { reset(); }
    void reset() {
        static const uint32_t i0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        memcpy(h, i0, sizeof h); len = 0; bl = 0;
    }
    static inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const unsigned char *p) {
        static const uint32_t K[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25), ch = (e & f) ^ (~e & g), t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22), mj = (a & b) ^ (a & c) ^ (b & c), t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const void *data, size_t n) {
        const unsigned char *p = (const unsigned char *)data; len += n;
        if (bl) { size_t t = 64 - bl; if (t > n) t = n; memcpy(buf + bl, p, t); bl += t; p += t; n -= t; if (bl == 64) { block(buf); bl = 0; } }
        while (n >= 64) { block(p); p += 64; n -= 64; }
        if (n) { memcpy(buf, p, n); bl = n; }
    }
    std::string final_hex() {
        uint64_t bits = len * 8; unsigned char pad[72] = {0x80}; size_t padn = (bl < 56) ? 56 - bl : 120 - bl;
        unsigned char lenb[8]; for (int i = 0; i < 8; i++) lenb[i] = (unsigned char)(bits >> (56 - 8 * i));
        update(pad, padn); update(lenb, 8);
        std::string o; for (int i = 0; i < 8; i++) o += strf("%08x", h[i]); return o;
    }
};

// ------------------------------------------------------------------------------------------ XXH64 (streaming)
struct Xxh64 {
    static const uint64_t P1 = 11400714785074694791ULL, P2 = 14029467366897019727ULL, P3 = 1609587929392839161ULL, P4 = 9650029242287828579ULL, P5 = 2870177450012600261ULL;
    uint64_t v1, v2, v3, v4, total; unsigned char mem[32]; size_t ml;
    Xxh64() { reset(); }
    static inline uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
    static inline uint64_t rd64(const unsigned char *p) { uint64_t v; memcpy(&v, p, 8); return v; }   // little-endian host (mipsel / x86)
    static inline uint32_t rd32(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return v; }
    static inline uint64_t round(uint64_t acc, uint64_t in) { acc += in * P2; acc = rotl(acc, 31); return acc * P1; }
    static inline uint64_t merge(uint64_t acc, uint64_t v) { v = round(0, v); acc ^= v; return acc * P1 + P4; }
    void reset() { v1 = P1 + P2; v2 = P2; v3 = 0; v4 = 0ULL - P1; total = 0; ml = 0; }
    void update(const void *data, size_t n) {
        const unsigned char *p = (const unsigned char *)data; total += n;
        if (ml + n < 32) { memcpy(mem + ml, p, n); ml += n; return; }
        if (ml) {
            size_t t = 32 - ml; memcpy(mem + ml, p, t);
            v1 = round(v1, rd64(mem)); v2 = round(v2, rd64(mem + 8)); v3 = round(v3, rd64(mem + 16)); v4 = round(v4, rd64(mem + 24));
            p += t; n -= t; ml = 0;
        }
        while (n >= 32) {
            v1 = round(v1, rd64(p)); v2 = round(v2, rd64(p + 8)); v3 = round(v3, rd64(p + 16)); v4 = round(v4, rd64(p + 24));
            p += 32; n -= 32;
        }
        if (n) { memcpy(mem, p, n); ml = n; }
    }
    uint64_t digest() const {
        uint64_t h;
        if (total >= 32) {
            h = rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18);
            h = merge(h, v1); h = merge(h, v2); h = merge(h, v3); h = merge(h, v4);
        } else h = v3 + P5;
        h += total;
        const unsigned char *p = mem; size_t n = ml;
        while (n >= 8) { h ^= round(0, rd64(p)); h = rotl(h, 27) * P1 + P4; p += 8; n -= 8; }
        if (n >= 4) { h ^= (uint64_t)rd32(p) * P1; h = rotl(h, 23) * P2 + P3; p += 4; n -= 4; }
        while (n) { h ^= (*p) * P5; h = rotl(h, 11) * P1; p++; n--; }
        h ^= h >> 33; h *= P2; h ^= h >> 29; h *= P3; h ^= h >> 32;
        return h;
    }
    std::string hex() const { return strf("%016llx", (unsigned long long)digest()); }
};

} // namespace ms
