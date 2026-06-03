#define _GNU_SOURCE
#include "daemon.h"
#include "db.h"
#include "shutdown.h"
#include "agent.h"
#include "agent_config.h"
#include "tools.h"
#include "tool_shell.h"
#include "tool_file.h"
#include "tool_js.h"
#include "tool_web_fetch.h"
#include "tool_db_query.h"
#include "tool_agent.h"
#include "tool_cron.h"
#include "config.h"
#include "context.h"
#include "agent_exit.h"
#include "log_collector.h"
#include <cJSON.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
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

/* T242/V105: daemon_wake is now in channel_api.c — removed daemon_signal_external */

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

/* ── Namespace check (T208, V85) ────────────────────────────────── */

#include <sched.h>

static int g_max_agents = DAEMON_MAX_CHILDREN;

/* V85: Read max_user_namespaces, test unshare, enforce agent limit ≤ max_ns/6 */
static void daemon_check_namespaces(void) {
    /* Read kernel limit */
    FILE *f = fopen("/proc/sys/user/max_user_namespaces", "r");
    if (!f) {
        fprintf(stderr, "[daemon] warning: cannot read /proc/sys/user/max_user_namespaces — "
                "namespace sandbox unavailable, continuing without\n");
        return;
    }
    int max_ns = 0;
    if (fscanf(f, "%d", &max_ns) != 1) max_ns = 0;
    fclose(f);

    if (max_ns <= 0) {
        fprintf(stderr, "[daemon] warning: max_user_namespaces=%d — "
                "namespace sandbox unavailable, continuing without\n", max_ns);
        return;
    }

    /* Test unshare in a forked child */
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[daemon] warning: fork failed for namespace test — "
                "continuing without namespace check\n");
        return;
    }
    if (pid == 0) {
        /* Child: test unshare and exit with result */
        _exit(unshare(CLONE_NEWUSER) == 0 ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "[daemon] warning: unshare(CLONE_NEWUSER) failed — "
                "namespace sandbox unavailable, continuing without\n");
        return;
    }

    /* Enforce: max concurrent agents ≤ max_ns / 6 */
    int ns_limit = max_ns / 6;
    if (ns_limit < g_max_agents) {
        g_max_agents = ns_limit > 0 ? ns_limit : 1;
        fprintf(stderr, "[daemon] warning: max_user_namespaces=%d constrains "
                "max concurrent agents to %d (V3 wants 10)\n", max_ns, g_max_agents);
    }
}

int daemon_get_max_agents(void) {
    return g_max_agents;
}

/* ── Child tracking (T87, T200, V24) ────────────────────────────── */

#define CHILD_MAX (DAEMON_MAX_CHILDREN + 16) /* agents + channels */
#define CHANNEL_MAX_RESTARTS 3

typedef enum { CHILD_AGENT, CHILD_CHANNEL } ChildType;

typedef struct {
    pid_t pid;
    ChildType type;
    /* Agent fields */
    int64_t session_id;
    char agent_name[64];
    /* Channel fields */
    char channel_name[64];
    char binary_path[512];
    int restart_count;
} ChildProcess;

static ChildProcess g_children[CHILD_MAX];
static int g_child_count = 0;

static ChildProcess *child_find(pid_t pid) {
    for (int i = 0; i < g_child_count; i++)
        if (g_children[i].pid == pid) return &g_children[i];
    return NULL;
}

static int child_add_agent(pid_t pid, int64_t session_id, const char *agent_name) {
    if (g_child_count >= g_max_agents) return -1;
    ChildProcess *c = &g_children[g_child_count++];
    memset(c, 0, sizeof(*c));
    c->pid = pid;
    c->type = CHILD_AGENT;
    c->session_id = session_id;
    if (agent_name) snprintf(c->agent_name, 64, "%s", agent_name);
    return 0;
}

static int child_add_channel(pid_t pid, const char *name, const char *binary_path) {
    if (g_child_count >= CHILD_MAX) return -1;
    ChildProcess *c = &g_children[g_child_count++];
    memset(c, 0, sizeof(*c));
    c->pid = pid;
    c->type = CHILD_CHANNEL;
    if (name) snprintf(c->channel_name, 64, "%s", name);
    if (binary_path) snprintf(c->binary_path, 512, "%s", binary_path);
    return 0;
}

static void child_remove(ChildProcess *c) {
    int idx = (int)(c - g_children);
    g_children[idx] = g_children[g_child_count - 1];
    g_child_count--;
}

/* V24: Check if session already has an active child */
static int child_has_session(int64_t session_id) {
    for (int i = 0; i < g_child_count; i++) {
        if (g_children[i].type == CHILD_AGENT && g_children[i].session_id == session_id)
            return 1;
    }
    return 0;
}

int64_t daemon_child_session(pid_t pid) {
    for (int i = 0; i < g_child_count; i++) {
        if (g_children[i].pid == pid) return g_children[i].session_id;
    }
    return -1;
}

/* Forward declarations for channel/reap functions defined later */
static void daemon_launch_channel(const Config *cfg, sqlite3 *db, const char *name);
static void reap_one_channel(const Config *cfg, sqlite3 *db, ChildProcess *c, int status);

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
 * Daemon is sole holder of .cclaw_key — agents read injected env vars.
 * Injects all provider API keys + primary provider config. */
