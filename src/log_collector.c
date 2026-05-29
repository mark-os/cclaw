/* T218/V75: Log collector process.
 * Receives fds via SCM_RIGHTS over unix socketpair.
 * Epoll-monitors received fds, reads lines, batch-inserts to journal.db.
 * Flush every 100ms or 64 lines. */
#define _GNU_SOURCE
#include "log_collector.h"
#include "db.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

/* ── Daemon-side state ─────────────────────────────────────────── */

static int g_sock = -1;       /* daemon's end of socketpair */
static pid_t g_collector_pid; /* collector child pid */

/* ── SCM_RIGHTS helpers ────────────────────────────────────────── */

/* Metadata sent alongside each fd */
typedef struct {
    pid_t pid;
    int64_t session_id;
    int stream;           /* 1=stdout, 2=stderr */
    char source[64];
} FdMeta;

static int send_fd_with_meta(int sock, int fd, const FdMeta *meta) {
    struct msghdr msg = {0};
    struct iovec iov;
    iov.iov_base = (void *)meta;
    iov.iov_len = sizeof(*meta);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    /* Ancillary data: one fd */
    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    return sendmsg(sock, &msg, 0) >= 0 ? 0 : -1;
}

static int recv_fd_with_meta(int sock, int *fd_out, FdMeta *meta) {
    struct msghdr msg = {0};
    struct iovec iov;
    iov.iov_base = meta;
    iov.iov_len = sizeof(*meta);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    char cmsgbuf[CMSG_SPACE(sizeof(int))];
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);

    ssize_t n = recvmsg(sock, &msg, 0);
    if (n <= 0) return -1;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS) return -1;
    memcpy(fd_out, CMSG_DATA(cmsg), sizeof(int));
    return 0;
}

/* ── Collector process internals ───────────────────────────────── */

#define MAX_FDS 128
#define BATCH_SIZE 64
#define LINE_BUF_SIZE 4096

typedef struct {
    int fd;
    FdMeta meta;
    char buf[LINE_BUF_SIZE];
    int buf_len;
} FdSlot;

static volatile sig_atomic_t g_running = 1;

static void collector_sigterm(int sig) {
    (void)sig;
    g_running = 0;
}

