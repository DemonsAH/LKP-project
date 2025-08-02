#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define SMALL_CONTENT "hello world, this is <128 bytes.\n"
#define LARGE_CONTENT_SIZE 200

int main()
{
    const char *filename = "/mnt/ouichefs/testfile_8.txt";

    // Step 1: Create new file
    int fd = open(filename, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    // Step 2: Write small content
    if (write(fd, SMALL_CONTENT, strlen(SMALL_CONTENT)) < 0) {
        perror("write small");
        close(fd);
        return 1;
    }

    printf("✅ Wrote small file successfully.\n");

    // Step 3: Write large content (>128 bytes)
    char large_buf[LARGE_CONTENT_SIZE];
    memset(large_buf, 'X', LARGE_CONTENT_SIZE);
    if (write(fd, large_buf, LARGE_CONTENT_SIZE) < 0) {
        perror("write large");
        close(fd);
        return 1;
    }

    printf("✅ Wrote large content, file should now use block storage.\n");

    close(fd);

    // Step 4: Re-open and read
    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        perror("re-open");
        return 1;
    }

    char read_buf[300];
    int n = read(fd, read_buf, sizeof(read_buf) - 1);
    if (n < 0) {
        perror("read");
        close(fd);
        return 1;
    }

    read_buf[n] = '\0';
    printf("✅ Read back %d bytes. First 50 bytes:\n", n);
    fwrite(read_buf, 1, 50, stdout);
    printf("\n");

    close(fd);

    // Step 5: Stat the file
    struct stat st;
    if (stat(filename, &st) == 0) {
        printf("✅ File size reported by stat: %ld bytes\n", st.st_size);
    } else {
        perror("stat");
    }

    return 0;
}
