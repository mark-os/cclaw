#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "daemon.h"
#include "db.h"
#include "shutdown.h"
#include "agent.h"
#include "agent_config.h"
#include "telegram.h"
#include "tools.h"
#include "tool_shell.h"
#include "tool_file.h"
#include "tool_js.h"
#include "tool_web_fetch.h"
#include "tool_db_query.h"
#include "tool_agent.h"
#include "tool_cron.h"
#include "landlock.h"
#include "config.h"
#include "context.h"
#include "agent_exit.h"
#include <cJSON.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── Signal pipe (T82, T200, V25) ───────────────────────────────── */

static int g_signal_pipe[2] = {-1, -1};
static char g_self_path[4096] = "";

void daemon_set_self_path(const char *path) {
    snprintf(g_self_path, sizeof(g_self_path), "%s", path);
}

static void set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* T200: Signal message carries session_id + agent_name */
typedef struct {
    int64_t session_id;
    char agent_name[64];
} SignalMsg;

int daemon_signal_init(void) {
    if (pipe(g_signal_pipe) != 0) return -1;
    set_nonblock(g_signal_pipe[0]);
    set_nonblock(g_signal_pipe[1]);
    return 0;
}

int daemon_signal_session(int64_t session_id) {
    if (g_signal_pipe[1] < 0) return -1;
    /* Legacy: write just session_id for backward compat with callers that
     * don't know agent_name. Daemon will resolve from tg_chat_sessions or
     * agents table. */
    SignalMsg msg = {0};
    msg.session_id = session_id;
    ssize_t n = write(g_signal_pipe[1], &msg, sizeof(msg));
    return (n == (ssize_t)sizeof(msg)) ? 0 : -1;
}

/* T200: Signal with agent_name (preferred — avoids DB lookup in daemon loop) */
int daemon_signal_session_agent(int64_t session_id, const char *agent_name) {
    if (g_signal_pipe[1] < 0) return -1;
    SignalMsg msg = {0};
    msg.session_id = session_id;
    if (agent_name)
        snprintf(msg.agent_name, sizeof(msg.agent_name), "%s", agent_name);
    ssize_t n = write(g_signal_pipe[1], &msg, sizeof(msg));
    return (n == (ssize_t)sizeof(msg)) ? 0 : -1;
}

/* Signal daemon from external process via named FIFO. */
int daemon_signal_external(const char *db_path, int64_t session_id) {
    char *path = daemon_pipe_path(db_path);
    if (!path) return -1;
    int fd = open(path, O_WRONLY | O_NONBLOCK);
    free(path);
    if (fd < 0) return -1;
    SignalMsg msg = {0};
    msg.session_id = session_id;
    ssize_t n = write(fd, &msg, sizeof(msg));
    close(fd);
    return (n == (ssize_t)sizeof(msg)) ? 0 : -1;
}

void daemon_signal_close(void) {
    if (g_signal_pipe[0] >= 0) { close(g_signal_pipe[0]); g_signal_pipe[0] = -1; }
    if (g_signal_pipe[1] >= 0) { close(g_signal_pipe[1]); g_signal_pipe[1] = -1; }
}

int daemon_signal_fd(void) {
    return g_signal_pipe[0];
}

/* ── Named FIFO for daemon detection ────────────────────────────── */

#include <sys/stat.h>

char *daemon_pipe_path(const char *db_path) {
    if (!db_path) return NULL;
    size_t len = strlen(db_path);
    char *path = malloc(len + 6); /* .pipe\0 */
    if (!path) return NULL;
    memcpy(path, db_path, len);
    /* Replace .db suffix or append .pipe */
    if (len > 3 && strcmp(db_path + len - 3, ".db") == 0)
        strcpy(path + len - 3, ".pipe");
    else
        strcpy(path + len, ".pipe");
    return path;
}

int daemon_is_running(const char *db_path) {
    char *path = daemon_pipe_path(db_path);
    if (!path) return -1;
    int fd = open(path, O_WRONLY | O_NONBLOCK);
    free(path);
    if (fd >= 0) {
        close(fd);
        return 1;
    }
    return 0; /* ENXIO = no reader, ENOENT = no fifo */
}

int daemon_fifo_open(const char *db_path) {
    char *path = daemon_pipe_path(db_path);
    if (!path) return -1;
    unlink(path); /* remove stale */
    if (mkfifo(path, 0600) != 0 && errno != EEXIST) {
        free(path);
        return -1;
    }
    /* Open read end non-blocking (won't block waiting for writer) */
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    free(path);
    return fd;
}

void daemon_fifo_close(int fd, const char *db_path) {
    if (fd >= 0) close(fd);
    char *path = daemon_pipe_path(db_path);
    if (path) { unlink(path); free(path); }
}

/* ── SIGCHLD self-pipe ──────────────────────────────────────────── */

static int g_chld_pipe[2] = {-1, -1};

static void sigchld_handler(int sig) {
    (void)sig;
    char c = 1;
    (void)write(g_chld_pipe[1], &c, 1);
}

static int sigchld_pipe_init(void) {
    if (pipe(g_chld_pipe) != 0) return -1;
    set_nonblock(g_chld_pipe[0]);
    set_nonblock(g_chld_pipe[1]);

    struct sigaction sa = {0};
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    return 0;
}

static void sigchld_pipe_close(void) {
    if (g_chld_pipe[0] >= 0) { close(g_chld_pipe[0]); g_chld_pipe[0] = -1; }
    if (g_chld_pipe[1] >= 0) { close(g_chld_pipe[1]); g_chld_pipe[1] = -1; }
}

/* ── V71/T194: Token rate limiting (rolling 1h window, in-memory) ── */

#include <time.h>

#define TOKEN_BUCKET_COUNT 128

static struct {
    time_t timestamp;
    int tokens;
} g_token_buckets[TOKEN_BUCKET_COUNT];
static int g_token_bucket_head = 0;

void daemon_token_usage_add(int tokens) {
    if (tokens <= 0) return;
    time_t now = time(NULL);
    g_token_buckets[g_token_bucket_head].timestamp = now;
    g_token_buckets[g_token_bucket_head].tokens = tokens;
    g_token_bucket_head = (g_token_bucket_head + 1) % TOKEN_BUCKET_COUNT;
}

int daemon_token_usage_hourly(void) {
    time_t cutoff = time(NULL) - 3600;
    int total = 0;
    for (int i = 0; i < TOKEN_BUCKET_COUNT; i++) {
        if (g_token_buckets[i].timestamp >= cutoff)
            total += g_token_buckets[i].tokens;
    }
    return total;
}

void daemon_token_usage_reset(void) {
    memset(g_token_buckets, 0, sizeof(g_token_buckets));
    g_token_bucket_head = 0;
}

/* ── T199/V76: Daemon writes inbox to agent DB ─────────────────── */

