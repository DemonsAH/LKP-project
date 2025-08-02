#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>

#define PATH "/mnt/ouichefs/test_multislice.txt"
#define WRITE_SIZE 384  // 需要 3 slices (128 x 3)

int main() {
    char write_buf[WRITE_SIZE];
    memset(write_buf, 'M', sizeof(write_buf));

    // Step 1: 创建并写入文件
    int fd = open(PATH, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open for write");
        return 1;
    }

    ssize_t written = write(fd, write_buf, WRITE_SIZE);
    if (written != WRITE_SIZE) {
        perror("write");
        close(fd);
        return 1;
    }
    close(fd);
    printf("✔ Wrote %d bytes across multiple slices.\n", WRITE_SIZE);

    // Step 2: 读取文件内容验证
    fd = open(PATH, O_RDONLY);
    if (fd < 0) {
        perror("open for read");
        return 1;
    }

    char read_buf[WRITE_SIZE + 1];
    memset(read_buf, 0, sizeof(read_buf));
    ssize_t read_bytes = read(fd, read_buf, WRITE_SIZE);
    close(fd);

    if (read_bytes != WRITE_SIZE) {
        fprintf(stderr, "❌ Read size mismatch: got %ld, expected %d\n", read_bytes, WRITE_SIZE);
        return 1;
    }

    // 验证内容一致
    if (memcmp(read_buf, write_buf, WRITE_SIZE) != 0) {
        fprintf(stderr, "❌ File content mismatch.\n");
        return 1;
    }

    printf("✔ Read back %ld bytes. First 50 bytes:\n", read_bytes);
    write(1, read_buf, 50);
    printf("\n");

    // Step 3: stat 检查文件大小
    struct stat st;
    if (stat(PATH, &st) < 0) {
        perror("stat");
        return 1;
    }

    if (st.st_size != WRITE_SIZE) {
        fprintf(stderr, "❌ Stat reports wrong file size: %ld\n", st.st_size);
        return 1;
    }

    printf("✔ File size reported by stat: %ld bytes\n", st.st_size);
    printf("✅ Test passed: multi-slice file written and read correctly.\n");
    return 0;
}
