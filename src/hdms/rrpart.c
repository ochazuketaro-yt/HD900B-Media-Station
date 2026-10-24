/* rrpart: re-read the partition table of a block device (BLKRRPART). usage: rrpart /dev/block/sdb */
#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#define BLKRRPART 0x2000125f  /* MIPS: _IO(0x12,95) has the IOC_VOID bit 0x20000000 */
int main(int c, char **v) {
    if (c < 2) return 2;
    int fd = open(v[1], O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    int r = ioctl(fd, BLKRRPART, 0);
    if (r) perror("BLKRRPART");
    close(fd);
    return r ? 1 : 0;
}