static char *agent_db_path(const char *agent_name) {
    if (!agent_name) return NULL;
    char buf[1024];
    snprintf(buf, sizeof(buf), "agents/%s/agent.db", agent_name);
    return strdup(buf);
}

int64_t daemon_inbox_insert(const char *agent_name, int64_t session_id,
                            const char *source, const char *payload) {
    char *path = agent_db_path(agent_name);
    if (!path) return -1;
    sqlite3 *adb = db_open_agent(path);
    free(path);
    if (!adb) return -1;
    int64_t id = inbox_insert(adb, session_id, source, payload);
    db_close(adb);
    return id;
}

int daemon_inbox_count(const char *agent_name, int64_t session_id) {
    char *path = agent_db_path(agent_name);
    if (!path) return -1;
    sqlite3 *adb = db_open_agent(path);
    free(path);
    if (!adb) return -1;
    int count = inbox_count(adb, session_id);
    db_close(adb);
    return count;
}

/* T203/V78: Resolve approval — UPDATE PENDING entry, transition state, signal. */
int daemon_resolve_approval(const char *agent_name, int64_t session_id,
                            const char *tool_call_id, const char *result) {
    if (!agent_name || !tool_call_id || !result) return -1;
    char *path = agent_db_path(agent_name);
    if (!path) return -1;
    sqlite3 *adb = db_open_agent(path);
    free(path);
    if (!adb) return -1;

    /* Find PENDING entry by tool_call_id */
    const char *sql = "SELECT id FROM entries WHERE session_id=?"
                      " AND tool_call_id=? AND content='PENDING' LIMIT 1;";
    sqlite3_stmt *stmt;
    int rc = -1;
    if (sqlite3_prepare_v2(adb, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, session_id);
        sqlite3_bind_text(stmt, 2, tool_call_id, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int64_t eid = sqlite3_column_int64(stmt, 0);
            sqlite3_finalize(stmt);
            /* UPDATE content */
            const char *usql = "UPDATE entries SET content=? WHERE id=? AND content='PENDING';";
            sqlite3_stmt *us;
            if (sqlite3_prepare_v2(adb, usql, -1, &us, NULL) == SQLITE_OK) {
                sqlite3_bind_text(us, 1, result, -1, SQLITE_STATIC);
                sqlite3_bind_int64(us, 2, eid);
                if (sqlite3_step(us) == SQLITE_DONE) rc = 0;
                sqlite3_finalize(us);
            }
        } else {
            sqlite3_finalize(stmt);
        }
    }

    /* Transition waiting→idle */
    if (rc == 0)
        session_set_state(adb, session_id, "idle");

    db_close(adb);

    /* Signal daemon to re-fork */
    if (rc == 0)
        daemon_signal_session_agent(session_id, agent_name);

    return rc;
}

/* ── Child tracking (T87, T200, V24) ────────────────────────────── */

typedef struct {
    pid_t pid;
    int64_t session_id;
    char agent_name[64];
} ChildSlot;

static ChildSlot g_children[DAEMON_MAX_CHILDREN];
static int g_child_count = 0;

static int child_add(pid_t pid, int64_t session_id, const char *agent_name) {
    if (g_child_count >= DAEMON_MAX_CHILDREN) return -1;
    g_children[g_child_count].pid = pid;
    g_children[g_child_count].session_id = session_id;
    if (agent_name)
        snprintf(g_children[g_child_count].agent_name, 64, "%s", agent_name);
    else
        g_children[g_child_count].agent_name[0] = '\0';
    g_child_count++;
    return 0;
}

static int child_remove(pid_t pid, int64_t *out_session_id, char *out_agent_name) {
    for (int i = 0; i < g_child_count; i++) {
        if (g_children[i].pid == pid) {
            *out_session_id = g_children[i].session_id;
            if (out_agent_name)
                snprintf(out_agent_name, 64, "%s", g_children[i].agent_name);
            g_children[i] = g_children[g_child_count - 1];
            g_child_count--;
            return 0;
        }
    }
    return -1;
}

/* V24: Check if session already has an active child */
static int child_has_session(int64_t session_id) {
    for (int i = 0; i < g_child_count; i++) {
        if (g_children[i].session_id == session_id) return 1;
    }
    return 0;
}

int64_t daemon_child_session(pid_t pid) {
    for (int i = 0; i < g_child_count; i++) {
        if (g_children[i].pid == pid) return g_children[i].session_id;
    }
    return -1;
}

