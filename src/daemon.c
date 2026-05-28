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

/* ── Signal pipe (T82, V25) ─────────────────────────────────────── */

static int g_signal_pipe[2] = {-1, -1};
static char g_self_path[4096] = "";

void daemon_set_self_path(const char *path) {
    snprintf(g_self_path, sizeof(g_self_path), "%s", path);
}

static void set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int daemon_signal_init(void) {
    if (pipe(g_signal_pipe) != 0) return -1;
    set_nonblock(g_signal_pipe[0]);
    set_nonblock(g_signal_pipe[1]);
    return 0;
}

int daemon_signal_session(int64_t session_id) {
    if (g_signal_pipe[1] < 0) return -1;
    /* Write session_id as 8 bytes; non-blocking, drop if pipe full */
    ssize_t n = write(g_signal_pipe[1], &session_id, sizeof(session_id));
    return (n == sizeof(session_id)) ? 0 : -1;
}

/* Signal daemon from external process via named FIFO. */
int daemon_signal_external(const char *db_path, int64_t session_id) {
    char *path = daemon_pipe_path(db_path);
    if (!path) return -1;
    int fd = open(path, O_WRONLY | O_NONBLOCK);
    free(path);
    if (fd < 0) return -1;
    ssize_t n = write(fd, &session_id, sizeof(session_id));
    close(fd);
    return (n == sizeof(session_id)) ? 0 : -1;
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

/* ── Child tracking (T87, V24) ──────────────────────────────────── */

typedef struct {
    pid_t pid;
    int64_t session_id;
} ChildSlot;

static ChildSlot g_children[DAEMON_MAX_CHILDREN];
static int g_child_count = 0;

static int child_add(pid_t pid, int64_t session_id) {
    if (g_child_count >= DAEMON_MAX_CHILDREN) return -1;
    g_children[g_child_count].pid = pid;
    g_children[g_child_count].session_id = session_id;
    g_child_count++;
    return 0;
}

static int child_remove(pid_t pid, int64_t *out_session_id) {
    for (int i = 0; i < g_child_count; i++) {
        if (g_children[i].pid == pid) {
            *out_session_id = g_children[i].session_id;
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

static int fork_agent(const Config *cfg, sqlite3 *db, int64_t session_id) {
    /* V24: only fork if state == idle */
    if (child_has_session(session_id)) return -1;

    /* V71/T194: reject fork if token rate limit exceeded */
    if (cfg->token_rate_limit > 0 && daemon_token_usage_hourly() >= cfg->token_rate_limit) {
        char *aname = session_get_agent_name(db, session_id);
        if (aname) {
            daemon_inbox_insert(aname, session_id, "system", "error: token rate limit exceeded");
            free(aname);
        }
        return -1;
    }

    /* Mark session running */
    if (session_set_state(db, session_id, "running") != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        session_set_state(db, session_id, "idle");
        return -1;
    }
    if (pid == 0) {
        /* V67/T188: inject decrypted secrets as env vars for child */
        inject_secrets_for_child(db);

        /* V73/V74,T198: inject agent identity + DB path + config as env vars */
        char *agent_name = session_get_agent_name(db, session_id);
        if (agent_name) {
            setenv("CCLAW_AGENT_NAME", agent_name, 1);

            /* Build agent DB path: .cclaw/agents/<name>/agent.db */
            char agent_db[1024];
            snprintf(agent_db, sizeof(agent_db), ".cclaw/agents/%s/agent.db", agent_name);
            setenv("CCLAW_AGENT_DB", agent_db, 1);

            /* Load per-agent config from daemon.db, inject as env vars */
            AgentConfig *ac = agent_config_load_db(db, agent_name);
            if (ac) {
                if (ac->workspace)
                    setenv("CCLAW_WORKSPACE", ac->workspace, 1);
                else {
                    char ws[1024];
                    snprintf(ws, sizeof(ws), ".cclaw/agents/%s/workspace", agent_name);
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
                snprintf(ws, sizeof(ws), ".cclaw/agents/%s/workspace", agent_name);
                setenv("CCLAW_WORKSPACE", ws, 1);
            }
            free(agent_name);
        } else {
            /* No agent_name on session — use daemon DB as agent DB (transition) */
            const char *db_filename = sqlite3_db_filename(db, "main");
            if (db_filename)
                setenv("CCLAW_AGENT_DB", db_filename, 1);
            if (cfg->workspace)
                setenv("CCLAW_WORKSPACE", cfg->workspace, 1);
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

    /* Parent: track child */
    child_add(pid, session_id);
    return 0;
}

/* ── Response delivery (T85, V26) ──────────────────────────────── */

static void deliver_response(const Config *cfg, sqlite3 *db, int64_t session_id) {
    /* Get last assistant message from session branch */
    int count = 0;
    Entry *entries = session_get_branch(db, session_id, &count);
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
        return;
    }

    /* V42/T110: Suppress HEARTBEAT_OK sentinel — never deliver to channel */
    if (strcmp(reply, "HEARTBEAT_OK") == 0) {
        entry_branch_free(entries, count);
        return;
    }

    /* V44/T113: Suppress [NO_REPLY] — agent decided not to respond (e.g. group irrelevance) */
    if (strstr(reply, "[NO_REPLY]") != NULL) {
        entry_branch_free(entries, count);
        return;
    }

    char *route = session_get_last_route(db, session_id);
    if (route && strncmp(route, "telegram", 8) == 0) {
        /* Route format: "telegram" — need chat_id from tg_chat_sessions */
        /* Look up chat_id for this session */
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

/* ── Reap children ──────────────────────────────────────────────── */

static void process_spawn_queue(const Config *cfg, sqlite3 *db);

static void reap_children(const Config *cfg, sqlite3 *db) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int64_t session_id;
        if (child_remove(pid, &session_id) != 0) continue;

        /* Mark session idle */
        session_set_state(db, session_id, "idle");

        /* V71/T194: Accumulate token usage from this agent run */
        {
            const char *uq = "SELECT COALESCE(SUM(usage_in + usage_out), 0)"
                             " FROM entries WHERE session_id=? AND turn_id="
                             "(SELECT MAX(turn_id) FROM entries WHERE session_id=?);";
            sqlite3_stmt *us;
            if (sqlite3_prepare_v2(db, uq, -1, &us, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(us, 1, session_id);
                sqlite3_bind_int64(us, 2, session_id);
                if (sqlite3_step(us) == SQLITE_ROW)
                    daemon_token_usage_add(sqlite3_column_int(us, 0));
                sqlite3_finalize(us);
            }
        }

        /* T88: Check if this was a sub-agent from spawn_queue (blocking) */
        const char *sq_sql =
            "SELECT sq.id, sq.parent_session_id, sq.tool_call_id, sq.background"
            " FROM spawn_queue sq WHERE sq.child_session_id=? AND sq.status='forked';";
        sqlite3_stmt *sq_stmt;
        if (sqlite3_prepare_v2(db, sq_sql, -1, &sq_stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(sq_stmt, 1, session_id);
            if (sqlite3_step(sq_stmt) == SQLITE_ROW) {
                int64_t sq_id = sqlite3_column_int64(sq_stmt, 0);
                int64_t parent_sid = sqlite3_column_int64(sq_stmt, 1);
                (void)sqlite3_column_text(sq_stmt, 2); /* tool_call_id — used by T201 */
                int bg = sqlite3_column_int(sq_stmt, 3);

                /* Get sub-agent result from last assistant entry in child session */
                int bcount = 0;
                Entry *branch = session_get_branch(db, session_id, &bcount);
                const char *result_text = NULL;
                if (branch) {
                    for (int i = bcount - 1; i >= 0; i--) {
                        if (branch[i].message.role == ROLE_ASSISTANT && branch[i].message.content) {
                            result_text = branch[i].message.content;
                            break;
                        }
                    }
                }
                if (!result_text) result_text = "sub-agent completed with no output";

                spawn_queue_mark(db, sq_id, "done", session_id);

                if (!bg) {
                    /* V13 blocking: post tool_result to parent inbox, wake parent */
                    char *parent_agent = session_get_agent_name(db, parent_sid);
                    if (parent_agent) {
                        daemon_inbox_insert(parent_agent, parent_sid, "sub-agent", result_text);
                        free(parent_agent);
                    } else {
                        inbox_insert(db, parent_sid, "sub-agent", result_text);
                    }
                    /* Transition parent waiting→idle */
                    const char *wake_sql =
                        "UPDATE sessions SET state='idle' WHERE id=? AND state='waiting';";
                    sqlite3_stmt *ws;
                    if (sqlite3_prepare_v2(db, wake_sql, -1, &ws, NULL) == SQLITE_OK) {
                        sqlite3_bind_int64(ws, 1, parent_sid);
                        sqlite3_step(ws);
                        sqlite3_finalize(ws);
                    }
                    /* Signal daemon to re-fork parent */
                    daemon_signal_session(parent_sid);
                }
                entry_branch_free(branch, bcount);
                sqlite3_finalize(sq_stmt);
                continue; /* Skip normal deliver_response for sub-agents */
            }
            sqlite3_finalize(sq_stmt);
        }

        /* V26: deliver response (normal agent, not sub-agent) */
        deliver_response(cfg, db, session_id);

        /* Check if more inbox items arrived while agent was running */
        {
            char *aname = session_get_agent_name(db, session_id);
            int pending = aname ? daemon_inbox_count(aname, session_id)
                                : inbox_count(db, session_id);
            free(aname);
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

        /* V3: re-check limits before forking */
        if (session_count_active_agents(db) >= AGENT_MAX_TOTAL) {
            spawn_queue_mark(db, r->id, "rejected", 0);
            continue;
        }
        if (session_count_children(db, r->parent_session_id) >= AGENT_MAX_PER_PARENT) {
            spawn_queue_mark(db, r->id, "rejected", 0);
            continue;
        }

        /* Create child session */
        char name_buf[128];
        snprintf(name_buf, sizeof(name_buf), "sub-agent:%lld", (long long)r->parent_session_id);
        char *parent_agent = session_get_agent_name(db, r->parent_session_id);
        int64_t child_sid = session_create(db, name_buf, parent_agent,
                                           r->parent_session_id, r->depth);
        if (child_sid < 0) {
            free(parent_agent);
            spawn_queue_mark(db, r->id, "error", 0);
            continue;
        }

        /* Insert task as user message in child session inbox */
        if (parent_agent) {
            daemon_inbox_insert(parent_agent, child_sid, "spawn", r->task);
            free(parent_agent);
        } else {
            inbox_insert(db, child_sid, "spawn", r->task);
        }

        /* Fork sub-agent process */
        char session_arg[64];
        snprintf(session_arg, sizeof(session_arg), "--session-id=%lld", (long long)child_sid);
        char task_arg[4096];
        snprintf(task_arg, sizeof(task_arg), "--task=%s", r->task);
        char depth_arg[32];
        snprintf(depth_arg, sizeof(depth_arg), "--depth=%d", r->depth);

        /* Mark child session running */
        if (session_set_state(db, child_sid, "running") != 0) {
            spawn_queue_mark(db, r->id, "error", 0);
            continue;
        }

        pid_t pid = fork();
        if (pid < 0) {
            session_set_state(db, child_sid, "idle");
            spawn_queue_mark(db, r->id, "error", 0);
            continue;
        }
        if (pid == 0) {
            /* V67/T188: inject decrypted secrets as env vars for child */
            inject_secrets_for_child(db);
            char sid_arg[64];
            snprintf(sid_arg, sizeof(sid_arg), "--session-id=%lld", (long long)child_sid);
            execl(g_self_path, g_self_path, "--agent", sid_arg, (char *)NULL);
            _exit(127);
        }

        /* Track child */
        child_add(pid, child_sid);
        spawn_queue_mark(db, r->id, "forked", child_sid);

        /* V13 blocking: transition parent to "waiting" state.
         * The parent agent process should have already exited after posting the request. */
        if (!r->background) {
            const char *sql = "UPDATE sessions SET state='waiting' WHERE id=? AND state IN ('running','idle');";
            sqlite3_stmt *stmt;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(stmt, 1, r->parent_session_id);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
        }
    }
    spawn_request_free(reqs, count);
}

/* ── T94/V34: Daemon startup recovery ───────────────────────────── */

void daemon_startup_recovery(sqlite3 *db) {
    /* (1) "running" sessions: process died, re-fork to continue.
     * run_agent_turn will exit immediately if session is already complete. */
    sqlite3_stmt *run_stmt;
    const char *run_sql = "SELECT id FROM sessions WHERE state='running';";
    if (sqlite3_prepare_v2(db, run_sql, -1, &run_stmt, NULL) == SQLITE_OK) {
        int64_t running_ids[128];
        int nrunning = 0;
        while (sqlite3_step(run_stmt) == SQLITE_ROW && nrunning < 128)
            running_ids[nrunning++] = sqlite3_column_int64(run_stmt, 0);
        sqlite3_finalize(run_stmt);

        sqlite3_exec(db,
            "UPDATE sessions SET state='idle'"
            " WHERE state='running';", NULL, NULL, NULL);

        for (int i = 0; i < nrunning; i++)
            daemon_signal_session(running_ids[i]);
    }

    /* (2) "waiting" sessions: check inbox for result */
    sqlite3_stmt *stmt;
    const char *sql = "SELECT id FROM sessions WHERE state='waiting';";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return;

    /* Collect IDs first (avoid modifying while iterating) */
    int64_t waiting_ids[128];
    int nwaiting = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && nwaiting < 128) {
        waiting_ids[nwaiting++] = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);

    for (int i = 0; i < nwaiting; i++) {
        int64_t sid = waiting_ids[i];
        char *aname = session_get_agent_name(db, sid);
        int pending = aname ? daemon_inbox_count(aname, sid)
                            : inbox_count(db, sid);
        if (pending <= 0) {
            /* No result — write error tool_result to inbox */
            if (aname)
                daemon_inbox_insert(aname, sid, "recovery",
                    "error: sub-agent process lost during daemon restart");
            else
                inbox_insert(db, sid, "recovery",
                    "error: sub-agent process lost during daemon restart");
        }
        free(aname);
        /* Transition to idle (daemon loop will fork if inbox has items) */
        const char *upd = "UPDATE sessions SET state='idle'"
            " WHERE id=? AND state='waiting';";
        sqlite3_stmt *us;
        if (sqlite3_prepare_v2(db, upd, -1, &us, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(us, 1, sid);
            sqlite3_step(us);
            sqlite3_finalize(us);
        }
    }

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

    /* Create session + inbox message to trigger first turn */
    int64_t sid = session_create(db, "bootstrap", agent_name, -1, 0);
    if (sid < 0) { free(agent_name); return -1; }

    daemon_inbox_insert(agent_name, sid, "system",
                 "Welcome! Let's set up your first CClaw agent. "
                 "What LLM provider would you like to use? "
                 "(OpenRouter is recommended — just need an API key)");

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
                /* Read session_ids from signal pipe or external FIFO */
                int rfd = events[i].data.fd;
                int64_t sid;
                while (read(rfd, &sid, sizeof(sid)) == sizeof(sid)) {
                    /* Check inbox has pending items before forking */
                    char *aname = session_get_agent_name(db, sid);
                    int has_inbox = aname ? daemon_inbox_count(aname, sid) > 0
                                         : inbox_count(db, sid) > 0;
                    free(aname);
                    if (has_inbox) {
                        fork_agent(cfg, db, sid);
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
