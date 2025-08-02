#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

// 用法: ./write_small_file /mnt/ouichefs/testfile.txt "Hello world"

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <file> <string-to-write>\n", argv[0]);
        return 1;
    }

    const char *filepath = argv[1];
    const char *text = argv[2];
    size_t len = strlen(text);

    if (len > 128) {
        fprintf(stderr, "Error: Text must be ≤ 128 characters for slice-based storage.\n");
        return 1;
    }

    int fd = open(filepath, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    ssize_t written = write(fd, text, len);
    if (written < 0) {
        perror("write");
        close(fd);
        return 1;
    }

    printf("Wrote %zd bytes to %s\n", written, filepath);

    close(fd);
    return 0;
}