/* T200/V73: Count active children for a parent session (in-memory) */
static int child_count_for_parent(sqlite3 *db, int64_t parent_session_id) {
    (void)db;
    int count = 0;
    for (int i = 0; i < g_child_count; i++) {
        /* Check if child's session has this parent */
        if (g_children[i].agent_name[0]) {
            char *path = agent_db_path(g_children[i].agent_name);
            if (!path) continue;
            sqlite3 *adb = db_open_agent(path);
            free(path);
            if (!adb) continue;
            const char *sql = "SELECT parent_session_id FROM sessions WHERE id=?;";
            sqlite3_stmt *stmt;
            if (sqlite3_prepare_v2(adb, sql, -1, &stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(stmt, 1, g_children[i].session_id);
                if (sqlite3_step(stmt) == SQLITE_ROW &&
                    sqlite3_column_int64(stmt, 0) == parent_session_id)
                    count++;
                sqlite3_finalize(stmt);
            }
            db_close(adb);
        }
    }
    return count;
}

/* T200/V73: Open agent DB, set session state. Returns 0 on success. */
static int daemon_agent_set_state(const char *agent_name, int64_t session_id, const char *state) {
    if (!agent_name || !agent_name[0]) return -1;
    char *path = agent_db_path(agent_name);
    if (!path) return -1;
    sqlite3 *adb = db_open_agent(path);
    free(path);
    if (!adb) return -1;
    int rc = session_set_state(adb, session_id, state);
    db_close(adb);
    return rc;
}

/* T200/V73: Open agent DB, get session state. Caller frees. */
static char *daemon_agent_get_state(const char *agent_name, int64_t session_id) {
    if (!agent_name || !agent_name[0]) return NULL;
    char *path = agent_db_path(agent_name);
    if (!path) return NULL;
    sqlite3 *adb = db_open_agent(path);
    free(path);
    if (!adb) return NULL;
    const char *sql = "SELECT state FROM sessions WHERE id=?;";
    sqlite3_stmt *stmt;
    char *result = NULL;
    if (sqlite3_prepare_v2(adb, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, session_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *val = (const char *)sqlite3_column_text(stmt, 0);
            if (val) result = strdup(val);
        }
        sqlite3_finalize(stmt);
    }
    db_close(adb);
    return result;
}

/* ── DB helpers (T86) ───────────────────────────────────────────── */

int session_set_last_route(sqlite3 *db, int64_t session_id, const char *route) {
    const char *sql = "UPDATE sessions SET last_route=?, updated_at=unixepoch() WHERE id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, route, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, session_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

char *session_get_last_route(sqlite3 *db, int64_t session_id) {
    const char *sql = "SELECT last_route FROM sessions WHERE id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int64(stmt, 1, session_id);
    char *result = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *val = (const char *)sqlite3_column_text(stmt, 0);
        if (val) result = strdup(val);
    }
    sqlite3_finalize(stmt);
    return result;
}

/* ── Agent process entry (T83, V21, V23, V34) ──────────────────── */

/* V67/T188: Decrypt secrets from DB and inject as env vars before exec.
 * Daemon is sole holder of .cclaw_key — agents read injected env vars. */
static void inject_secrets_for_child(sqlite3 *db) {
    char *api_key = db_kv_get_secret(db, "provider.api_key");
    if (api_key) {
        setenv("CCLAW_INJECTED_API_KEY", api_key, 1);
        free(api_key);
    }
    char *tg_token = db_kv_get_secret(db, "telegram_token");
    if (tg_token) {
        setenv("CCLAW_INJECTED_TELEGRAM_TOKEN", tg_token, 1);
        free(tg_token);
    }
}

static int fork_agent(const Config *cfg, sqlite3 *db, int64_t session_id,
                     const char *agent_name) {
    /* V24: only fork if session already has active child */
    if (child_has_session(session_id)) return -1;

    /* Resolve agent_name if not provided */
    char *resolved_name = NULL;
    if (!agent_name || !agent_name[0]) {
        resolved_name = session_get_agent_name(db, session_id);
        agent_name = resolved_name;
    }
    if (!agent_name || !agent_name[0]) {
        free(resolved_name);
        return -1;
    }

    /* V71/T194: reject fork if token rate limit exceeded */
    if (cfg->token_rate_limit > 0 && daemon_token_usage_hourly() >= cfg->token_rate_limit) {
        daemon_inbox_insert(agent_name, session_id, "system", "error: token rate limit exceeded");
        free(resolved_name);
        return -1;
    }

    /* T200/V73: Read state from agent DB — only fork if idle */
    char *state = daemon_agent_get_state(agent_name, session_id);
    if (state && strcmp(state, "idle") != 0) {
        free(state);
        free(resolved_name);
        return -1;
    }
    free(state);

    /* T200: Mark session running in agent DB */
    if (daemon_agent_set_state(agent_name, session_id, "running") != 0) {
        free(resolved_name);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        daemon_agent_set_state(agent_name, session_id, "idle");
        free(resolved_name);
        return -1;
    }
    if (pid == 0) {
        /* V67/T188: inject decrypted secrets as env vars for child */
        inject_secrets_for_child(db);

        /* V73/V74,T198: inject agent identity + DB path + config as env vars */
        /* agent_name already known from caller */
        if (agent_name) {
            setenv("CCLAW_AGENT_NAME", agent_name, 1);

            /* Build agent DB path: agents/<name>/agent.db */
            char agent_db[1024];
            snprintf(agent_db, sizeof(agent_db), "agents/%s/agent.db", agent_name);
            setenv("CCLAW_AGENT_DB", agent_db, 1);

            /* Load per-agent config from daemon.db, inject as env vars */
            AgentConfig *ac = agent_config_load_db(db, agent_name);
            if (ac) {
                if (ac->workspace)
                    setenv("CCLAW_WORKSPACE", ac->workspace, 1);
                else {
                    char ws[1024];
                    snprintf(ws, sizeof(ws), "agents/%s/workspace", agent_name);
                    setenv("CCLAW_WORKSPACE", ws, 1);
                }
                if (ac->model)
                    setenv("CCLAW_MODEL", ac->model, 1);
                if (ac->max_iterations > 0) {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%d", ac->max_iterations);
                    setenv("CCLAW_MAX_ITERATIONS", buf, 1);
                }
                if (ac->allowed_hosts_count > 0) {
                    /* Comma-separated list */
                    size_t len = 0;
                    for (size_t i = 0; i < ac->allowed_hosts_count; i++)
                        len += strlen(ac->allowed_hosts[i]) + 1;
                    char *hosts = malloc(len);
                    if (hosts) {
                        hosts[0] = '\0';
                        for (size_t i = 0; i < ac->allowed_hosts_count; i++) {
                            if (i > 0) strcat(hosts, ",");
                            strcat(hosts, ac->allowed_hosts[i]);
                        }
                        setenv("CCLAW_ALLOWED_HOSTS", hosts, 1);
                        free(hosts);
                    }
                }
                agent_config_free(ac);
            } else {
                /* No per-agent config — use defaults */
                char ws[1024];
                snprintf(ws, sizeof(ws), "agents/%s/workspace", agent_name);
                setenv("CCLAW_WORKSPACE", ws, 1);
            }
        }

        /* Always inject global config as baseline (per-agent overrides above win) */
        {
            char buf[32];
            if (cfg->provider.base_url && !getenv("CCLAW_PROVIDER"))
                setenv("CCLAW_PROVIDER", cfg->provider.base_url, 0);
            if (cfg->provider.model && !getenv("CCLAW_MODEL"))
                setenv("CCLAW_MODEL", cfg->provider.model, 0);
            snprintf(buf, sizeof(buf), "%d", cfg->provider.max_tokens);
            setenv("CCLAW_MAX_TOKENS", buf, 0);
            snprintf(buf, sizeof(buf), "%d", cfg->provider.context_window);
            setenv("CCLAW_CONTEXT_WINDOW", buf, 0);
            if (cfg->max_iterations > 0) {
                snprintf(buf, sizeof(buf), "%d", cfg->max_iterations);
                setenv("CCLAW_MAX_ITERATIONS", buf, 0);
            }
            if (cfg->shell_timeout > 0) {
                snprintf(buf, sizeof(buf), "%d", cfg->shell_timeout);
                setenv("CCLAW_SHELL_TIMEOUT", buf, 0);
            }
        }

        /* Child: exec agent process */
        char sid_arg[64];
        snprintf(sid_arg, sizeof(sid_arg), "--session-id=%lld", (long long)session_id);
        execl(g_self_path, g_self_path, "--agent", sid_arg, (char *)NULL);
        _exit(127);
    }

    /* Parent: track child (T200: includes agent_name) */
    child_add(pid, session_id, agent_name);
    free(resolved_name);
    return 0;
}

/* ── Response delivery (T85, V26) ──────────────────────────────── */

/* T200/V73: deliver_response reads from agent DB */
static void deliver_response(const Config *cfg, sqlite3 *db,
                             const char *agent_name, int64_t session_id) {
    /* Open agent DB to read response */
    char *path = agent_db_path(agent_name);
    if (!path) return;
    sqlite3 *adb = db_open_agent(path);
    free(path);
    if (!adb) return;

    /* Get last assistant message from session branch */
    int count = 0;
    Entry *entries = session_get_branch(adb, session_id, &count);
    char *route = session_get_last_route(adb, session_id);
    db_close(adb);

    const char *reply = NULL;
    if (entries) {
        for (int i = count - 1; i >= 0; i--) {
            if (entries[i].message.role == ROLE_ASSISTANT && entries[i].message.content) {
                reply = entries[i].message.content;
                break;
            }
        }
    }
    if (!reply) {
        entry_branch_free(entries, count);
        free(route);
        return;
    }

    /* V42/T110: Suppress HEARTBEAT_OK sentinel — never deliver to channel */
    if (strcmp(reply, "HEARTBEAT_OK") == 0) {
        entry_branch_free(entries, count);
        free(route);
        return;
    }

    /* V44/T113: Suppress [NO_REPLY] — agent decided not to respond (e.g. group irrelevance) */
    if (strstr(reply, "[NO_REPLY]") != NULL) {
        entry_branch_free(entries, count);
        free(route);
        return;
    }

    if (route && strncmp(route, "telegram", 8) == 0) {
        /* Route format: "telegram" — need chat_id from tg_chat_sessions (daemon.db) */
        const char *sql = "SELECT chat_id FROM tg_chat_sessions WHERE session_id=?;";
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, session_id);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                int64_t chat_id = sqlite3_column_int64(stmt, 0);
                telegram_send_message(cfg->telegram_token, chat_id, reply);
            }
            sqlite3_finalize(stmt);
        }
    }
    /* For other routes (cli, etc.) — response stays in DB, client polls */

    free(route);
    entry_branch_free(entries, count);
}

