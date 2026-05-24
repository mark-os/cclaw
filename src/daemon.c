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
#include "tool_subagent.h"
#include "tool_cron.h"
#include "landlock.h"
#include "config.h"

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

void daemon_signal_close(void) {
    if (g_signal_pipe[0] >= 0) { close(g_signal_pipe[0]); g_signal_pipe[0] = -1; }
    if (g_signal_pipe[1] >= 0) { close(g_signal_pipe[1]); g_signal_pipe[1] = -1; }
}

int daemon_signal_fd(void) {
    return g_signal_pipe[0];
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

static char *dispatch_tools_daemon(const char *name, const char *arguments, void *user_data) {
    ToolRegistry *reg = (ToolRegistry *)user_data;
    ToolEntry *e = tools_lookup(reg, name);
    if (!e) {
        char *err = malloc(128);
        if (err) snprintf(err, 128, "error: unknown tool '%s'", name);
        return err;
    }
    return e->handler(arguments, e->user_data);
}

static void agent_process_entry(const Config *cfg, int64_t session_id) {
    /* V34: die if daemon dies */
    prctl(PR_SET_PDEATHSIG, SIGTERM);

    /* V31: re-init signal handlers in child (inherited but be explicit) */
    shutdown_init();

    /* V23: resource limits */
    struct rlimit rl;
    rl.rlim_cur = 256 * 1024 * 1024;
    rl.rlim_max = 256 * 1024 * 1024;
    setrlimit(RLIMIT_AS, &rl);
    rl.rlim_cur = 300;
    rl.rlim_max = 300;
    setrlimit(RLIMIT_CPU, &rl);
    rl.rlim_cur = 64;
    rl.rlim_max = 64;
    setrlimit(RLIMIT_NOFILE, &rl);

    /* Open own DB connection (fork-safe) */
    sqlite3 *db = db_open(cfg->db_path);
    if (!db) _exit(1);

    /* V20: Load per-agent config from disk if session has agent_name */
    char *agent_name = session_get_agent_name(db, session_id);
    const Config *effective_cfg = cfg;
    Config *merged_cfg = NULL;
    AgentConfig *ac = NULL;
    if (agent_name) {
        ac = agent_config_load("agents", agent_name);
        merged_cfg = agent_config_merge(cfg, ac);
        if (merged_cfg) effective_cfg = merged_cfg;
    }

    /* V22: Landlock — restrict filesystem after config loaded */
    if (landlock_apply(effective_cfg->workspace, cfg->db_path) < 0) {
        fprintf(stderr, "[agent %lld] landlock unavailable, continuing without\n",
                (long long)session_id);
    }

    /* V27: Update last_route from newest inbox source before consuming */
    int peek_count = 0;
    InboxItem *items = inbox_peek(db, session_id, 100, &peek_count);
    if (items && peek_count > 0) {
        session_set_last_route(db, session_id, items[peek_count - 1].source);
        inbox_items_free(items, peek_count);
    }

    /* V18: Drain inbox into session entries */
    inbox_consume_into_entries(db, session_id, 100);

    /* Register tools */
    ToolRegistry reg;
    tools_init(&reg);
    tool_shell_register(&reg, effective_cfg->shell_timeout,
                        effective_cfg->workspace, ac ? ac->shell_network : 0);
    tool_file_read_register(&reg, effective_cfg->workspace);
    tool_file_write_register(&reg, effective_cfg->workspace);
    tool_js_eval_register(&reg);
    tool_web_fetch_register(&reg);
    tool_db_query_register(&reg, db);

    char self_path[4096];
    ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (len > 0) self_path[len] = '\0';
    else strcpy(self_path, "./build/cclaw");

    SubAgentCtx sa_ctx = {.db = db, .session_id = session_id,
                          .depth = 0, .self_path = self_path,
                          .daemon_mode = 1, .tool_call_id = NULL};
    tool_spawn_agent_register(&reg, &sa_ctx);
    tool_check_agent_register(&reg, &sa_ctx);

    ToolCronCtx cron_ctx = {.db = db, .session_id = session_id};
    tool_cron_register(&reg, &cron_ctx);

    size_t tool_count = 0;
    const ToolSchema *schemas = tools_schemas(&reg, &tool_count);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = session_id;
    ctx.cfg = effective_cfg;
    ctx.dispatch = dispatch_tools_daemon;
    ctx.dispatch_data = &reg;
    ctx.tools = schemas;
    ctx.tool_count = tool_count;
    ctx.debug = effective_cfg->debug;

    int rc = agent_run(&ctx);

    tools_free(&reg);
    agent_config_free(ac);
    if (merged_cfg) config_free(merged_cfg);
    free(agent_name);
    db_close(db);
    _exit(rc == 0 ? 0 : 1);
}

/* ── Fork agent (V21, V24) ─────────────────────────────────────── */

static int fork_agent(const Config *cfg, sqlite3 *db, int64_t session_id) {
    /* V24: only fork if state == idle */
    if (child_has_session(session_id)) return -1;

    /* Acquire session lock (idle→running) */
    char holder[32];
    snprintf(holder, sizeof(holder), "daemon-%d", (int)getpid());
    if (session_try_acquire(db, session_id, holder) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        session_release(db, session_id, holder);
        return -1;
    }
    if (pid == 0) {
        /* Child: close daemon fds, run agent */
        close(g_signal_pipe[0]);
        close(g_signal_pipe[1]);
        close(g_chld_pipe[0]);
        close(g_chld_pipe[1]);
        agent_process_entry(cfg, session_id);
        _exit(1); /* unreachable */
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

        /* Release session lock */
        char holder[32];
        snprintf(holder, sizeof(holder), "daemon-%d", (int)getpid());
        session_release(db, session_id, holder);

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
                const char *tcid = (const char *)sqlite3_column_text(sq_stmt, 2);
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
                    char *payload = malloc(strlen(result_text) + 128);
                    if (payload) {
                        if (tcid) {
                            snprintf(payload, strlen(result_text) + 128,
                                     "{\"tool_call_id\":\"%s\",\"result\":\"%s\"}", tcid, "see_content");
                        }
                        /* Post plain result as inbox message */
                        inbox_insert(db, parent_sid, "sub-agent", result_text);
                        free(payload);
                    } else {
                        inbox_insert(db, parent_sid, "sub-agent", result_text);
                    }
                    /* Transition parent waiting→idle */
                    const char *wake_sql =
                        "UPDATE sessions SET state='idle', lock_holder=NULL WHERE id=? AND state='waiting';";
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
        int pending = inbox_count(db, session_id);
        if (pending > 0) {
            daemon_signal_session(session_id);
        }
    }
}

/* ── T88: Process spawn queue ───────────────────────────────────── */

static void process_spawn_queue(const Config *cfg, sqlite3 *db) {
    int count = 0;
    SpawnRequest *reqs = spawn_queue_peek_pending(db, &count);
    if (!reqs) return;

    char self_path[4096];
    ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (len > 0) self_path[len] = '\0';
    else strcpy(self_path, "./build/cclaw");

    for (int i = 0; i < count; i++) {
        SpawnRequest *r = &reqs[i];

        /* V3: re-check limits before forking */
        if (subagent_count_total(db) >= SUBAGENT_MAX_TOTAL) {
            spawn_queue_mark(db, r->id, "rejected", 0);
            continue;
        }
        if (subagent_count_by_parent(db, r->parent_session_id) >= SUBAGENT_MAX_PER_PARENT) {
            spawn_queue_mark(db, r->id, "rejected", 0);
            continue;
        }

        /* Create child session */
        char name_buf[128];
        snprintf(name_buf, sizeof(name_buf), "sub-agent:%lld", (long long)r->parent_session_id);
        int64_t child_sid = session_create(db, name_buf, NULL);
        if (child_sid < 0) {
            spawn_queue_mark(db, r->id, "error", 0);
            continue;
        }

        /* Insert task as user message in child session inbox */
        inbox_insert(db, child_sid, "spawn", r->task);

        /* Fork sub-agent process */
        char session_arg[64];
        snprintf(session_arg, sizeof(session_arg), "--session-id=%lld", (long long)child_sid);
        char task_arg[4096];
        snprintf(task_arg, sizeof(task_arg), "--task=%s", r->task);
        char depth_arg[32];
        snprintf(depth_arg, sizeof(depth_arg), "--depth=%d", r->depth);

        /* Acquire child session lock */
        char holder[32];
        snprintf(holder, sizeof(holder), "daemon-%d", (int)getpid());
        if (session_try_acquire(db, child_sid, holder) != 0) {
            spawn_queue_mark(db, r->id, "error", 0);
            continue;
        }

        pid_t pid = fork();
        if (pid < 0) {
            session_release(db, child_sid, holder);
            spawn_queue_mark(db, r->id, "error", 0);
            continue;
        }
        if (pid == 0) {
            close(g_signal_pipe[0]);
            close(g_signal_pipe[1]);
            close(g_chld_pipe[0]);
            close(g_chld_pipe[1]);
            agent_process_entry(cfg, child_sid);
            _exit(1);
        }

        /* Track child + record in sub_agents table */
        child_add(pid, child_sid);
        subagent_create(db, r->parent_session_id, child_sid, pid, r->depth, r->task);
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
    /* (1) Reset all "running" → "idle" (children already dead) */
    sqlite3_exec(db,
        "UPDATE sessions SET state='idle', lock_holder=NULL"
        " WHERE state='running';", NULL, NULL, NULL);

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
        int pending = inbox_count(db, sid);
        if (pending <= 0) {
            /* No result — write error tool_result to inbox */
            inbox_insert(db, sid, "recovery",
                "error: sub-agent process lost during daemon restart");
        }
        /* Transition to idle (daemon loop will fork if inbox has items) */
        const char *upd = "UPDATE sessions SET state='idle', lock_holder=NULL"
            " WHERE id=? AND state='waiting';";
        sqlite3_stmt *us;
        if (sqlite3_prepare_v2(db, upd, -1, &us, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(us, 1, sid);
            sqlite3_step(us);
            sqlite3_finalize(us);
        }
    }
}

/* ── Daemon main loop (T81) ─────────────────────────────────────── */

int daemon_run(const Config *cfg, sqlite3 *db) {
    if (daemon_signal_init() != 0) return -1;
    if (sigchld_pipe_init() != 0) {
        daemon_signal_close();
        return -1;
    }

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        daemon_signal_close();
        sigchld_pipe_close();
        return -1;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = g_signal_pipe[0];
    epoll_ctl(epfd, EPOLL_CTL_ADD, g_signal_pipe[0], &ev);

    ev.data.fd = g_chld_pipe[0];
    epoll_ctl(epfd, EPOLL_CTL_ADD, g_chld_pipe[0], &ev);

    /* T94/V34: Startup recovery — children already dead after daemon restart */
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
            } else if (events[i].data.fd == g_signal_pipe[0]) {
                /* Read session_ids from signal pipe */
                int64_t sid;
                while (read(g_signal_pipe[0], &sid, sizeof(sid)) == sizeof(sid)) {
                    /* Check inbox has pending items before forking */
                    if (inbox_count(db, sid) > 0) {
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
    g_child_count = 0;
    return 0;
}
