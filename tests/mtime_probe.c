#define _LARGEFILE64_SOURCE 1
#include <time.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <utime.h>
#include <sys/stat.h>
#include <sys/time.h>
int main(void) {
    int fd = open("/tmp/ms/mt.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644); write(fd, "x", 1); close(fd);
    struct stat st; stat("/tmp/ms/mt.txt", &st); printf("fresh mtime=%ld (now=%ld)\n", (long)st.st_mtime, (long)time(0));
    struct utimbuf ut = {1700000000, 1700000000}; printf("utime=%d\n", utime("/tmp/ms/mt.txt", &ut));
    stat("/tmp/ms/mt.txt", &st); printf("after utime mtime=%ld\n", (long)st.st_mtime);
    struct stat64 s6; stat64("/tmp/ms/mt.txt", &s6); printf("stat64 mtime=%lld\n", (long long)s6.st_mtime);
    struct timeval tv[2] = {{1600000000, 0}, {1600000000, 0}}; printf("utimes=%d\n", utimes("/tmp/ms/mt.txt", tv));
    stat("/tmp/ms/mt.txt", &st); printf("after utimes mtime=%ld\n", (long)st.st_mtime);
    { unsigned int kb[2] = {1500000000, 1500000000}; printf("raw utime=%ld\n", syscall(4030, "/tmp/ms/mt.txt", kb)); stat("/tmp/ms/mt.txt", &st); printf("after raw mtime=%ld\n", (long)st.st_mtime); }
    return 0;
}