/* ── T201: PENDING entry helpers (V77/V78/V79) ─────────────────── */

/* Find tool_result entry with content='PENDING' in agent DB for given session.
 * Returns tool_call_id (caller frees) or NULL. Sets *entry_id. */
static char *find_pending_entry(sqlite3 *adb, int64_t session_id, int64_t *entry_id) {
    const char *sql =
        "SELECT id, tool_call_id FROM entries"
        " WHERE session_id=? AND role=3 AND content='PENDING'"
        " ORDER BY id DESC LIMIT 1;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(adb, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(stmt, 1, session_id);
    char *tc_id = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *entry_id = sqlite3_column_int64(stmt, 0);
        const char *v = (const char *)sqlite3_column_text(stmt, 1);
        if (v) tc_id = strdup(v);
    }
    sqlite3_finalize(stmt);
    return tc_id;
}

/* Read tool_call arguments from last assistant entry matching tool_call_id.
 * Returns JSON args string (caller frees) and sets *tool_name. */
static char *read_tool_call_args(sqlite3 *adb, int64_t session_id,
                                 const char *tool_call_id, char **tool_name) {
    *tool_name = NULL;
    /* Find last assistant entry with tool_calls containing this id */
    const char *sql =
        "SELECT tool_calls FROM entries"
        " WHERE session_id=? AND role=2 AND tool_calls IS NOT NULL"
        " ORDER BY id DESC LIMIT 1;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(adb, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(stmt, 1, session_id);
    char *args = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *tc_json = (const char *)sqlite3_column_text(stmt, 0);
        if (tc_json) {
            cJSON *arr = cJSON_Parse(tc_json);
            if (arr && cJSON_IsArray(arr)) {
                int n = cJSON_GetArraySize(arr);
                for (int i = 0; i < n; i++) {
                    cJSON *item = cJSON_GetArrayItem(arr, i);
                    cJSON *id = cJSON_GetObjectItem(item, "id");
                    if (id && id->valuestring &&
                        strcmp(id->valuestring, tool_call_id) == 0) {
                        cJSON *a = cJSON_GetObjectItem(item, "args");
                        if (a) args = cJSON_PrintUnformatted(a);
                        cJSON *nm = cJSON_GetObjectItem(item, "name");
                        if (nm && nm->valuestring)
                            *tool_name = strdup(nm->valuestring);
                        break;
                    }
                }
            }
            cJSON_Delete(arr);
        }
    }
    sqlite3_finalize(stmt);
    return args;
}