static void inject_secrets_for_child(sqlite3 *db) {
    /* Inject all provider API keys from providers table */
    sqlite3_stmt *stmt;
    const char *sql = "SELECT api_key_env FROM providers WHERE api_key_env != '';";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *env_name = (const char *)sqlite3_column_text(stmt, 0);
            if (!env_name || !env_name[0]) continue;
            /* kv key is lowercase version of env var name */
            size_t len = strlen(env_name);
            char *kv_key = malloc(len + 1);
            if (!kv_key) continue;
            for (size_t i = 0; i < len; i++)
                kv_key[i] = (env_name[i] >= 'A' && env_name[i] <= 'Z')
                    ? (char)(env_name[i] + 32) : env_name[i];
            kv_key[len] = '\0';
            char *val = db_kv_get_secret(db, kv_key);
            if (val && val[0]) setenv(env_name, val, 1);
            free(val);
            free(kv_key);
        }
        sqlite3_finalize(stmt);
    }

    /* Inject primary provider config */
    char *base_url = db_kv_get(db, "provider.base_url");
    if (base_url) { setenv("CCLAW_PROVIDER_BASE_URL", base_url, 1); free(base_url); }

    char *model = db_kv_get(db, "provider.model");
    if (model) { setenv("CCLAW_MODEL", model, 1); free(model); }

    /* Determine endpoint_type from providers table for primary */
    const char *etype_sql = "SELECT endpoint_type FROM providers LIMIT 1;";
    if (sqlite3_prepare_v2(db, etype_sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *et = (const char *)sqlite3_column_text(stmt, 0);
            if (et && et[0]) setenv("CCLAW_PROVIDER_ENDPOINT_TYPE", et, 1);
        }
        sqlite3_finalize(stmt);
    }

    /* Determine which env var holds the primary API key */
    const char *keyenv_sql = "SELECT api_key_env FROM providers LIMIT 1;";
    if (sqlite3_prepare_v2(db, keyenv_sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *env = (const char *)sqlite3_column_text(stmt, 0);
            if (env && env[0]) setenv("CCLAW_PROVIDER_API_KEY_ENV", env, 1);
        }
        sqlite3_finalize(stmt);
    }

    /* Telegram token */
    char *tg_token = db_kv_get_secret(db, "telegram_token");
    if (tg_token) {
        setenv("CCLAW_INJECTED_TELEGRAM_TOKEN", tg_token, 1);
        free(tg_token);
    }
}

/* T279/V123: Inject agent config as env vars into child process.
 * If parent_ac is non-NULL, enforce ceiling: tools ⊆ parent, hosts ⊆ parent,
 * max_iterations ≤ parent. For unnamed spawn (same agent), pass NULL. */
