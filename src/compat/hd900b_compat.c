/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * hd900b_compat.c -- syscall compatibility layer: uClibc-ng 1.0.50 (time64, statx) on Linux 2.6.34.
 *
 * Why: the Bootlin "mips32el--uclibc--stable-2024.05-1" toolchain ships a uClibc-ng whose
 *   - stat/lstat/fstat/stat64/lstat64/fstatat use statx(4366)                      -> ENOSYS on 2.6.34
 *   - getrlimit/setrlimit/prlimit use prlimit64(4338), no fallback                 -> ENOSYS
 *   - uses a 64-bit time_t but passes it straight to the 32-bit-timespec syscalls  -> clock_gettime()/time()/
 *     gettimeofday() return garbage (the high word of tv_sec contains tv_nsec) and nanosleep()/select() lose
 *     their fractional part.  OpenSSL then reports "format error in certificate's notBefore field".
 *
 * How: this object defines the same symbols (and the libc-internal __GI_* aliases) and is named on the link
 * line BEFORE libc.a, so the archive members that carry the broken versions are never pulled in.
 * Old syscalls used (MIPS o32): stat64 4213, lstat64 4214, fstat64 4215, newfstatat 4293, getrlimit 4076,
 * setrlimit 4075, gettimeofday 4078, nanosleep 4166, _newselect 4142, clock_* 4262-4265.
 *
 * Link with:  mipsel-linux-gcc ... hd900b_compat.o <objects> ... -Wl,-u,__xstat64_conv
 * Verify with: tools/check_newsyscalls.sh <elf>   and run tests/t3_compat_test on the device.
 *
 * NOT covered (inline syscalls inside libc.a): pthread_cond_timedwait, pthread_mutex_timedlock,
 * sem_timedwait, pselect, ppoll, timerfd, utimensat -> do not rely on timed waits.
 */
#define _GNU_SOURCE
#define _LARGEFILE64_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>

#define NR_getrlimit_old   4076
#define NR_setrlimit_old   4075
#define NR_stat64_old      4213
#define NR_lstat64_old     4214
#define NR_fstat64_old     4215
#define NR_newfstatat_old  4293
#define NR_getrandom       4353

#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

/* libc.a internal (xstatconv.os): kernel struct stat64 -> libc struct stat64 */
extern void __xstat64_conv(const void *kbuf, struct stat64 *buf);

/* generous, 8-byte aligned buffer for the kernel's struct stat64 (<=144 B) */
typedef struct { unsigned long long raw[24]; } kstat_t;