/* UPDATE PENDING entry content with real result */
static int update_pending_entry(sqlite3 *adb, int64_t entry_id, const char *result) {
    const char *sql = "UPDATE entries SET content=? WHERE id=? AND content='PENDING';";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(adb, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, result, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, entry_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* ── T205/V79: Apply config changes from agent exit code 4 ──────── */

static const struct {
    const char *name;
    const char *base_url;
    const char *model;
} KNOWN_PROVIDERS[] = {
    {"openrouter", "https://openrouter.ai/api/v1", "deepseek/deepseek-v4-flash"},
    {"gemini",     "https://generativelanguage.googleapis.com/v1beta/openai", "gemini-2.5-flash"},
    {"anthropic",  "https://api.anthropic.com/v1", "claude-sonnet-4-20250514"},
};
#define KNOWN_PROVIDER_COUNT (sizeof(KNOWN_PROVIDERS) / sizeof(KNOWN_PROVIDERS[0]))

char *daemon_apply_config(const Config *cfg, sqlite3 *db,
                                 const char *agent_name,
                                 const char *tool_name, const char *args_json) {
    (void)cfg;
    if (!tool_name || !args_json) return NULL;

    cJSON *args = cJSON_Parse(args_json);
    if (!args) return strdup("error: invalid JSON in tool_call args");

    char *result = NULL;

    if (strcmp(tool_name, "configure_provider") == 0) {
        /* V67: Store provider config in daemon.db */
        cJSON *provider = cJSON_GetObjectItemCaseSensitive(args, "provider");
        cJSON *api_key = cJSON_GetObjectItemCaseSensitive(args, "api_key");
        cJSON *base_url = cJSON_GetObjectItemCaseSensitive(args, "base_url");
        cJSON *model = cJSON_GetObjectItemCaseSensitive(args, "model");

        if (!cJSON_IsString(provider) || !cJSON_IsString(api_key)) {
            cJSON_Delete(args);
            return strdup("error: provider and api_key required");
        }

        const char *pname = provider->valuestring;
        const char *url = NULL;
        const char *mdl = NULL;

        for (size_t i = 0; i < KNOWN_PROVIDER_COUNT; i++) {
            if (strcmp(pname, KNOWN_PROVIDERS[i].name) == 0) {
                url = KNOWN_PROVIDERS[i].base_url;
                mdl = KNOWN_PROVIDERS[i].model;
                break;
            }
        }

        if (cJSON_IsString(base_url) && base_url->valuestring[0])
            url = base_url->valuestring;
        if (cJSON_IsString(model) && model->valuestring[0])
            mdl = model->valuestring;

        db_kv_set_secret(db, "provider.api_key", api_key->valuestring);
        if (url) db_kv_set(db, "provider.base_url", url);
        if (mdl) db_kv_set(db, "provider.model", mdl);

        char buf[256];
        snprintf(buf, sizeof(buf), "provider configured: %s (model: %s)",
                 pname, mdl ? mdl : "(default)");
        result = strdup(buf);

    } else if (strcmp(tool_name, "configure_channel") == 0) {
        /* V67: Store channel credentials in daemon.db */
        cJSON *channel_type = cJSON_GetObjectItemCaseSensitive(args, "channel_type");
        cJSON *bot_token = cJSON_GetObjectItemCaseSensitive(args, "bot_token");

        if (!cJSON_IsString(channel_type)) {
            cJSON_Delete(args);
            return strdup("error: channel_type required");
        }

        const char *ctype = channel_type->valuestring;
        if (strcmp(ctype, "telegram") == 0) {
            if (cJSON_IsString(bot_token) && bot_token->valuestring[0])
                db_kv_set_secret(db, "telegram_token", bot_token->valuestring);
            /* V69: Bind channel to requesting agent */
            db_channel_binding_set(db, "telegram", "default", agent_name);
            result = strdup("channel configured: telegram");
        } else if (strcmp(ctype, "cli") == 0) {
            db_channel_binding_set(db, "cli", "default", agent_name);
            result = strdup("channel configured: cli");
        } else {
            result = strdup("error: unknown channel_type");
        }

    } else if (strcmp(tool_name, "create_agent") == 0) {
        /* V79/V68: Create new agent dir + agent.db + daemon.db rows */
        char *args_str = cJSON_PrintUnformatted(args);
        if (args_str) {
            int rc = agent_config_create("agents", db, args_str);
            if (rc == 0) {
                cJSON *name = cJSON_GetObjectItemCaseSensitive(args, "name");
                char buf[256];
                snprintf(buf, sizeof(buf), "agent created: %s",
                         cJSON_IsString(name) ? name->valuestring : "unknown");
                result = strdup(buf);
            } else {
                result = strdup("error: failed to create agent");
            }
            free(args_str);
        } else {
            result = strdup("error: failed to serialize args");
        }
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "error: unknown config tool: %s", tool_name);
        result = strdup(buf);
    }

    cJSON_Delete(args);
    return result;
}

/* ── Reap children ──────────────────────────────────────────────── */

static void process_spawn_queue(const Config *cfg, sqlite3 *db);

static void reap_children(const Config *cfg, sqlite3 *db) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int64_t session_id;
        char agent_name[64] = "";
        if (child_remove(pid, &session_id, agent_name) != 0) continue;

        /* T200: Agent sets own state before exit. Daemon only forces idle
         * for normal completion (exit 0/1) or abnormal termination (signal).
         * Exit codes 2/3/4 mean agent set "waiting" — daemon respects that. */
        int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        if (exit_code == AGENT_EXIT_DONE || exit_code == AGENT_EXIT_ERROR ||
            !WIFEXITED(status)) {
            daemon_agent_set_state(agent_name, session_id, "idle");
        }

        /* V71/T194: Accumulate token usage from this agent run */
        {
            char *path = agent_db_path(agent_name);
            if (path) {
                sqlite3 *adb = db_open_agent(path);
                free(path);
                if (adb) {
                    const char *uq = "SELECT COALESCE(SUM(usage_in + usage_out), 0)"
                                     " FROM entries WHERE session_id=? AND turn_id="
                                     "(SELECT MAX(turn_id) FROM entries WHERE session_id=?);";
                    sqlite3_stmt *us;
                    if (sqlite3_prepare_v2(adb, uq, -1, &us, NULL) == SQLITE_OK) {
                        sqlite3_bind_int64(us, 1, session_id);
                        sqlite3_bind_int64(us, 2, session_id);
                        if (sqlite3_step(us) == SQLITE_ROW)
                            daemon_token_usage_add(sqlite3_column_int(us, 0));
                        sqlite3_finalize(us);
                    }
                    db_close(adb);
                }
            }
        }

        /* T201/V77: Exit code 2 = spawn request. Find PENDING entry, read tool_call args. */
        if (exit_code == AGENT_EXIT_SPAWN) {
            char *path = agent_db_path(agent_name);
            if (path) {
                sqlite3 *adb = db_open_agent(path);
                free(path);
                if (adb) {
                    int64_t pending_eid = 0;
                    char *tc_id = find_pending_entry(adb, session_id, &pending_eid);
                    if (tc_id) {
                        char *tname = NULL;
                        char *args = read_tool_call_args(adb, session_id, tc_id, &tname);
                        if (args) {
                            /* Parse spawn_agent args: task, background */
                            cJSON *j = cJSON_Parse(args);
                            const char *task = "";
                            int bg = 0;
                            int depth = 0;
                            if (j) {
                                cJSON *t = cJSON_GetObjectItem(j, "task");
                                if (t && t->valuestring) task = t->valuestring;
                                cJSON *b = cJSON_GetObjectItem(j, "background");
                                if (cJSON_IsTrue(b)) bg = 1;
                            }
                            /* Get depth from session */
                            const char *dsql = "SELECT depth FROM sessions WHERE id=?;";
                            sqlite3_stmt *ds;
                            if (sqlite3_prepare_v2(adb, dsql, -1, &ds, NULL) == SQLITE_OK) {
                                sqlite3_bind_int64(ds, 1, session_id);
                                if (sqlite3_step(ds) == SQLITE_ROW)
                                    depth = sqlite3_column_int(ds, 0);
                                sqlite3_finalize(ds);
                            }
                            /* Insert into daemon.db spawn_queue */
                            const char *ins = "INSERT INTO spawn_queue"
                                " (parent_agent, parent_session_id, task, background, depth, tool_call_id)"
                                " VALUES (?,?,?,?,?,?);";
                            sqlite3_stmt *is;
                            if (sqlite3_prepare_v2(db, ins, -1, &is, NULL) == SQLITE_OK) {
                                sqlite3_bind_text(is, 1, agent_name, -1, SQLITE_STATIC);
                                sqlite3_bind_int64(is, 2, session_id);
                                sqlite3_bind_text(is, 3, task, -1, SQLITE_STATIC);
                                sqlite3_bind_int(is, 4, bg);
                                sqlite3_bind_int(is, 5, depth + 1);
                                sqlite3_bind_text(is, 6, tc_id, -1, SQLITE_STATIC);
                                sqlite3_step(is);
                                sqlite3_finalize(is);
                            }
                            /* Background: resolve PENDING immediately, re-fork */
                            if (bg) {
                                update_pending_entry(adb, pending_eid,
                                    "background agent spawned");
                                daemon_agent_set_state(agent_name, session_id, "idle");
                                daemon_signal_session_agent(session_id, agent_name);
                            }
                            cJSON_Delete(j);
                            free(args);
                        }
                        free(tname);
                        free(tc_id);
                    }
                    db_close(adb);
                }
            }
        }

        /* T201/V78: Exit code 3 = approval request. Find PENDING, read tool_call args. */
        if (exit_code == AGENT_EXIT_APPROVAL) {
            char *path = agent_db_path(agent_name);
            if (path) {
                sqlite3 *adb = db_open_agent(path);
                free(path);
                if (adb) {
                    int64_t pending_eid = 0;
                    char *tc_id = find_pending_entry(adb, session_id, &pending_eid);
                    if (tc_id) {
                        char *tname = NULL;
                        char *args = read_tool_call_args(adb, session_id, tc_id, &tname);
                        if (args) {
                            /* Parse approval_request args: type, payload */
                            cJSON *j = cJSON_Parse(args);
                            const char *type = "unknown";
                            char *payload_str = NULL;
                            if (j) {
                                cJSON *t = cJSON_GetObjectItem(j, "type");
                                if (t && t->valuestring) type = t->valuestring;
                                cJSON *p = cJSON_GetObjectItem(j, "payload");
                                if (p) payload_str = cJSON_PrintUnformatted(p);
                            }
                            if (!payload_str) payload_str = strdup("{}");
                            /* T203: Insert into daemon.db approvals w/ tool_call_id */
                            approval_insert(db, session_id, agent_name,
                                            tc_id, type, payload_str);
                            free(payload_str);
                            cJSON_Delete(j);
                            free(args);
                        }
                        free(tname);
                        free(tc_id);
                    }
                    db_close(adb);
                }
            }
        }

        /* T201/V79: Exit code 4 = config change. Find PENDING, read tool_call args, apply. */
        if (exit_code == AGENT_EXIT_CONFIG) {
            char *path = agent_db_path(agent_name);
            if (path) {
                sqlite3 *adb = db_open_agent(path);
                free(path);
                if (adb) {
                    int64_t pending_eid = 0;
                    char *tc_id = find_pending_entry(adb, session_id, &pending_eid);
                    if (tc_id) {
                        char *tname = NULL;
                        char *args = read_tool_call_args(adb, session_id, tc_id, &tname);
                        if (args) {
                            /* T205/V79: Daemon applies config to daemon.db */
                            char *result = daemon_apply_config(cfg, db, agent_name,
                                                              tname, args);
                            update_pending_entry(adb, pending_eid,
                                                result ? result : "error: config apply failed");
                            free(result);
                            daemon_agent_set_state(agent_name, session_id, "idle");
                            daemon_signal_session_agent(session_id, agent_name);
                            free(args);
                        }
                        free(tname);
                        free(tc_id);
                    }
                    db_close(adb);
                }
            }
        }

        /* T201/V33: Check if this was a sub-agent from spawn_queue (blocking).
         * On completion, UPDATE parent's PENDING entry with result. */
        const char *sq_sql =
            "SELECT sq.id, sq.parent_session_id, sq.tool_call_id, sq.background, sq.parent_agent"
            " FROM spawn_queue sq WHERE sq.child_session_id=? AND sq.status='forked';";
        sqlite3_stmt *sq_stmt;
        if (sqlite3_prepare_v2(db, sq_sql, -1, &sq_stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(sq_stmt, 1, session_id);
            if (sqlite3_step(sq_stmt) == SQLITE_ROW) {
                int64_t sq_id = sqlite3_column_int64(sq_stmt, 0);
                int64_t parent_sid = sqlite3_column_int64(sq_stmt, 1);
                const char *parent_tc_id = (const char *)sqlite3_column_text(sq_stmt, 2);
                int bg = sqlite3_column_int(sq_stmt, 3);
                const char *parent_agent = (const char *)sqlite3_column_text(sq_stmt, 4);

                /* Get sub-agent result from last assistant entry */
                const char *result_text = "sub-agent completed with no output";
                char *result_buf = NULL;
                {
                    char *cpath = agent_db_path(agent_name);
                    if (cpath) {
                        sqlite3 *cadb = db_open_agent(cpath);
                        free(cpath);
                        if (cadb) {
                            int bcount = 0;
                            Entry *branch = session_get_branch(cadb, session_id, &bcount);
                            if (branch) {
                                for (int i = bcount - 1; i >= 0; i--) {
                                    if (branch[i].message.role == ROLE_ASSISTANT && branch[i].message.content) {
                                        result_buf = strdup(branch[i].message.content);
                                        result_text = result_buf;
                                        break;
                                    }
                                }
                                entry_branch_free(branch, bcount);
                            }
                            db_close(cadb);
                        }
                    }
                }

                spawn_queue_mark(db, sq_id, "done", session_id);

                if (!bg && parent_agent && parent_agent[0] && parent_tc_id) {
                    /* V77/V33: UPDATE parent's PENDING entry with result */
                    char *ppath = agent_db_path(parent_agent);
                    if (ppath) {
                        sqlite3 *padb = db_open_agent(ppath);
                        free(ppath);
                        if (padb) {
                            /* Find PENDING entry by tool_call_id */
                            const char *fsql =
                                "SELECT id FROM entries WHERE session_id=?"
                                " AND tool_call_id=? AND content='PENDING' LIMIT 1;";
                            sqlite3_stmt *fs;
                            if (sqlite3_prepare_v2(padb, fsql, -1, &fs, NULL) == SQLITE_OK) {
                                sqlite3_bind_int64(fs, 1, parent_sid);
                                sqlite3_bind_text(fs, 2, parent_tc_id, -1, SQLITE_STATIC);
                                if (sqlite3_step(fs) == SQLITE_ROW) {
                                    int64_t peid = sqlite3_column_int64(fs, 0);
                                    update_pending_entry(padb, peid, result_text);
                                }
                                sqlite3_finalize(fs);
                            }
                            /* Transition parent waiting→idle, re-fork */
                            session_set_state(padb, parent_sid, "idle");
                            db_close(padb);
                            daemon_signal_session_agent(parent_sid, parent_agent);
                        }
                    }
                }
                free(result_buf);
                sqlite3_finalize(sq_stmt);
                continue; /* Skip normal deliver_response for sub-agents */
            }
            sqlite3_finalize(sq_stmt);
        }

        /* V26: deliver response (normal agent, not sub-agent) */
        deliver_response(cfg, db, agent_name, session_id);

        /* Check if more inbox items arrived while agent was running */
        {
            int pending = daemon_inbox_count(agent_name, session_id);
            if (pending > 0) {
                daemon_signal_session(session_id);
            }
        }
    }
}

