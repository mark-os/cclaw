#define _POSIX_C_SOURCE 200809L
#include "wake.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_pipe[2] = {-1, -1};

static void set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int wake_init(void) {
    if (pipe(g_pipe) != 0) return -1;
    fcntl(g_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(g_pipe[1], F_SETFD, FD_CLOEXEC);
    set_nonblock(g_pipe[0]);
    set_nonblock(g_pipe[1]);
    return 0;
}

int wake_fd(void) { return g_pipe[0]; }

int wake_session(int64_t session_id) {
    if (g_pipe[1] < 0) return -1;
    WakeMsg msg = { .session_id = session_id };
    ssize_t n = write(g_pipe[1], &msg, sizeof(msg));
    return (n == (ssize_t)sizeof(msg)) ? 0 : -1;
}

void wake_close(void) {
    if (g_pipe[0] >= 0) { close(g_pipe[0]); g_pipe[0] = -1; }
    if (g_pipe[1] >= 0) { close(g_pipe[1]); g_pipe[1] = -1; }
}

/* ── External FIFO ──────────────────────────────────────────────── */

char *wake_fifo_path(const char *db_path) {
    if (!db_path) return NULL;
    size_t len = strlen(db_path);
    char *path = malloc(len + 6);
    if (!path) return NULL;
    memcpy(path, db_path, len);
    if (len > 3 && strcmp(db_path + len - 3, ".db") == 0)
        strcpy(path + len - 3, ".pipe");
    else
        strcpy(path + len, ".pipe");
    return path;
}

int wake_fifo_open(const char *db_path) {
    char *path = wake_fifo_path(db_path);
    if (!path) return -1;
    unlink(path);
    if (mkfifo(path, 0600) != 0 && errno != EEXIST) { free(path); return -1; }
    /* O_RDWR (not O_RDONLY): a FIFO reader with no writer of its own sees
     * a perpetual EOF once any external writer has ever opened-and-closed
     * it, which poll() reports as endlessly "readable" — a busy-loop that
     * pins a CPU core and hammers the DB on every iteration. Holding our
     * own write end means the reader never observes a zero-writer state. */
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    free(path);
    return fd;
}

void wake_fifo_close(int fd, const char *db_path) {
    if (fd >= 0) close(fd);
    char *path = wake_fifo_path(db_path);
    if (path) { unlink(path); free(path); }
}

int wake_external(const char *db_path) {
    char *path = wake_fifo_path(db_path);
    if (!path) return -1;
    int fd = open(path, O_WRONLY | O_NONBLOCK);
    free(path);
    if (fd < 0) return -1;
    char c = 1;
    (void)write(fd, &c, 1);
    close(fd);
    return 0;
}