static void inject_agent_config_env(sqlite3 *db, const char *agent_name,
                                    const AgentConfig *parent_ac) {
    AgentConfig *ac = agent_config_load_db(db, agent_name);

    /* V123: intersect with parent ceiling if provided */
    if (parent_ac && ac)
        agent_config_intersect(ac, parent_ac);

    /* Determine effective values */
    char **eff_tools = ac ? ac->tools : NULL;
    size_t eff_tool_count = ac ? ac->tool_count : 0;
    char **eff_hosts = ac ? ac->allowed_hosts : NULL;
    size_t eff_host_count = ac ? ac->allowed_hosts_count : 0;
    int eff_max_iter = ac ? ac->max_iterations : 0;

    /* If no agent config but parent ceiling given, use parent values directly */
    if (!ac && parent_ac) {
        eff_tools = parent_ac->tools;
        eff_tool_count = parent_ac->tool_count;
        eff_hosts = parent_ac->allowed_hosts;
        eff_host_count = parent_ac->allowed_hosts_count;
        eff_max_iter = parent_ac->max_iterations;
    }

    /* Inject workspace */
    if (ac && ac->workspace) {
        setenv("CCLAW_WORKSPACE", ac->workspace, 1);
    } else {
        char ws[1024];
        snprintf(ws, sizeof(ws), "agents/%s/workspace", agent_name);
        setenv("CCLAW_WORKSPACE", ws, 1);
    }

    /* Inject model */
    if (ac && ac->model)
        setenv("CCLAW_MODEL", ac->model, 1);

    /* Inject max_iterations */
    if (eff_max_iter > 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", eff_max_iter);
        setenv("CCLAW_MAX_ITERATIONS", buf, 1);
    }

    /* Inject allowed_hosts (comma-separated) */
    if (eff_host_count > 0) {
        size_t len = 0;
        for (size_t i = 0; i < eff_host_count; i++)
            len += strlen(eff_hosts[i]) + 1;
        char *hosts = malloc(len);
        if (hosts) {
            hosts[0] = '\0';
            for (size_t i = 0; i < eff_host_count; i++) {
                if (i > 0) strcat(hosts, ",");
                strcat(hosts, eff_hosts[i]);
            }
            setenv("CCLAW_ALLOWED_HOSTS", hosts, 1);
            free(hosts);
        }
    }

    /* Inject tools (comma-separated) */
    if (eff_tool_count > 0) {
        size_t len = 0;
        for (size_t i = 0; i < eff_tool_count; i++)
            len += strlen(eff_tools[i]) + 1;
        char *tools = malloc(len);
        if (tools) {
            tools[0] = '\0';
            for (size_t i = 0; i < eff_tool_count; i++) {
                if (i > 0) strcat(tools, ",");
                strcat(tools, eff_tools[i]);
            }
            setenv("CCLAW_TOOLS", tools, 1);
            free(tools);
        }
    }

    if (ac) agent_config_free(ac);
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

    /* T218/V75: Create pipes for child stdout/stderr → log collector */
    int out_pipe[2] = {-1, -1}, err_pipe[2] = {-1, -1};
    (void)pipe(out_pipe);
    (void)pipe(err_pipe);

    pid_t pid = fork();
    if (pid < 0) {
        daemon_agent_set_state(agent_name, session_id, "idle");
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        free(resolved_name);
        return -1;
    }
    if (pid == 0) {
        /* Child: redirect stdout/stderr to pipe write ends */
        close(out_pipe[0]);
        close(err_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[1]);
        close(err_pipe[1]);
        /* V67/T188: inject decrypted secrets as env vars for child */
        inject_secrets_for_child(db);

        /* V73/V74,T198: inject agent identity + DB path + config as env vars */
        if (agent_name) {
            setenv("CCLAW_AGENT_NAME", agent_name, 1);
            char agent_db[1024];
            snprintf(agent_db, sizeof(agent_db), "agents/%s/agent.db", agent_name);
            setenv("CCLAW_AGENT_DB", agent_db, 1);
            /* T279: inject config (no ceiling for direct fork) */
            inject_agent_config_env(db, agent_name, NULL);
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

    /* Parent: close write ends, send read ends to log collector */
    close(out_pipe[1]);
    close(err_pipe[1]);
    log_collector_send_fd(out_pipe[0], agent_name, pid, session_id, 1);
    log_collector_send_fd(err_pipe[0], agent_name, pid, session_id, 2);
    close(out_pipe[0]);
    close(err_pipe[0]);

    /* Parent: track child (T200: includes agent_name) */
    child_add_agent(pid, session_id, agent_name);
    free(resolved_name);
    return 0;
}

/* ── Response delivery (T85, V26) ──────────────────────────────── */

/* T200/V73: deliver_response reads from agent DB */
static void deliver_response(const Config *cfg, sqlite3 *db,
                             const char *agent_name, int64_t session_id) {
    (void)cfg;
    /* Open agent DB to read response */
    char *path = agent_db_path(agent_name);
    if (!path) return;
    sqlite3 *adb = db_open_agent(path);
    free(path);
    if (!adb) return;

    char *reply = get_response_text(adb, session_id);
    char *route = session_get_last_route(adb, session_id);
    db_close(adb);

    if (!reply) {
        free(route);
        return;
    }

    /* V42/T110: Suppress HEARTBEAT_OK sentinel — never deliver to channel */
    if (strcmp(reply, "HEARTBEAT_OK") == 0) {
        free(reply);
        free(route);
        return;
    }

    /* V44/T113: Suppress [NO_REPLY] — agent decided not to respond (e.g. group irrelevance) */
    if (strstr(reply, "[NO_REPLY]") != NULL) {
        free(reply);
        free(route);
        return;
    }

    if (route && strncmp(route, "telegram", 8) == 0) {
        /* T247: Telegram delivery now via channel_outbox (channel process picks up).
         * Resolve chat_id and build outbox payload with chat_id for channel process. */
        const char *sql = "SELECT chat_id FROM tg_chat_sessions WHERE session_id=?;";
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, session_id);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                int64_t chat_id = sqlite3_column_int64(stmt, 0);
                /* Build JSON payload for channel_telegram outbox */
                cJSON *opj = cJSON_CreateObject();
                cJSON_AddNumberToObject(opj, "chat_id", (double)chat_id);
                cJSON_AddStringToObject(opj, "text", reply);
                char *opayload = cJSON_PrintUnformatted(opj);
                cJSON_Delete(opj);
                if (opayload) {
                    const char *osql =
                        "INSERT INTO channel_outbox (channel_name, session_id, payload)"
                        " VALUES ('telegram',?,?);";
                    sqlite3_stmt *ostmt;
                    if (sqlite3_prepare_v2(db, osql, -1, &ostmt, NULL) == SQLITE_OK) {
                        sqlite3_bind_int64(ostmt, 1, session_id);
                        sqlite3_bind_text(ostmt, 2, opayload, -1, SQLITE_STATIC);
                        sqlite3_step(ostmt);
                        sqlite3_finalize(ostmt);
                    }
                    free(opayload);
                }
            }
            sqlite3_finalize(stmt);
        }
    }

    /* T246/V101: Insert channel_outbox row for non-telegram channel processes.
     * Skip "cli" (polls agent DB directly) and "telegram" (handled above with chat_id). */
    if (route && strcmp(route, "cli") != 0 && strcmp(route, "telegram") != 0) {
        const char *osql =
            "INSERT INTO channel_outbox (channel_name, session_id, payload)"
            " VALUES (?,?,?);";
        sqlite3_stmt *ostmt;
        if (sqlite3_prepare_v2(db, osql, -1, &ostmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(ostmt, 1, route, -1, SQLITE_STATIC);
            sqlite3_bind_int64(ostmt, 2, session_id);
            sqlite3_bind_text(ostmt, 3, reply, -1, SQLITE_STATIC);
            sqlite3_step(ostmt);
            sqlite3_finalize(ostmt);
        }
    }

    free(reply);
    free(route);
}