#define EXPORT __attribute__((visibility("default"), used))
#define ALIAS(name, target) \
    extern __typeof(target) name __attribute__((alias(#target), visibility("default")))

/* ---- stat64 family -------------------------------------------------- */
static int do_stat64(long nr, const char *path, struct stat64 *buf)
{
    kstat_t k;
    long r = syscall(nr, path, &k);
    if (r != 0)
        return -1;                 /* errno set by syscall() */
    __xstat64_conv(&k, buf);
    return 0;
}

EXPORT int stat64(const char *path, struct stat64 *buf)  { return do_stat64(NR_stat64_old, path, buf); }
EXPORT int lstat64(const char *path, struct stat64 *buf) { return do_stat64(NR_lstat64_old, path, buf); }

static int do_fstat64(int fd, struct stat64 *buf)
{
    kstat_t k;
    long r = syscall(NR_fstat64_old, fd, &k);
    if (r != 0)
        return -1;
    __xstat64_conv(&k, buf);
    return 0;
}

/* ---- 32-bit struct stat (derived from the stat64 result; EOVERFLOW check) */
static int narrow(const struct stat64 *s, struct stat *o)
{
    if (s->st_ino > 0xffffffffULL || s->st_size > 0x7fffffffLL ||
        s->st_blocks > 0x7fffffffLL) {
        errno = EOVERFLOW;
        return -1;
    }
    memset(o, 0, sizeof *o);
    o->st_dev = s->st_dev;
    o->st_ino = (__ino_t)s->st_ino;
    o->st_mode = s->st_mode;
    o->st_nlink = s->st_nlink;
    o->st_uid = s->st_uid;
    o->st_gid = s->st_gid;
    o->st_rdev = s->st_rdev;
    o->st_size = (__off_t)s->st_size;
    o->st_atim = s->st_atim;
    o->st_mtim = s->st_mtim;
    o->st_ctim = s->st_ctim;
    o->st_blksize = s->st_blksize;
    o->st_blocks = (__blkcnt_t)s->st_blocks;
    return 0;
}

EXPORT int stat(const char *path, struct stat *buf)
{
    struct stat64 s;
    if (do_stat64(NR_stat64_old, path, &s)) return -1;
    return narrow(&s, buf);
}
EXPORT int lstat(const char *path, struct stat *buf)
{
    struct stat64 s;
    if (do_stat64(NR_lstat64_old, path, &s)) return -1;
    return narrow(&s, buf);
}
EXPORT int fstat(int fd, struct stat *buf)
{
    struct stat64 s;
    if (do_fstat64(fd, &s)) return -1;
    return narrow(&s, buf);
}

/* ---- fstatat family (AT_EMPTY_PATH needs >=2.6.39: emulate for "") ----- */
EXPORT int fstatat64(int dirfd, const char *path, struct stat64 *buf, int flags)
{
    if ((flags & AT_EMPTY_PATH) && path && path[0] == '\0')
        return do_fstat64(dirfd, buf);
    {
        kstat_t k;
        long r = syscall(NR_newfstatat_old, dirfd, path, &k, flags & ~AT_EMPTY_PATH);
        if (r != 0) return -1;
        __xstat64_conv(&k, buf);
        return 0;
    }
}
EXPORT int fstatat(int dirfd, const char *path, struct stat *buf, int flags)
{
    struct stat64 s;
    if (fstatat64(dirfd, path, &s, flags)) return -1;
    return narrow(&s, buf);
}

/* ---- rlimit ----------------------------------------------------------
 * Old MIPS kernel ABI: struct {unsigned long cur,max}; RLIM_INFINITY(kernel)
 * is 0x7fffffff (asm/resource.h of the sysroot). Treat >=0x7fffffff as
 * infinity both ways. (Exact value the 2.6.34 kernel returns is printed by
 * the next device test.)                                                    */
#define KRLIM_INF 0x7fffffffUL
struct krlimit { unsigned long cur, max; };

static int kget(int res, unsigned long long *cur, unsigned long long *max)
{
    struct krlimit k;
    if (syscall(NR_getrlimit_old, res, &k) != 0) return -1;
    *cur = (k.cur >= KRLIM_INF) ? ~0ULL : k.cur;
    *max = (k.max >= KRLIM_INF) ? ~0ULL : k.max;
    return 0;
}
static int kset(int res, unsigned long long cur, unsigned long long max)
{
    struct krlimit k;
    k.cur = (cur >= KRLIM_INF) ? KRLIM_INF : (unsigned long)cur;
    k.max = (max >= KRLIM_INF) ? KRLIM_INF : (unsigned long)max;
    return syscall(NR_setrlimit_old, res, &k) == 0 ? 0 : -1;
}

/* userspace 32-bit rlim_t infinity is RLIM_INFINITY from <bits/resource.h> */
static unsigned long to32(unsigned long long v) { return v == ~0ULL ? (unsigned long)RLIM_INFINITY : (v > 0xfffffffeULL ? (unsigned long)RLIM_INFINITY : (unsigned long)v); }
static unsigned long long from32(unsigned long v) { return v == (unsigned long)RLIM_INFINITY ? ~0ULL : v; }

EXPORT int getrlimit(__rlimit_resource_t res, struct rlimit *rl)
{
    unsigned long long c, m;
    if (!rl) { errno = EFAULT; return -1; }
    if (kget((int)res, &c, &m)) return -1;
    rl->rlim_cur = to32(c); rl->rlim_max = to32(m);
    return 0;
}
EXPORT int setrlimit(__rlimit_resource_t res, const struct rlimit *rl)
{
    if (!rl) { errno = EFAULT; return -1; }
    return kset((int)res, from32(rl->rlim_cur), from32(rl->rlim_max));
}
EXPORT int getrlimit64(__rlimit_resource_t res, struct rlimit64 *rl)
{
    unsigned long long c, m;
    if (!rl) { errno = EFAULT; return -1; }
    if (kget((int)res, &c, &m)) return -1;
    rl->rlim_cur = c; rl->rlim_max = m;
    return 0;
}
EXPORT int setrlimit64(__rlimit_resource_t res, const struct rlimit64 *rl)
{
    if (!rl) { errno = EFAULT; return -1; }
    return kset((int)res, rl->rlim_cur, rl->rlim_max);
}

/* prlimit: only the calling process (pid 0 or getpid()) can be served by
 * the old syscalls; anything else is reported as ENOSYS (honest). */
EXPORT int prlimit64(pid_t pid, __rlimit_resource_t res,
                     const struct rlimit64 *nw, struct rlimit64 *old)
{
    if (pid != 0 && pid != getpid()) { errno = ENOSYS; return -1; }
    if (old && getrlimit64(res, old)) return -1;
    if (nw && setrlimit64(res, nw)) return -1;
    return 0;
}
EXPORT int prlimit(pid_t pid, __rlimit_resource_t res,
                   const struct rlimit *nw, struct rlimit *old)
{
    if (pid != 0 && pid != getpid()) { errno = ENOSYS; return -1; }
    if (old && getrlimit(res, old)) return -1;
    if (nw && setrlimit(res, nw)) return -1;
    return 0;
}

/* ---- getrandom (optional): real syscall, else /dev/urandom ----------- */
EXPORT ssize_t getrandom(void *buf, size_t len, unsigned int flags)
{
    long r = syscall(NR_getrandom, buf, len, flags);
    if (r >= 0 || errno != ENOSYS)
        return r;
    {
        int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
        size_t got = 0;
        if (fd < 0) { errno = ENOSYS; return -1; }
        while (got < len) {
            ssize_t n = read(fd, (char *)buf + got, len - got);
            if (n < 0) { if (errno == EINTR) continue; close(fd); return got ? (ssize_t)got : -1; }
            if (n == 0) break;
            got += (size_t)n;
        }
        close(fd);
        return (ssize_t)got;
    }
}

/* ======================================================================
 * TIME64 repair (found 2026-10-11 via OpenSSL "notBefore format error").
 * uClibc-ng 1.0.50 here uses a 64-bit time_t (struct timespec {int64 tv_sec; long tv_nsec;}),
 * but calls the Linux 2.6.34 syscalls with the user's struct directly: the kernel only knows
 * {long sec; long nsec} (8 bytes).  Result: clock_gettime() leaves tv_sec's HIGH word = nsec and tv_nsec
 * garbage, so time(), gettimeofday() (built on it) and every timer derived from it are wrong; relative
 * timeouts (nanosleep/select/futex) lose their fractional part.  Fix: convert at the syscall boundary.
 * NOT covered (libc-internal inline syscalls): pthread_cond_timedwait, pthread_mutex_timedlock,
 * sem_timedwait, pselect, ppoll, timerfd, utimensat -> do not rely on timed waits in code linked with this.
 * ====================================================================== */
#define NR_gettimeofday_old 4078
#define NR_nanosleep_old    4166
#define NR_newselect_old    4142
#define NR_clock_settime_o  4262
#define NR_clock_gettime_o  4263
#define NR_clock_getres_o   4264
#define NR_clock_nanosleep_o 4265
struct ktimespec { long tv_sec; long tv_nsec; };
struct ktimeval  { long tv_sec; long tv_usec; };
_Static_assert(sizeof(((struct timespec *)0)->tv_sec) == 8, "this layer assumes a 64-bit time_t (uClibc-ng time64)");

EXPORT int clock_gettime(clockid_t clk, struct timespec *ts)
{
    struct ktimespec k;
    if (syscall(NR_clock_gettime_o, clk, &k) != 0) return -1;
    ts->tv_sec = k.tv_sec; ts->tv_nsec = k.tv_nsec; return 0;
}
EXPORT int clock_getres(clockid_t clk, struct timespec *ts)
{
    struct ktimespec k;
    if (syscall(NR_clock_getres_o, clk, ts ? &k : 0) != 0) return -1;
    if (ts) { ts->tv_sec = k.tv_sec; ts->tv_nsec = k.tv_nsec; }
    return 0;
}
EXPORT int clock_settime(clockid_t clk, const struct timespec *ts)
{
    struct ktimespec k = { (long)ts->tv_sec, (long)ts->tv_nsec };
    return syscall(NR_clock_settime_o, clk, &k) == 0 ? 0 : -1;
}
EXPORT int gettimeofday(struct timeval *__restrict tv, __timezone_ptr_t tz)
{
    struct ktimeval k;
    (void)tz;
    if (!tv) return 0;
    if (syscall(NR_gettimeofday_old, &k, 0) != 0) return -1;
    tv->tv_sec = k.tv_sec; tv->tv_usec = k.tv_usec; return 0;
}
EXPORT time_t time(time_t *t)
{
    struct ktimeval k; time_t r;
    if (syscall(NR_gettimeofday_old, &k, 0) != 0) r = (time_t)-1; else r = k.tv_sec;
    if (t) *t = r;
    return r;
}
static int knanosleep(const struct timespec *req, struct timespec *rem)
{
    struct ktimespec kq, kr = {0, 0};
    long r;
    kq.tv_sec = (long)req->tv_sec; kq.tv_nsec = (long)req->tv_nsec;
    r = syscall(NR_nanosleep_old, &kq, &kr);
    if (rem) { rem->tv_sec = kr.tv_sec; rem->tv_nsec = kr.tv_nsec; }
    return r == 0 ? 0 : -1;
}
EXPORT int nanosleep(const struct timespec *req, struct timespec *rem) { return knanosleep(req, rem); }
EXPORT int __nanosleep_nocancel(const struct timespec *req, struct timespec *rem) { return knanosleep(req, rem); }
EXPORT int clock_nanosleep(clockid_t clk, int flags, const struct timespec *req, struct timespec *rem)
{
    struct ktimespec kq = { (long)req->tv_sec, (long)req->tv_nsec }, kr = {0, 0};
    long r = syscall(NR_clock_nanosleep_o, clk, flags, &kq, &kr);
    if (r != 0) {                      /* clock_nanosleep returns the error number */
        int e = errno;
        if (rem && !(flags & 1)) { rem->tv_sec = kr.tv_sec; rem->tv_nsec = kr.tv_nsec; }
        return e;
    }
    return 0;
}
EXPORT int select(int n, fd_set *r, fd_set *w, fd_set *x, struct timeval *tv)
{
    struct ktimeval k; long rc;
    if (tv) { k.tv_sec = (long)tv->tv_sec; k.tv_usec = (long)tv->tv_usec; }
    rc = syscall(NR_newselect_old, n, r, w, x, tv ? &k : 0);
    if (tv) { tv->tv_sec = k.tv_sec; tv->tv_usec = k.tv_usec; }
    return (int)rc;
}
EXPORT int __select_nocancel(int n, fd_set *r, fd_set *w, fd_set *x, struct timeval *tv) { return select(n, r, w, x, tv); }
ALIAS(__libc_clock_gettime, clock_gettime);
ALIAS(__GI_clock_getres, clock_getres);
ALIAS(__GI_gettimeofday, gettimeofday);
ALIAS(__GI_time, time);
ALIAS(__GI_nanosleep, nanosleep);
ALIAS(__GI_select, select);

/* ---- libc-internal aliases (what other libc.a members reference) ----- */
ALIAS(__GI_stat, stat);
ALIAS(__GI_lstat, lstat);
ALIAS(__GI_fstat, fstat);
ALIAS(__GI_stat64, stat64);
ALIAS(__GI_lstat64, lstat64);
ALIAS(__GI_fstatat, fstatat);
ALIAS(__GI_fstatat64, fstatat64);
ALIAS(__GI_getrlimit, getrlimit);
ALIAS(__GI_setrlimit, setrlimit);
