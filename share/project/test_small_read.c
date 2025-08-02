#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define TESTFILE "/mnt/ouichefs/test_1_9.txt"
#define DATA "Hello from OuicheFS slice read!\n"

int main() {
    int fd;
    char buf[128] = {0};

    // Step 1: Create and write small file
    fd = open(TESTFILE, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open for write");
        return 1;
    }

    ssize_t written = write(fd, DATA, strlen(DATA));
    if (written < 0) {
        perror("write");
        close(fd);
        return 1;
    }
    close(fd);

    // Step 2: Reopen for reading
    fd = open(TESTFILE, O_RDONLY);
    if (fd < 0) {
        perror("open for read");
        return 1;
    }

    ssize_t read_bytes = read(fd, buf, sizeof(buf) - 1);
    if (read_bytes < 0) {
        perror("read");
        close(fd);
        return 1;
    }

    buf[read_bytes] = '\0';  // Null-terminate buffer

    // Step 3: Compare
    printf("Read content: \"%s\"\n", buf);
    if (strcmp(buf, DATA) == 0) {
        printf("✅ Slice read test passed.\n");
    } else {
        printf("❌ Mismatch!\nExpected: \"%s\"\n", DATA);
    }

    close(fd);
    return 0;
}
