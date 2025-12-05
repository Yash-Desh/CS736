// Send 4 bytes over a UNIX pipe using fork()
// Parent writes 4 bytes; child reads 4 bytes (ordering & delivery guaranteed)

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>

static int write_full(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t n = 0;
    while (n < len) {
        ssize_t w = write(fd, p + n, len - n);
        if (w < 0) {
            if (errno == EINTR) continue;
            perror("write");
            return -1;
        }
        n += (size_t)w;
    }
    return 0;
}

static int read_full(int fd, void *buf, size_t len) {
    uint8_t *p = (uint8_t *)buf;
    size_t n = 0;
    while (n < len) {
        ssize_t r = read(fd, p + n, len - n);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("read");
            return -1;
        }
        if (r == 0) {  // pipe closed early
            fprintf(stderr, "EOF on pipe\n");
            return -1;
        }
        n += (size_t)r;
    }
    return 0;
}

int main(void) {
    int fds[2];
    if (pipe(fds) == -1) {
        perror("pipe");
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        // ---- Child: read 4 bytes ----
        close(fds[1]); // close write end
        uint8_t buf[4];
        if (read_full(fds[0], buf, 4) != 0) {
            close(fds[0]);
            _exit(2);
        }
        close(fds[0]);

        printf("child: got 4 bytes: %02X %02X %02X %02X  (\"%c%c%c%c\")\n",
               buf[0], buf[1], buf[2], buf[3],
               (buf[0] >= 32 && buf[0] < 127) ? buf[0] : '.',
               (buf[1] >= 32 && buf[1] < 127) ? buf[1] : '.',
               (buf[2] >= 32 && buf[2] < 127) ? buf[2] : '.',
               (buf[3] >= 32 && buf[3] < 127) ? buf[3] : '.');
        _exit(0);
    }

    // ---- Parent: write 4 bytes ----
    close(fds[0]); // close read end
    uint8_t msg[4] = { 'P', 'I', 'N', 'G' }; // exactly 4 bytes
    if (write_full(fds[1], msg, 4) != 0) {
        close(fds[1]);
        return 1;
    }
    close(fds[1]);

    int status = 0;
    waitpid(pid, &status, 0);
    return 0;
}
