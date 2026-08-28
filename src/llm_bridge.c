#define _POSIX_C_SOURCE 200809L
#include "llm_bridge.h"
#include "config_registry.h"
#include "db.h"
#include "llm_proc.h"
#include "log.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct LlmBridge {
    char sock_path[256];
    char agent_name[64];
    char source[64];
    char *db_path;
    int64_t session_id, iteration_id;
    int listen_fd;
    volatile int running;
    pthread_t thread;
    int thread_started;
    int calls;          /* served on this bridge (single accept thread) */
    int max_calls;      /* llm_max_calls_per_tool; <=0 = unlimited */
};

static int read_full(int fd, void *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n) {
    size_t put = 0;
    while (put < n) {
        ssize_t r = write(fd, (const char *)buf + put, n - put);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;
        }
        put += (size_t)r;
    }
    return 0;
}

static void send_json(int fd, const char *json) {
    uint32_t len = (uint32_t)strlen(json);
    unsigned char hdr[4] = { (unsigned char)len, (unsigned char)(len >> 8),
                             (unsigned char)(len >> 16), (unsigned char)(len >> 24) };
    if (write_full(fd, hdr, 4) == 0)
        write_full(fd, json, len);
}

static void serve_conn(LlmBridge *b, sqlite3 *db, int fd) {
    /* A stuck client must not wedge the bridge past the tool's own life. */
    struct timeval tv = { .tv_sec = 10 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    unsigned char hdr[4];
    if (read_full(fd, hdr, 4) != 0) return;
    uint32_t len = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                   ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    if (len == 0 || len > LLM_BRIDGE_REQ_MAX) {
        send_json(fd, "{\"ok\":false,\"error\":\"request too large\"}");
        return;
    }
    char *req = malloc((size_t)len + 1);
    if (!req) return;
    if (read_full(fd, req, len) != 0) { free(req); return; }
    req[len] = '\0';

    b->calls++;
    if (b->max_calls > 0 && b->calls > b->max_calls) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "{\"ok\":false,\"error\":\"LLM() call cap reached (%d per tool "
                 "call; config llm_max_calls_per_tool)\"}", b->max_calls);
        send_json(fd, msg);
        free(req);
        return;
    }

    char *resp = db ? llm_request(db, b->agent_name, req, b->source,
                                  b->session_id, b->iteration_id)
                    : NULL;
    send_json(fd, resp ? resp
                       : "{\"ok\":false,\"error\":\"LLM bridge has no database\"}");
    free(resp);
    free(req);
}

static void *bridge_thread(void *arg) {
    LlmBridge *b = arg;
    /* Own connection: the poll-loop thread's handle is not ours to share, and
     * a fresh WAL reader sees every write (the update-detector lesson). */
    sqlite3 *db = db_open(b->db_path);
    while (b->running) {
        struct pollfd p = { .fd = b->listen_fd, .events = POLLIN };
        int pr = poll(&p, 1, 300);
        if (pr <= 0) continue;
        int c = accept(b->listen_fd, NULL, NULL);
        if (c < 0) continue;
        serve_conn(b, db, c);
        close(c);
    }
    if (db) sqlite3_close(db);
    return NULL;
}

const char *llm_bridge_sock(const LlmBridge *b) {
    return b ? b->sock_path : NULL;
}

LlmBridge *llm_bridge_start(const char *agent_dir, const char *db_path,
                            const char *agent_name, const char *source,
                            int64_t session_id, int64_t iteration_id) {
    static int seq;   /* event-loop thread only — uniquifies per-call paths */
    if (!agent_dir || !agent_dir[0] || !db_path || !db_path[0]) return NULL;
    LlmBridge *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    int n = snprintf(b->sock_path, sizeof(b->sock_path), "%s/.llm.%d.%d.sock",
                     agent_dir, (int)getpid(), ++seq);
    if (n <= 0 || (size_t)n >= sizeof(b->sock_path) ||
        (size_t)n >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        free(b);
        return NULL;
    }
    snprintf(b->agent_name, sizeof(b->agent_name), "%s", agent_name ? agent_name : "");
    snprintf(b->source, sizeof(b->source), "%s", source ? source : "js");
    b->db_path = strdup(db_path);
    b->session_id = session_id;
    b->iteration_id = iteration_id;

    /* Cap read once at start on a scratch handle — config is effectively
     * static for the life of one tool call. */
    sqlite3 *cdb = db_open(db_path);
    if (cdb) {
        b->max_calls = config_get_int(cdb, "llm_max_calls_per_tool");
        sqlite3_close(cdb);
    }

    unlink(b->sock_path);
    b->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (b->listen_fd < 0) { free(b->db_path); free(b); return NULL; }
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    /* fits: n was checked against sun_path above */
    memcpy(addr.sun_path, b->sock_path, (size_t)n + 1);
    if (bind(b->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(b->listen_fd, 4) != 0) {
        LOG_WARN_("llm_bridge: bind %s failed: %s", b->sock_path, strerror(errno));
        close(b->listen_fd);
        free(b->db_path);
        free(b);
        return NULL;
    }
    b->running = 1;
    if (pthread_create(&b->thread, NULL, bridge_thread, b) != 0) {
        close(b->listen_fd);
        unlink(b->sock_path);
        free(b->db_path);
        free(b);
        return NULL;
    }
    b->thread_started = 1;
    return b;
}

void llm_bridge_stop(LlmBridge *b) {
    if (!b) return;
    b->running = 0;
    if (b->thread_started) pthread_join(b->thread, NULL);
    close(b->listen_fd);
    unlink(b->sock_path);
    free(b->db_path);
    free(b);
}
