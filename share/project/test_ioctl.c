#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <unistd.h>

#define OUICHEFS_IOCTL_MAGIC 'O'
#define OUICHEFS_IOCTL_DUMP_BLOCK _IO(OUICHEFS_IOCTL_MAGIC, 0x01)

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <ouichefs-small-file>\n", argv[0]);
        return 1;
    }

    int fd = open(argv[1], O_RDONLY);
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