static void collector_main(int sock, const char *journal_path) {
    signal(SIGTERM, collector_sigterm);
    signal(SIGINT, collector_sigterm);

    sqlite3 *db = db_open_journal(journal_path);
    if (!db) _exit(1);

    int epfd = epoll_create1(0);
    if (epfd < 0) { sqlite3_close(db); _exit(1); }

    /* Monitor the control socket for new fds */
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = sock};
    epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &ev);

    /* Timer fd for 100ms flush */
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (tfd >= 0) {
        struct itimerspec its = {
            .it_interval = {.tv_sec = 0, .tv_nsec = 100000000},
            .it_value = {.tv_sec = 0, .tv_nsec = 100000000}
        };
        timerfd_settime(tfd, 0, &its, NULL);
        ev.events = EPOLLIN;
        ev.data.fd = tfd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, tfd, &ev);
    }

    FdSlot slots[MAX_FDS] = {{0}};
    int slot_count = 0;

    /* Batch buffer */
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO log(source, pid, session_id, stream, line) VALUES(?,?,?,?,?)",
        -1, &stmt, NULL);

    int pending = 0;
    int in_txn = 0;

    while (g_running) {
        struct epoll_event events[32];
        int nev = epoll_wait(epfd, events, 32, 200);
        if (nev < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < nev; i++) {
            int efd = events[i].data.fd;

            /* New fd from daemon */
            if (efd == sock) {
                int new_fd = -1;
                FdMeta meta;
                if (recv_fd_with_meta(sock, &new_fd, &meta) == 0 && slot_count < MAX_FDS) {
                    /* Set non-blocking */
                    int flags = fcntl(new_fd, F_GETFL);
                    fcntl(new_fd, F_SETFL, flags | O_NONBLOCK);

                    slots[slot_count].fd = new_fd;
                    slots[slot_count].meta = meta;
                    slots[slot_count].buf_len = 0;

                    ev.events = EPOLLIN;
                    ev.data.fd = new_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, new_fd, &ev);
                    slot_count++;
                }
                continue;
            }

            /* Timer tick — flush */
            if (efd == tfd) {
                uint64_t exp;
                (void)read(tfd, &exp, sizeof(exp));
                if (in_txn && pending > 0) {
                    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
                    in_txn = 0;
                    pending = 0;
                }
                continue;
            }

            /* Data from a monitored fd */
            FdSlot *slot = NULL;
            for (int s = 0; s < slot_count; s++) {
                if (slots[s].fd == efd) { slot = &slots[s]; break; }
            }
            if (!slot) continue;

            char tmp[4096];
            ssize_t nr = read(efd, tmp, sizeof(tmp));
            if (nr <= 0) {
                /* EOF or error — flush remaining buffer, remove slot */
                if (slot->buf_len > 0 && stmt) {
                    if (!in_txn) {
                        sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
                        in_txn = 1;
                    }
                    slot->buf[slot->buf_len] = '\0';
                    sqlite3_bind_text(stmt, 1, slot->meta.source, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int(stmt, 2, slot->meta.pid);
                    sqlite3_bind_int64(stmt, 3, slot->meta.session_id);
                    sqlite3_bind_int(stmt, 4, slot->meta.stream);
                    sqlite3_bind_text(stmt, 5, slot->buf, slot->buf_len, SQLITE_TRANSIENT);
                    sqlite3_step(stmt);
                    sqlite3_reset(stmt);
                    pending++;
                }
                epoll_ctl(epfd, EPOLL_CTL_DEL, efd, NULL);
                close(efd);
                /* Compact slot array */
                int idx = (int)(slot - slots);
                slots[idx] = slots[--slot_count];
                continue;
            }

            /* Line-buffer: split on newlines, insert complete lines */
            for (ssize_t j = 0; j < nr; j++) {
                if (tmp[j] == '\n' || slot->buf_len >= LINE_BUF_SIZE - 1) {
                    slot->buf[slot->buf_len] = '\0';
                    if (slot->buf_len > 0 && stmt) {
                        if (!in_txn) {
                            sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
                            in_txn = 1;
                        }
                        sqlite3_bind_text(stmt, 1, slot->meta.source, -1, SQLITE_TRANSIENT);
                        sqlite3_bind_int(stmt, 2, slot->meta.pid);
                        sqlite3_bind_int64(stmt, 3, slot->meta.session_id);
                        sqlite3_bind_int(stmt, 4, slot->meta.stream);
                        sqlite3_bind_text(stmt, 5, slot->buf, slot->buf_len, SQLITE_TRANSIENT);
                        sqlite3_step(stmt);
                        sqlite3_reset(stmt);
                        pending++;
                    }
                    slot->buf_len = 0;

                    /* Flush at batch size */
                    if (pending >= BATCH_SIZE && in_txn) {
                        sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
                        in_txn = 0;
                        pending = 0;
                    }
                } else {
                    slot->buf[slot->buf_len++] = tmp[j];
                }
            }
        }
    }

    /* Final flush */
    if (in_txn) sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    sqlite3_finalize(stmt);

    /* Close all monitored fds */
    for (int s = 0; s < slot_count; s++) close(slots[s].fd);
    close(epfd);
    if (tfd >= 0) close(tfd);
    close(sock);
    sqlite3_close(db);
    _exit(0);
}

/* ── Public API (daemon side) ──────────────────────────────────── */

int log_collector_start(const char *journal_path) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(sv[0]);
        close(sv[1]);
        return -1;
    }
    if (pid == 0) {
        /* Child: collector process */
        close(sv[0]); /* daemon's end */
        collector_main(sv[1], journal_path);
        _exit(0); /* unreachable */
    }

    /* Parent: daemon */
    close(sv[1]); /* collector's end */
    g_sock = sv[0];
    g_collector_pid = pid;
    return 0;
}

int log_collector_send_fd(int fd, const char *source, pid_t pid,
                          int64_t session_id, int stream) {
    if (g_sock < 0) return -1;
    FdMeta meta = {.pid = pid, .session_id = session_id, .stream = stream};
    if (source) {
        strncpy(meta.source, source, sizeof(meta.source) - 1);
        meta.source[sizeof(meta.source) - 1] = '\0';
    }
    return send_fd_with_meta(g_sock, fd, &meta);
}

void log_collector_stop(void) {
    if (g_collector_pid > 0) {
        kill(g_collector_pid, SIGTERM);
        waitpid(g_collector_pid, NULL, 0);
        g_collector_pid = 0;
    }
    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
}

pid_t log_collector_pid(void) {
    return g_collector_pid;
}