/* ── T88: Process spawn queue ───────────────────────────────────── */

static void process_spawn_queue(const Config *cfg, sqlite3 *db) {
    int count = 0;
    SpawnRequest *reqs = spawn_queue_peek_pending(db, &count);
    if (!reqs) return;


    for (int i = 0; i < count; i++) {
        SpawnRequest *r = &reqs[i];

        /* V71/T194: reject if token rate limit exceeded */
        if (cfg->token_rate_limit > 0 && daemon_token_usage_hourly() >= cfg->token_rate_limit) {
            spawn_queue_mark(db, r->id, "rejected", 0);
            continue;
        }

        /* V3: re-check limits before forking (in-memory child count) */
        if (g_child_count >= AGENT_MAX_TOTAL) {
            spawn_queue_mark(db, r->id, "rejected", 0);
            continue;
        }
        if (child_count_for_parent(db, r->parent_session_id) >= AGENT_MAX_PER_PARENT) {
            spawn_queue_mark(db, r->id, "rejected", 0);
            continue;
        }

        /* Resolve parent agent name from spawn_queue */
        const char *parent_agent_name = NULL;
        char pa_buf[64] = "";
        {
            const char *pa_sql = "SELECT parent_agent FROM spawn_queue WHERE id=?;";
            sqlite3_stmt *pa_stmt;
            if (sqlite3_prepare_v2(db, pa_sql, -1, &pa_stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(pa_stmt, 1, r->id);
                if (sqlite3_step(pa_stmt) == SQLITE_ROW) {
                    const char *v = (const char *)sqlite3_column_text(pa_stmt, 0);
                    if (v && v[0]) snprintf(pa_buf, sizeof(pa_buf), "%s", v);
                }
                sqlite3_finalize(pa_stmt);
            }
            parent_agent_name = pa_buf[0] ? pa_buf : NULL;
        }
        if (!parent_agent_name) {
            spawn_queue_mark(db, r->id, "error", 0);
            continue;
        }

        /* Create child session in parent's agent DB (sub-agents share parent's DB) */
        char *apath = agent_db_path(parent_agent_name);
        if (!apath) { spawn_queue_mark(db, r->id, "error", 0); continue; }
        sqlite3 *adb = db_open_agent(apath);
        free(apath);
        if (!adb) { spawn_queue_mark(db, r->id, "error", 0); continue; }

        char name_buf[128];
        snprintf(name_buf, sizeof(name_buf), "sub-agent:%lld", (long long)r->parent_session_id);
        int64_t child_sid = session_create(adb, name_buf, parent_agent_name,
                                           r->parent_session_id, r->depth);
        if (child_sid < 0) {
            db_close(adb);
            spawn_queue_mark(db, r->id, "error", 0);
            continue;
        }

        /* Insert task as user message in child session inbox */
        inbox_insert(adb, child_sid, "spawn", r->task);

        /* T200: Mark child session running in agent DB */
        session_set_state(adb, child_sid, "running");
        db_close(adb);

        pid_t pid = fork();
        if (pid < 0) {
            daemon_agent_set_state(parent_agent_name, child_sid, "idle");
            spawn_queue_mark(db, r->id, "error", 0);
            continue;
        }
        if (pid == 0) {
            /* V67/T188: inject decrypted secrets as env vars for child */
            inject_secrets_for_child(db);
            setenv("CCLAW_AGENT_NAME", parent_agent_name, 1);
            char agent_db_buf[1024];
            snprintf(agent_db_buf, sizeof(agent_db_buf), "agents/%s/agent.db", parent_agent_name);
            setenv("CCLAW_AGENT_DB", agent_db_buf, 1);
            char sid_arg[64];
            snprintf(sid_arg, sizeof(sid_arg), "--session-id=%lld", (long long)child_sid);
            execl(g_self_path, g_self_path, "--agent", sid_arg, (char *)NULL);
            _exit(127);
        }

        /* Track child (T200: includes agent_name) */
        child_add(pid, child_sid, parent_agent_name);
        spawn_queue_mark(db, r->id, "forked", child_sid);

        /* V13 blocking: transition parent to "waiting" state in agent DB */
        if (!r->background) {
            daemon_agent_set_state(parent_agent_name, r->parent_session_id, "waiting");
        }
    }
    spawn_request_free(reqs, count);
}

/* ── T94/T200/V34: Daemon startup recovery — scan all agent DBs ── */

void daemon_startup_recovery(sqlite3 *db) {
    (void)db; /* daemon.db not used for session state anymore */

    /* T200: Scan all agent DBs for non-idle sessions */
    size_t agent_count = 0;
    char **names = agent_discover("agents", &agent_count);
    if (!names) goto cleanup;

    for (size_t a = 0; a < agent_count; a++) {
        char *path = agent_db_path(names[a]);
        if (!path) continue;
        sqlite3 *adb = db_open_agent(path);
        free(path);
        if (!adb) continue;

        /* (1) "running" sessions: process died, reset to idle + signal re-fork */
        {
            const char *sql = "SELECT id FROM sessions WHERE state='running';";
            sqlite3_stmt *stmt;
            if (sqlite3_prepare_v2(adb, sql, -1, &stmt, NULL) == SQLITE_OK) {
                int64_t ids[128];
                int n = 0;
                while (sqlite3_step(stmt) == SQLITE_ROW && n < 128)
                    ids[n++] = sqlite3_column_int64(stmt, 0);
                sqlite3_finalize(stmt);

                sqlite3_exec(adb, "UPDATE sessions SET state='idle' WHERE state='running';",
                             NULL, NULL, NULL);

                for (int i = 0; i < n; i++)
                    daemon_signal_session(ids[i]);
            }
        }

        /* (2) "waiting" sessions: check inbox for result */
        {
            const char *sql = "SELECT id FROM sessions WHERE state='waiting';";
            sqlite3_stmt *stmt;
            if (sqlite3_prepare_v2(adb, sql, -1, &stmt, NULL) == SQLITE_OK) {
                int64_t ids[128];
                int n = 0;
                while (sqlite3_step(stmt) == SQLITE_ROW && n < 128)
                    ids[n++] = sqlite3_column_int64(stmt, 0);
                sqlite3_finalize(stmt);

                for (int i = 0; i < n; i++) {
                    int pending = inbox_count(adb, ids[i]);
                    if (pending <= 0) {
                        inbox_insert(adb, ids[i], "recovery",
                            "error: sub-agent process lost during daemon restart");
                    }
                    session_set_state(adb, ids[i], "idle");
                }
            }
        }

        db_close(adb);
    }
    agent_discover_free(names, agent_count);

cleanup:
    /* T118: Clean up stale session temp dirs */
    (void)system("rm -rf /tmp/cclaw-*");
}

/* ── Bootstrap (T189, V68) ──────────────────────────────────────── */

#include "templates.h"

/* T196: Migrate agent.json files to daemon.db agent_config table */
static void migrate_agent_json_files(sqlite3 *db) {
    size_t count = 0;
    char **names = agent_discover("agents", &count);
    if (!names) return;
    for (size_t i = 0; i < count; i++) {
        agent_config_migrate_json(db, "agents", names[i]);
    }
    agent_discover_free(names, count);
}

int64_t daemon_bootstrap(sqlite3 *db) {
    if (!db) return -1;

    /* Check if any named (non-ephemeral) agents exist */
    size_t count = 0;
    char **names = agent_discover("agents", &count);
    int has_named = 0;
    for (size_t i = 0; i < count; i++) {
        if (strncmp(names[i], "ephemeral-", 10) != 0)
            has_named = 1;
    }
    agent_discover_free(names, count);
    if (has_named) return 0;

    /* No named agents — create bootstrap ephemeral */
    char *agent_name = agent_create_ephemeral("agents", db);
    if (!agent_name) return -1;

    /* T196: Write bootstrap config to daemon.db agent_config table */
    AgentConfig boot_ac = {0};
    boot_ac.name = agent_name;
    char *boot_tools[] = {"configure_provider", "configure_channel", "create_agent"};
    boot_ac.tools = boot_tools;
    boot_ac.tool_count = 3;
    agent_config_save_db(db, &boot_ac);

    /* Write bootstrap system prompt */
    char path[1024];
    snprintf(path, sizeof(path), "agents/%s/system.md", agent_name);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(TPL_BOOTSTRAP_SYSTEM_PROMPT_MD, f);
        fclose(f);
    }

    /* Re-seed DB row with updated config */
    AgentRow *row = db_agent_seed(db, "agents", agent_name);
    agent_row_free(row);

    /* T200: Create session in agent DB (not daemon.db) */
    char *apath = agent_db_path(agent_name);
    if (!apath) { free(agent_name); return -1; }
    sqlite3 *adb = db_open_agent(apath);
    free(apath);
    if (!adb) { free(agent_name); return -1; }

    int64_t sid = session_create(adb, "bootstrap", agent_name, -1, 0);
    db_close(adb);
    if (sid < 0) { free(agent_name); return -1; }

    daemon_inbox_insert(agent_name, sid, "system",
                 "Welcome! Let's set up your first CClaw agent. "
                 "What LLM provider would you like to use? "
                 "(OpenRouter is recommended — just need an API key)");

    /* Signal with agent_name so daemon can fork */
    daemon_signal_session_agent(sid, agent_name);
    free(agent_name);
    return sid;
}

