/* probe which libc calls used by hdms fail with ENOSYS on this kernel */
#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <utime.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/statfs.h>
#include <sys/mount.h>
#include <time.h>
#include <sys/time.h>
#include <dirent.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/syscall.h>
#define T(name, expr) do { errno = 0; long r = (long)(expr); printf("%-14s r=%ld errno=%d %s\n", name, r, errno, errno ? strerror(errno) : ""); } while (0)
int main(void) {
    mkdir("/tmp/ms/probe", 0755);
    int fd = open("/tmp/ms/probe/a", O_WRONLY | O_CREAT | O_TRUNC, 0644); write(fd, "hello", 5);
    T("fsync", fsync(fd)); T("fdatasync", fdatasync(fd)); T("fadvise", posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED)); T("flock", flock(fd, LOCK_EX | LOCK_NB)); close(fd);
    T("rename", syscall(4038, "/tmp/ms/probe/a", "/tmp/ms/probe/b"));
    struct utimbuf ut = {1000000000, 1000000000}; T("utime", utime("/tmp/ms/probe/b", &ut));
    struct timeval tv[2] = {{1000000000, 0}, {1000000000, 0}}; T("utimes", utimes("/tmp/ms/probe/b", tv));
    struct stat st; T("stat", stat("/tmp/ms/probe/b", &st)); printf("   mtime=%ld size=%ld\n", (long)st.st_mtime, (long)st.st_size);
    T("lstat", lstat("/tmp/ms/probe/b", &st));
    struct statfs sf; T("statfs", statfs("/tmp/ms/probe", &sf));
    T("access", access("/tmp/ms/probe/b", F_OK));
    char *rp = realpath("/tmp/ms/probe/b", NULL); T("realpath", rp != NULL);
    T("symlink", symlink("b", "/tmp/ms/probe/l")); char lb[64]; T("readlink", readlink("/tmp/ms/probe/l", lb, 64)); T("unlink-l", unlink("/tmp/ms/probe/l"));
    T("mkdir", mkdir("/tmp/ms/probe/d", 0755)); T("rmdir", rmdir("/tmp/ms/probe/d"));
    T("unlink", unlink("/tmp/ms/probe/b"));
    T("rmdir-probe", rmdir("/tmp/ms/probe"));
    T("O_DIRECT open", open("/tmp/ms/mke2fs", O_RDONLY | O_DIRECT));
    T("sync", (sync(), 0));
    T("posix_memalign", 0);
    struct timespec ts; T("clock_gettime", clock_gettime(CLOCK_MONOTONIC, &ts)); printf("   mono=%ld\n", (long)ts.tv_sec);
    T("time", time(NULL) > 1700000000);
    T("usleep", usleep(1000));
    return 0;
}
