// Send 512 KiB over a UNIX pipe using fork()
// Parent writes 512 KiB; child reads exactly 512 KiB and reports a checksum.

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>

#define MSG_SIZE (512 * 1024)   // 512 KiB

static int write_full(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t n = 0;
    while (n < len) {
        ssize_t w = write(fd, p + n, len - n);
        if (w < 0) { if (errno == EINTR) continue; perror("write"); return -1; }
        n += (size_t)w;
    }
    return 0;
}

static int read_full(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t n = 0;
    while (n < len) {
        ssize_t r = read(fd, p + n, len - n);
        if (r < 0) { if (errno == EINTR) continue; perror("read"); return -1; }
        if (r == 0) { fprintf(stderr, "EOF on pipe\n"); return -1; }
        n += (size_t)r;
    }
    return 0;
}

static uint32_t checksum32(const uint8_t *p, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i < n; ++i) s += p[i];
    return s;
}

int main(void) {
    int fds[2];
    if (pipe(fds) == -1) { perror("pipe"); return 1; }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    if (pid == 0) {
        // ---- Child: read 512 KiB ----
        close(fds[1]); // child only reads
        uint8_t *buf = (uint8_t *)malloc(MSG_SIZE);
        if (!buf) { perror("malloc"); _exit(2); }

        if (read_full(fds[0], buf, MSG_SIZE) != 0) {
            free(buf); close(fds[0]); _exit(3);
        }
        close(fds[0]);

        uint32_t sum = checksum32(buf, MSG_SIZE);
        printf("child: received %zu bytes, checksum32=0x%08x, "
               "first=%02X %02X %02X %02X  last=%02X %02X %02X %02X\n",
               (size_t)MSG_SIZE, sum,
               buf[0], buf[1], buf[2], buf[3],
               buf[MSG_SIZE-4], buf[MSG_SIZE-3], buf[MSG_SIZE-2], buf[MSG_SIZE-1]);
        free(buf);
        _exit(0);
    }

    // ---- Parent: write 512 KiB ----
    close(fds[0]); // parent only writes
    uint8_t *msg = (uint8_t *)malloc(MSG_SIZE);
    if (!msg) { perror("malloc"); close(fds[1]); return 1; }

    // Fill with a simple pattern so child can sanity-check
    for (size_t i = 0; i < MSG_SIZE; ++i) msg[i] = (uint8_t)(i & 0xFF);

    if (write_full(fds[1], msg, MSG_SIZE) != 0) { free(msg); close(fds[1]); return 1; }
    free(msg);
    close(fds[1]);

    int status = 0;
    waitpid(pid, &status, 0);
    return 0;
}