/* ── T201: PENDING entry helpers (V77/V78/V79) ─────────────────── */

/* Find tool_result entry with content='PENDING' in agent DB for given session.
 * Returns tool_call_id (caller frees) or NULL. Sets *entry_id. */
/* Find PENDING tool_result entry. Returns tool_call_id (caller frees), sets *entry_id. */
char *find_pending_entry(sqlite3 *adb, int64_t session_id, int64_t *entry_id) {
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
char *read_tool_call_args(sqlite3 *adb, int64_t session_id,
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
int update_pending_entry(sqlite3 *adb, int64_t entry_id, const char *result) {
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
        /* V67: Store provider config in cclaw.db */
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
        /* T251/V104/V79: Insert channels row + seed channel_state + launch */
        cJSON *channel_type = cJSON_GetObjectItemCaseSensitive(args, "channel_type");
        cJSON *bot_token = cJSON_GetObjectItemCaseSensitive(args, "bot_token");
        cJSON *binary_path = cJSON_GetObjectItemCaseSensitive(args, "binary_path");
        cJSON *config_obj = cJSON_GetObjectItemCaseSensitive(args, "config");

        if (!cJSON_IsString(channel_type)) {
            cJSON_Delete(args);
            return strdup("error: channel_type required");
        }

        const char *ctype = channel_type->valuestring;
        if (strcmp(ctype, "telegram") == 0) {
            const char *token = (cJSON_IsString(bot_token) && bot_token->valuestring[0])
                                ? bot_token->valuestring : NULL;
            if (!token) {
                cJSON_Delete(args);
                return strdup("error: bot_token required for telegram");
            }
            /* Registers channels row + seeds channel_state + sets active */
            daemon_register_telegram_channel(db, token);
            /* V69: Bind channel to requesting agent */
            db_channel_binding_set(db, "telegram", "default", agent_name);
            /* Launch the channel process */
            daemon_launch_channel(cfg, db, "telegram");
            result = strdup("channel configured and launched: telegram");
        } else if (strcmp(ctype, "cli") == 0) {
            db_channel_binding_set(db, "cli", "default", agent_name);
            result = strdup("channel configured: cli");
        } else {
            /* Custom channel — requires binary_path */
            if (!cJSON_IsString(binary_path) || !binary_path->valuestring[0]) {
                cJSON_Delete(args);
                return strdup("error: binary_path required for custom channel");
            }
            /* Insert channels row */
            const char *isql =
                "INSERT OR REPLACE INTO channels(name, type, binary_path, status)"
                " VALUES(?, ?, ?, 'active');";
            sqlite3_stmt *cstmt;
            if (sqlite3_prepare_v2(db, isql, -1, &cstmt, NULL) == SQLITE_OK) {
                sqlite3_bind_text(cstmt, 1, ctype, -1, SQLITE_STATIC);
                sqlite3_bind_text(cstmt, 2, ctype, -1, SQLITE_STATIC);
                sqlite3_bind_text(cstmt, 3, binary_path->valuestring, -1, SQLITE_STATIC);
                sqlite3_step(cstmt);
                sqlite3_finalize(cstmt);
            }
            /* Seed channel_state from config object */
            if (cJSON_IsObject(config_obj)) {
                cJSON *kv = NULL;
                cJSON_ArrayForEach(kv, config_obj) {
                    if (cJSON_IsString(kv) && kv->string) {
                        const char *ksql =
                            "INSERT OR REPLACE INTO channel_state(channel_name, key, value)"
                            " VALUES(?, ?, ?);";
                        sqlite3_stmt *ks;
                        if (sqlite3_prepare_v2(db, ksql, -1, &ks, NULL) == SQLITE_OK) {
                            sqlite3_bind_text(ks, 1, ctype, -1, SQLITE_STATIC);
                            sqlite3_bind_text(ks, 2, kv->string, -1, SQLITE_STATIC);
                            sqlite3_bind_text(ks, 3, kv->valuestring, -1, SQLITE_STATIC);
                            sqlite3_step(ks);
                            sqlite3_finalize(ks);
                        }
                    }
                }
            }
            /* V69: Bind channel to requesting agent */
            db_channel_binding_set(db, ctype, "default", agent_name);
            /* Launch the channel process */
            daemon_launch_channel(cfg, db, ctype);
            char buf[256];
            snprintf(buf, sizeof(buf), "channel configured and launched: %s", ctype);
            result = strdup(buf);
        }

    } else if (strcmp(tool_name, "create_agent") == 0) {
        /* V79/V68: Create new agent dir + agent.db + cclaw.db rows */
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

    } else if (strcmp(tool_name, "rename_agent") == 0) {
        /* T230: Rename agent directory + update registry */
        cJSON *new_name = cJSON_GetObjectItemCaseSensitive(args, "name");
        if (!cJSON_IsString(new_name) || !new_name->valuestring[0]) {
            cJSON_Delete(args);
            return strdup("error: 'name' required");
        }
        const char *nn = new_name->valuestring;

        /* Validate name (alphanumeric + dash + underscore) */
        for (const char *p = nn; *p; p++) {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
                  *p == '-' || *p == '_')) {
                cJSON_Delete(args);
                return strdup("error: name must be [a-z0-9_-]");
            }
        }

        /* Build paths */
        const char *home = getenv("HOME");
        if (!home) home = ".";
        char old_path[PATH_MAX], new_path[PATH_MAX];
        snprintf(old_path, sizeof(old_path), "%s/.cclaw/agents/%s", home, agent_name);
        snprintf(new_path, sizeof(new_path), "%s/.cclaw/agents/%s", home, nn);

        /* Check target doesn't exist */
        struct stat st;
        if (stat(new_path, &st) == 0) {
            cJSON_Delete(args);
            return strdup("error: agent name already taken");
        }

        /* Rename directory */
        if (rename(old_path, new_path) != 0) {
            cJSON_Delete(args);
            char buf[256];
            snprintf(buf, sizeof(buf), "error: rename failed (errno=%d)", errno);
            return strdup(buf);
        }

        /* Update cclaw.db agents registry */
        const char *upd = "UPDATE agents SET name=? WHERE name=?;";
        sqlite3_stmt *us;
        if (sqlite3_prepare_v2(db, upd, -1, &us, NULL) == SQLITE_OK) {
            sqlite3_bind_text(us, 1, nn, -1, SQLITE_STATIC);
            sqlite3_bind_text(us, 2, agent_name, -1, SQLITE_STATIC);
            sqlite3_step(us);
            sqlite3_finalize(us);
        }

        /* Update agent_config rows */
        const char *ucfg = "UPDATE agent_config SET agent_name=? WHERE agent_name=?;";
        if (sqlite3_prepare_v2(db, ucfg, -1, &us, NULL) == SQLITE_OK) {
            sqlite3_bind_text(us, 1, nn, -1, SQLITE_STATIC);
            sqlite3_bind_text(us, 2, agent_name, -1, SQLITE_STATIC);
            sqlite3_step(us);
            sqlite3_finalize(us);
        }

        /* Update CLI binding */
        db_kv_set(db, "cli.agent", nn);

        char buf[256];
        snprintf(buf, sizeof(buf), "agent renamed: %s → %s", agent_name, nn);
        result = strdup(buf);

    } else if (strcmp(tool_name, "request_config") == 0) {
        /* T274/V120: grant_tool or grant_host */
        cJSON *action = cJSON_GetObjectItemCaseSensitive(args, "action");
        if (!cJSON_IsString(action)) {
            cJSON_Delete(args);
            return strdup("error: 'action' required");
        }
        if (strcmp(action->valuestring, "grant_tool") == 0) {
            cJSON *tool = cJSON_GetObjectItemCaseSensitive(args, "tool");
            if (!cJSON_IsString(tool) || !tool->valuestring[0]) {
                cJSON_Delete(args);
                return strdup("error: 'tool' required");
            }
            if (agent_config_add_tool(db, agent_name, tool->valuestring) == 0) {
                char buf2[128];
                snprintf(buf2, sizeof(buf2), "granted tool: %s", tool->valuestring);
                result = strdup(buf2);
            } else {
                result = strdup("error: failed to grant tool");
            }
        } else if (strcmp(action->valuestring, "grant_host") == 0) {
            cJSON *host = cJSON_GetObjectItemCaseSensitive(args, "host");
            if (!cJSON_IsString(host) || !host->valuestring[0]) {
                cJSON_Delete(args);
                return strdup("error: 'host' required");
            }
            if (agent_config_add_host(db, agent_name, host->valuestring) == 0) {
                char buf2[128];
                snprintf(buf2, sizeof(buf2), "granted host: %s", host->valuestring);
                result = strdup(buf2);
            } else {
                result = strdup("error: failed to grant host");
            }
        } else {
            result = strdup("error: action must be grant_tool or grant_host");
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
        ChildProcess *c = child_find(pid);
        if (!c) continue; /* Unknown child — ignore */

        if (c->type == CHILD_CHANNEL) {
            reap_one_channel(cfg, db, c, status);
            continue;
        }

        /* Agent process */
        int64_t session_id = c->session_id;
        char agent_name[64];
        snprintf(agent_name, 64, "%s", c->agent_name);
        child_remove(c);

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
                            /* Insert into cclaw.db spawn_queue */
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
                            /* T203: Insert into cclaw.db approvals w/ tool_call_id */
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
                            /* T205/V79: Daemon applies config to cclaw.db */
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
                            result_buf = get_response_text(cadb, session_id);
                            result_text = result_buf;
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

        /* Resolve parent agent name + child_agent from spawn_queue */
        const char *parent_agent_name = NULL;
        const char *child_agent_name = NULL;
        char pa_buf[64] = "";
        char ca_buf[64] = "";
        {
            const char *pa_sql = "SELECT parent_agent, child_agent FROM spawn_queue WHERE id=?;";
            sqlite3_stmt *pa_stmt;
            if (sqlite3_prepare_v2(db, pa_sql, -1, &pa_stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(pa_stmt, 1, r->id);
                if (sqlite3_step(pa_stmt) == SQLITE_ROW) {
                    const char *v = (const char *)sqlite3_column_text(pa_stmt, 0);
                    if (v && v[0]) snprintf(pa_buf, sizeof(pa_buf), "%s", v);
                    const char *c = (const char *)sqlite3_column_text(pa_stmt, 1);
                    if (c && c[0]) snprintf(ca_buf, sizeof(ca_buf), "%s", c);
                }
                sqlite3_finalize(pa_stmt);
            }
            parent_agent_name = pa_buf[0] ? pa_buf : NULL;
            child_agent_name = ca_buf[0] ? ca_buf : NULL;
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

        /* T218/V75: pipes for sub-agent stdout/stderr */
        int sa_out[2] = {-1,-1}, sa_err[2] = {-1,-1};
        (void)pipe(sa_out);
        (void)pipe(sa_err);

        pid_t pid = fork();
        if (pid < 0) {
            close(sa_out[0]); close(sa_out[1]);
            close(sa_err[0]); close(sa_err[1]);
            daemon_agent_set_state(parent_agent_name, child_sid, "idle");
            spawn_queue_mark(db, r->id, "error", 0);
            continue;
        }
        if (pid == 0) {
            close(sa_out[0]); close(sa_err[0]);
            dup2(sa_out[1], STDOUT_FILENO);
            dup2(sa_err[1], STDERR_FILENO);
            close(sa_out[1]); close(sa_err[1]);
            /* V67/T188: inject decrypted secrets as env vars for child */
            inject_secrets_for_child(db);

            /* T279/V123: load parent config as privilege ceiling */
            AgentConfig *parent_ac = agent_config_load_db(db, parent_agent_name);

            /* Determine effective agent identity for sub-agent */
            const char *eff_agent = child_agent_name ? child_agent_name : parent_agent_name;
            setenv("CCLAW_AGENT_NAME", eff_agent, 1);
            char agent_db_buf[1024];
            snprintf(agent_db_buf, sizeof(agent_db_buf), "agents/%s/agent.db", eff_agent);
            setenv("CCLAW_AGENT_DB", agent_db_buf, 1);

            /* T279: inject config with parent ceiling enforcement */
            inject_agent_config_env(db, eff_agent, parent_ac);

            if (parent_ac) agent_config_free(parent_ac);

            /* Inject global config baseline */
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
                if (cfg->shell_timeout > 0) {
                    snprintf(buf, sizeof(buf), "%d", cfg->shell_timeout);
                    setenv("CCLAW_SHELL_TIMEOUT", buf, 0);
                }
            }

            char sid_arg[64];
            snprintf(sid_arg, sizeof(sid_arg), "--session-id=%lld", (long long)child_sid);
            execl(g_self_path, g_self_path, "--agent", sid_arg, (char *)NULL);
            _exit(127);
        }

        /* Parent: send pipe read ends to log collector */
        close(sa_out[1]); close(sa_err[1]);
        log_collector_send_fd(sa_out[0], parent_agent_name, pid, child_sid, 1);
        log_collector_send_fd(sa_err[0], parent_agent_name, pid, child_sid, 2);
        close(sa_out[0]); close(sa_err[0]);

        /* Track child (T200: includes agent_name) */
        child_add_agent(pid, child_sid, parent_agent_name);
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
    (void)db; /* cclaw.db not used for session state anymore */

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

/* T196: Migrate agent.json files to cclaw.db agent_config table */
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

    /* T196: Write bootstrap config to cclaw.db agent_config table */
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

/* T247/V103: Register telegram channel in channels table + seed config.
 * Ensures channel_telegram binary is launched by daemon_launch_channels. */
void daemon_register_telegram_channel(sqlite3 *db, const char *token) {
    /* Resolve binary path relative to self */
    char self_path[256] = {0};
    ssize_t n = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (n <= 0) return;
    self_path[n] = '\0';
    /* Replace last component with channel_telegram */
    char *slash = strrchr(self_path, '/');
    if (!slash) return;
    slash[1] = '\0';
    char bin_path[512];
    snprintf(bin_path, sizeof(bin_path), "%s%s", self_path, "channel_telegram");

    /* INSERT OR IGNORE — don't overwrite if already registered */
    const char *sql =
        "INSERT OR IGNORE INTO channels(name, type, binary_path)"
        " VALUES('telegram', 'telegram', ?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, bin_path, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    /* Ensure status is active */
    const char *usql = "UPDATE channels SET status='active' WHERE name='telegram';";
    sqlite3_exec(db, usql, NULL, NULL, NULL);

    /* Seed bot_token into channel_state */
    const char *ksql =
        "INSERT OR REPLACE INTO channel_state(channel_name, key, value)"
        " VALUES('telegram', 'bot_token', ?);";
    if (sqlite3_prepare_v2(db, ksql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, token, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

/* ── T244/V98/V104: Channel process launcher ───────────────────── */

/* Forward declarations */
static pid_t channel_fork(const char *binary_path, const char *db_path, const char *name);
static void channel_update_pid(sqlite3 *db, const char *name, pid_t pid);

/* T251: Launch a single channel by name (reads from channels table) */
static void daemon_launch_channel(const Config *cfg, sqlite3 *db, const char *name) {
    if (!cfg || !cfg->db_path || !cfg->db_path[0]) return;
    /* Check if already running */
    for (int i = 0; i < g_child_count; i++) {
        if (g_children[i].type == CHILD_CHANNEL &&
            strcmp(g_children[i].channel_name, name) == 0)
            return; /* Already tracked */
    }

    const char *sql = "SELECT binary_path FROM channels WHERE name=? AND status='active';";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return; }
    const char *bpath = (const char *)sqlite3_column_text(stmt, 0);
    if (!bpath) { sqlite3_finalize(stmt); return; }

    char bin_path[512];
    snprintf(bin_path, sizeof(bin_path), "%s", bpath);
    sqlite3_finalize(stmt);

    pid_t pid = channel_fork(bin_path, cfg->db_path, name);
    if (pid <= 0) return;

    child_add_channel(pid, name, bin_path);
    channel_update_pid(db, name, pid);
    fprintf(stderr, "[daemon] launched channel '%s' (pid %d)\n", name, (int)pid);
}

/* Fork a channel binary. Returns pid or -1. */
static pid_t channel_fork(const char *binary_path, const char *db_path, const char *name) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl(binary_path, binary_path, db_path, name, (char *)NULL);
        _exit(127);
    }
    return pid;
}

/* Update channels.pid in cclaw.db */
static void channel_update_pid(sqlite3 *db, const char *name, pid_t pid) {
    const char *sql = "UPDATE channels SET pid=? WHERE name=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, (int)pid);
        sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

static void channel_set_status(sqlite3 *db, const char *name, const char *status) {
    const char *sql = "UPDATE channels SET status=? WHERE name=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, status, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

/* Launch all active channels from DB */
static void daemon_launch_channels(const Config *cfg, sqlite3 *db) {
    const char *sql = "SELECT name, binary_path FROM channels WHERE status='active';";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        const char *bpath = (const char *)sqlite3_column_text(stmt, 1);
        if (!name || !bpath) continue;

        pid_t pid = channel_fork(bpath, cfg->db_path, name);
        if (pid > 0) {
            child_add_channel(pid, name, bpath);
            channel_update_pid(db, name, pid);
            fprintf(stderr, "[daemon] launched channel '%s' (pid %d)\n", name, (int)pid);
        } else {
            fprintf(stderr, "[daemon] failed to launch channel '%s'\n", name);
        }
    }
    sqlite3_finalize(stmt);
}

/* Handle death of a channel process */
static void reap_one_channel(const Config *cfg, sqlite3 *db, ChildProcess *c, int status) {
    fprintf(stderr, "[daemon] channel '%s' (pid %d) exited (status %d)\n",
            c->channel_name, (int)c->pid,
            WIFEXITED(status) ? WEXITSTATUS(status) : -1);

    /* V98: restart immediately, count attempts, mark failed at max */
    if (c->restart_count >= CHANNEL_MAX_RESTARTS) {
        fprintf(stderr, "[daemon] channel '%s' exceeded max restarts, marking failed\n",
                c->channel_name);
        channel_set_status(db, c->channel_name, "failed");
        channel_update_pid(db, c->channel_name, 0);
        child_remove(c);
        return;
    }

    c->restart_count++;
    pid_t new_pid = channel_fork(c->binary_path, cfg->db_path, c->channel_name);
    if (new_pid > 0) {
        c->pid = new_pid;
        channel_update_pid(db, c->channel_name, new_pid);
        fprintf(stderr, "[daemon] restarted channel '%s' (pid %d, attempt %d)\n",
                c->channel_name, (int)new_pid, c->restart_count);
    } else {
        fprintf(stderr, "[daemon] failed to restart channel '%s'\n", c->channel_name);
        child_remove(c);
    }
}

/* ── T245/V100/V106: Channel events consumer ──────────────────── */

static void consume_channel_events(const Config *cfg, sqlite3 *db) {
    const char *sql =
        "SELECT id, channel_name, event_type, payload"
        " FROM channel_events ORDER BY id ASC;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t eid = sqlite3_column_int64(stmt, 0);
        const char *ch_name = (const char *)sqlite3_column_text(stmt, 1);
        const char *etype = (const char *)sqlite3_column_text(stmt, 2);
        const char *payload = (const char *)sqlite3_column_text(stmt, 3);

        if (!ch_name || !payload) goto delete_event;

        /* Only handle "message" events for now */
        if (!etype || strcmp(etype, "message") != 0) goto delete_event;

        /* Resolve agent via channel_bindings.
         * Payload is JSON: {"channel_id":"...", "text":"...", ...}
         * channel_id used for binding lookup + session routing. */
        {
            cJSON *pj = cJSON_Parse(payload);
            const char *channel_id = NULL;
            const char *text = NULL;
            if (pj) {
                cJSON *cid = cJSON_GetObjectItem(pj, "channel_id");
                cJSON *txt = cJSON_GetObjectItem(pj, "text");
                if (cJSON_IsString(cid)) channel_id = cid->valuestring;
                if (cJSON_IsString(txt)) text = txt->valuestring;
            }
            if (!channel_id || !text) { cJSON_Delete(pj); goto delete_event; }

            /* Look up agent binding for this channel_type + channel_id */
            char *agent_name = db_channel_binding_get(db, ch_name, channel_id);
            if (!agent_name) {
                /* Fallback: try "default" binding for this channel type */
                agent_name = db_channel_binding_get(db, ch_name, "default");
            }
            if (!agent_name) { cJSON_Delete(pj); goto delete_event; }

            /* Find or create session for this channel+channel_id.
             * Use tg_chat_sessions pattern: channel_type:channel_id → session_id.
             * Store in channel_state as "session:<channel_id>" key. */
            int64_t session_id = -1;
            {
                char sess_key[128];
                snprintf(sess_key, sizeof(sess_key), "session:%s", channel_id);
                /* Read from channel_state */
                const char *ssql =
                    "SELECT value FROM channel_state WHERE channel_name=? AND key=?;";
                sqlite3_stmt *ss;
                if (sqlite3_prepare_v2(db, ssql, -1, &ss, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(ss, 1, ch_name, -1, SQLITE_STATIC);
                    sqlite3_bind_text(ss, 2, sess_key, -1, SQLITE_STATIC);
                    if (sqlite3_step(ss) == SQLITE_ROW) {
                        const char *v = (const char *)sqlite3_column_text(ss, 0);
                        if (v) session_id = strtoll(v, NULL, 10);
                    }
                    sqlite3_finalize(ss);
                }

                if (session_id < 0) {
                    /* Create new session in agent DB */
                    char *apath = agent_db_path(agent_name);
                    if (apath) {
                        sqlite3 *adb = db_open_agent(apath);
                        free(apath);
                        if (adb) {
                            char sname[128];
                            snprintf(sname, sizeof(sname), "%s_%s", ch_name, channel_id);
                            session_id = session_create(adb, sname, agent_name, -1, 0);
                            db_close(adb);
                        }
                    }
                    if (session_id > 0) {
                        /* Store mapping */
                        char sid_str[32];
                        snprintf(sid_str, sizeof(sid_str), "%lld", (long long)session_id);
                        const char *isql =
                            "INSERT OR REPLACE INTO channel_state(channel_name, key, value)"
                            " VALUES(?,?,?);";
                        sqlite3_stmt *is;
                        if (sqlite3_prepare_v2(db, isql, -1, &is, NULL) == SQLITE_OK) {
                            sqlite3_bind_text(is, 1, ch_name, -1, SQLITE_STATIC);
                            sqlite3_bind_text(is, 2, sess_key, -1, SQLITE_STATIC);
                            sqlite3_bind_text(is, 3, sid_str, -1, SQLITE_STATIC);
                            sqlite3_step(is);
                            sqlite3_finalize(is);
                        }
                    }
                }
            }

            if (session_id > 0) {
                daemon_inbox_insert(agent_name, session_id, ch_name, text);
                daemon_signal_session_agent(session_id, agent_name);
            }

            free(agent_name);
            cJSON_Delete(pj);
        }

delete_event:
        /* Delete consumed event */
        {
            const char *dsql = "DELETE FROM channel_events WHERE id=?;";
            sqlite3_stmt *ds;
            if (sqlite3_prepare_v2(db, dsql, -1, &ds, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(ds, 1, eid);
                sqlite3_step(ds);
                sqlite3_finalize(ds);
            }
        }
    }
    sqlite3_finalize(stmt);
    (void)cfg;
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

    /* T218/V75: Start log collector process */
    {
        char journal_path[1024];
        const char *dbp = cfg->db_path ? cfg->db_path : ".cclaw/daemon.db";
        /* journal.db lives next to daemon.db */
        const char *slash = strrchr(dbp, '/');
        if (slash) {
            int dirlen = (int)(slash - dbp);
            snprintf(journal_path, sizeof(journal_path), "%.*s/journal.db", dirlen, dbp);
        } else {
            snprintf(journal_path, sizeof(journal_path), "journal.db");
        }
        if (log_collector_start(journal_path) != 0) {
            fprintf(stderr, "daemon: warning: log collector failed to start\n");
        }
    }

    /* T208/V85: Check namespace support, clamp max agents */
    daemon_check_namespaces();

    /* T244/V98/V104: Launch configured channel processes */
    daemon_launch_channels(cfg, db);

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
            } else if (events[i].data.fd == g_signal_pipe[0]) {
                /* T200: Read SignalMsg from internal signal pipe */
                SignalMsg msg;
                while (read(g_signal_pipe[0], &msg, sizeof(msg)) == sizeof(msg)) {
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
            } else if (events[i].data.fd == fifo_fd) {
                /* T244/T245/V105: External FIFO wake — drain bytes, consume channel_events */
                char drain[64];
                while (read(fifo_fd, drain, sizeof(drain)) > 0) {}
                consume_channel_events(cfg, db);
                process_spawn_queue(cfg, db);
            }
        }

        /* Also reap on timeout (defensive) */
        if (nfds == 0 && g_child_count > 0) {
            reap_children(cfg, db);
        }
    }

    /* V31: Forward SIGTERM to children */
    /* V31: Forward SIGTERM to all children (agents + channels) */
    for (int i = 0; i < g_child_count; i++) {
        if (g_children[i].pid > 0)
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
    log_collector_stop(); /* T218/V75 */
    g_child_count = 0;
    return 0;
}
