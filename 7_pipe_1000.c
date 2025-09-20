// Parent sends 4-byte messages to child 1000 times over a pipe.

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

#define MSG_SIZE 4
#define N_MSG    1001

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

int main(void) {
    int fds[2];
    if (pipe(fds) == -1) { perror("pipe"); return 1; }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    if (pid == 0) {
        // ---- Child: read N_MSG messages of 4 bytes each ----
        close(fds[1]);                // child only reads
        uint8_t buf[MSG_SIZE];
        for (int i = 0; i < N_MSG; ++i) {
            if (read_full(fds[0], buf, MSG_SIZE) != 0) { close(fds[0]); _exit(2); }

            // (Optional) verify sequence number:
            // uint32_t seq = (uint32_t)buf[0] |
            //                ((uint32_t)buf[1] << 8) |
            //                ((uint32_t)buf[2] << 16) |
            //                ((uint32_t)buf[3] << 24);
            // (void)seq;
        }
        close(fds[0]);
        printf("child: received %d messages of %d bytes each\n", N_MSG, MSG_SIZE);
        _exit(0);
    }

    // ---- Parent: send N_MSG messages ----
    close(fds[0]);                    // parent only writes
    uint8_t msg[MSG_SIZE];

    for (uint32_t i = 0; i < N_MSG; ++i) {
        // Example payload: 32-bit little-endian sequence number
        msg[0] = (uint8_t)(i & 0xFF);
        msg[1] = (uint8_t)((i >> 8) & 0xFF);
        msg[2] = (uint8_t)((i >> 16) & 0xFF);
        msg[3] = (uint8_t)((i >> 24) & 0xFF);

        if (write_full(fds[1], msg, MSG_SIZE) != 0) { close(fds[1]); return 1; }
    }

    close(fds[1]);
    int status = 0; waitpid(pid, &status, 0);
    printf("parent: sent %d messages\n", N_MSG);
    return 0;
}
