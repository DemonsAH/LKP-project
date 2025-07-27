#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <unistd.h>

#define OUICHEFS_MAGIC 'O'
#define OUICHEFS_IOCTL_DUMP_BLOCK _IO(OUICHEFS_MAGIC, 0x01)

int main()
{
    int fd = open("/mnt/ouichefs/testfile.txt", O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    if (ioctl(fd, OUICHEFS_IOCTL_DUMP_BLOCK) < 0) {
        perror("ioctl");
    }

    close(fd);
    return 0;
}