/* ── Daemon main loop (T81) ─────────────────────────────────────── */

int daemon_run(const Config *cfg, sqlite3 *db) {
    /* V61: Set CCLAW_DB_PATH so forked agent children find the DB */
    if (cfg->db_path)
        setenv("CCLAW_DB_PATH", cfg->db_path, 1);

    /* Resolve self path for fork+exec (skip if already set via daemon_set_self_path) */
    if (!g_self_path[0]) {
        ssize_t sp_len = readlink("/proc/self/exe", g_self_path, sizeof(g_self_path) - 1);
        if (sp_len > 0) g_self_path[sp_len] = '\0';
        else strcpy(g_self_path, "./build/cclaw");
    }

    if (daemon_signal_init() != 0) return -1;
    if (sigchld_pipe_init() != 0) {
        daemon_signal_close();
        return -1;
    }

    int fifo_fd = daemon_fifo_open(cfg->db_path);

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        daemon_signal_close();
        sigchld_pipe_close();
        if (fifo_fd >= 0) daemon_fifo_close(fifo_fd, cfg->db_path);
        return -1;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = g_signal_pipe[0];
    epoll_ctl(epfd, EPOLL_CTL_ADD, g_signal_pipe[0], &ev);

    ev.data.fd = g_chld_pipe[0];
    epoll_ctl(epfd, EPOLL_CTL_ADD, g_chld_pipe[0], &ev);

    if (fifo_fd >= 0) {
        ev.data.fd = fifo_fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, fifo_fd, &ev);
    }

    /* T94/V34: Startup recovery — children already dead after daemon restart */
    migrate_agent_json_files(db);
    daemon_startup_recovery(db);

    struct epoll_event events[8];
    while (!shutdown_requested()) {
        int nfds = epoll_wait(epfd, events, 8, 1000);
        if (nfds < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == g_chld_pipe[0]) {
                /* Drain SIGCHLD self-pipe */
                char buf[64];
                while (read(g_chld_pipe[0], buf, sizeof(buf)) > 0) {}
                reap_children(cfg, db);
                /* T184/V13: process spawn queue after reap (parent may have queued blocking spawn) */
                process_spawn_queue(cfg, db);
            } else if (events[i].data.fd == g_signal_pipe[0] ||
                       events[i].data.fd == fifo_fd) {
                /* T200: Read SignalMsg from signal pipe or external FIFO */
                int rfd = events[i].data.fd;
                SignalMsg msg;
                while (read(rfd, &msg, sizeof(msg)) == sizeof(msg)) {
                    /* Resolve agent_name if not in signal */
                    const char *aname = msg.agent_name[0] ? msg.agent_name : NULL;
                    if (!aname) {
                        /* Fallback: look up from tg_chat_sessions in daemon.db */
                        const char *lsql = "SELECT agent_name FROM tg_chat_sessions WHERE session_id=?;";
                        sqlite3_stmt *ls;
                        if (sqlite3_prepare_v2(db, lsql, -1, &ls, NULL) == SQLITE_OK) {
                            sqlite3_bind_int64(ls, 1, msg.session_id);
                            if (sqlite3_step(ls) == SQLITE_ROW) {
                                const char *v = (const char *)sqlite3_column_text(ls, 0);
                                if (v && v[0])
                                    snprintf(msg.agent_name, sizeof(msg.agent_name), "%s", v);
                            }
                            sqlite3_finalize(ls);
                        }
                        aname = msg.agent_name[0] ? msg.agent_name : NULL;
                    }
                    if (!aname) continue; /* Cannot resolve — skip */

                    /* Check inbox has pending items before forking */
                    int has_inbox = daemon_inbox_count(aname, msg.session_id) > 0;
                    if (has_inbox) {
                        fork_agent(cfg, db, msg.session_id, aname);
                    }
                }
                /* T88: Process any pending spawn requests */
                process_spawn_queue(cfg, db);
            }
        }

        /* Also reap on timeout (defensive) */
        if (nfds == 0 && g_child_count > 0) {
            reap_children(cfg, db);
        }
    }

    /* V31: Forward SIGTERM to children */
    for (int i = 0; i < g_child_count; i++) {
        kill(g_children[i].pid, SIGTERM);
    }
    /* Wait for children to exit (brief grace period) */
    for (int i = 0; i < 10 && g_child_count > 0; i++) {
        reap_children(cfg, db);
        if (g_child_count > 0) usleep(100000);
    }

    close(epfd);
    sigchld_pipe_close();
    daemon_signal_close();
    daemon_fifo_close(fifo_fd, cfg->db_path);
    g_child_count = 0;
    return 0;
}
