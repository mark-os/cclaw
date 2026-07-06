#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "cclaw.h"
#include "config_registry.h"
#include "log.h"
#include "secret_scan.h"
#include "secret_quarantine.h"
#include "unicode_normalize.h"
#include "agent_config.h"
#include "secret.h"
#include "validate.h"
#include "templates.h"
#include "monocypher.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

static const char *SCHEMA_SQL = TPL_SCHEMA_SQL;

/* ── Process registry + per-session ownership ──────────────────────
 * g_owner_instance is this process's instance id, set once at startup
 * (run_daemon/run_cli) and read inside session_set_state's CAS to stamp
 * sessions.owner_instance. Safe as a process-global: every session_set_state
 * call site runs on the owner process's main thread (workers never transition
 * state). Empty until set → stamps NULL (keeps standalone unit tests green). */
static char g_owner_instance[40];

void db_set_instance_id(const char *id) {
    if (!id) { g_owner_instance[0] = '\0'; return; }
    snprintf(g_owner_instance, sizeof(g_owner_instance), "%s", id);
}

const char *db_get_instance_id(void) { return g_owner_instance; }

int process_register(sqlite3 *db, const char *mode, int pid, char *out_id, size_t out_sz) {
    if (!db || !out_id || out_sz == 0) return -1;
    /* id = lower(hex(randomblob(16))) — a 32-char hex token */
    char id[40] = {0};
    sqlite3_stmt *g;
    if (sqlite3_prepare_v2(db, "SELECT lower(hex(randomblob(16)));", -1, &g, NULL) != SQLITE_OK)
        return -1;
    if (sqlite3_step(g) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(g, 0);
        if (v) snprintf(id, sizeof(id), "%s", v);
    }
    sqlite3_finalize(g);
    if (!id[0]) return -1;

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO processes(instance_id, pid, mode) VALUES(?,?,?);",
            -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(s, 1, id, -1, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, pid);
    sqlite3_bind_text(s, 3, mode ? mode : "cli", -1, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) return -1;
    snprintf(out_id, out_sz, "%s", id);
    return 0;
}

int process_heartbeat(sqlite3 *db, const char *id) {
    if (!db || !id || !id[0]) return -1;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "UPDATE processes SET heartbeat_at=unixepoch() WHERE instance_id=?;",
            -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(s, 1, id, -1, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int process_unregister(sqlite3 *db, const char *id) {
    if (!db || !id || !id[0]) return -1;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, "DELETE FROM processes WHERE instance_id=?;",
                           -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(s, 1, id, -1, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* A registry row is dead iff its pid is gone (kill(pid,0) == ESRCH) OR it has
 * not heartbeat within ttl_sec. kill==0 only suppresses the fast-path delete;
 * the TTL covers pid reuse (a reused pid won't heartbeat our row). Single-host
 * assumption — local SQLite means all owners share this kernel's pid space. */
int process_gc_dead(sqlite3 *db, int ttl_sec) {
    if (!db) return -1;
    /* Stale heartbeat → dead, regardless of pid liveness. */
    sqlite3_stmt *del;
    if (sqlite3_prepare_v2(db,
            "DELETE FROM processes WHERE unixepoch() - heartbeat_at > ?;",
            -1, &del, NULL) == SQLITE_OK) {
        sqlite3_bind_int(del, 1, ttl_sec);
        sqlite3_step(del);
        sqlite3_finalize(del);
    }
    /* Among the rest, prune any whose pid no longer exists. */
    sqlite3_stmt *sel;
    if (sqlite3_prepare_v2(db, "SELECT instance_id, pid FROM processes;",
                           -1, &sel, NULL) != SQLITE_OK)
        return -1;
    int cap = 0, n = 0;
    char **dead = NULL;
    while (sqlite3_step(sel) == SQLITE_ROW) {
        const char *id = (const char *)sqlite3_column_text(sel, 0);
        int pid = sqlite3_column_int(sel, 1);
        if (pid > 0 && kill(pid, 0) == -1 && errno == ESRCH) {
            if (n >= cap) {
                cap = cap ? cap * 2 : 8;
                char **tmp = realloc(dead, (size_t)cap * sizeof(*dead));
                if (!tmp) { for (int i = 0; i < n; i++) free(dead[i]); free(dead); sqlite3_finalize(sel); return -1; }
                dead = tmp;
            }
            dead[n++] = strdup(id ? id : "");
        }
    }
    sqlite3_finalize(sel);
    for (int i = 0; i < n; i++) {
        process_unregister(db, dead[i]);
        free(dead[i]);
    }
    free(dead);
    return 0;
}

int process_is_live(sqlite3 *db, const char *id, int ttl_sec) {
    if (!db || !id || !id[0]) return 0;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT pid, heartbeat_at FROM processes WHERE instance_id=?;",
            -1, &s, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(s, 1, id, -1, SQLITE_STATIC);
    int live = 0;
    if (sqlite3_step(s) == SQLITE_ROW) {
        int pid = sqlite3_column_int(s, 0);
        int64_t hb = sqlite3_column_int64(s, 1);
        int fresh = ((int64_t)time(NULL) - hb) <= ttl_sec;
        int present = (pid > 0) && !(kill(pid, 0) == -1 && errno == ESRCH);
        live = fresh && present;
    }
    sqlite3_finalize(s);
    return live;
}

/* ── sqlite3_trace_v2 callback ─────────────────────────────────── */

static int db_trace_cb(unsigned mask, void *ctx, void *p, void *x) {
    (void)ctx;
    if (mask == SQLITE_TRACE_PROFILE) {
        sqlite3_stmt *stmt = p;
        int64_t ns = *(int64_t *)x;
        const char *sql = sqlite3_sql(stmt);
        if (ns > 1000000) /* only log queries > 1ms */
            syslog(LOG_DEBUG, "sql: %ldms %s",
                   (long)(ns / 1000000), sql ? sql : "?");
    }
    return 0;
}

/* Enable sqlite3_trace_v2 on a connection (call when log_level >= trace). */
void db_enable_trace(sqlite3 *db) {
    sqlite3_trace_v2(db, SQLITE_TRACE_PROFILE, db_trace_cb, NULL);
}

/* Open DB with WAL + busy_timeout. No schema applied. */
/* Process-global SQLite log hook (SQLITE_CONFIG_LOG). SQLite invokes this for
 * every internal error/warning — BUSY, WAL recovery, malformed schema, etc. —
 * that would otherwise be silently swallowed inside a library call. May fire on
 * any thread; cclaw_log_write is thread-safe (syslog + thread-local ctx). */
static void sqlite_log_cb(void *arg, int rc, const char *msg) {
    (void)arg;
    LOG_INFO_("sqlite rc=%d err=%s msg=%s", rc, sqlite3_errstr(rc), msg ? msg : "");
}

void db_configure_logging(void) {
    /* Must precede sqlite3_initialize() — i.e. the first sqlite3_open(). */
    sqlite3_config(SQLITE_CONFIG_LOG, sqlite_log_cb, NULL);
}

sqlite3 *db_open(const char *path) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        LOG_ERROR_("db_open failed err=%s extended=%d",
                   sqlite3_errmsg(db), sqlite3_extended_errcode(db));
        sqlite3_close(db);
        return NULL;
    }
    /* Per-connection: turn plain result codes into extended ones, so a BUSY
     * surfaces as e.g. SQLITE_BUSY_SNAPSHOT (the read→write upgrade case). */
    sqlite3_extended_result_codes(db, 1);
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA wal_autocheckpoint=1000;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA busy_timeout=5000;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA foreign_keys=OFF;", NULL, NULL, NULL);
    return db;
}

/* Apply schema (CREATE TABLE IF NOT EXISTS). Call once from main at startup. */
int db_ensure_schema(sqlite3 *db) {
    char *err = NULL;
    int rc = sqlite3_exec(db, SCHEMA_SQL, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        LOG_ERROR_("db_ensure_schema failed err=%s", err);
        sqlite3_free(err);
        return -1;
    }

    /* Keep entries_fts in sync with the current tokenizer: IF NOT EXISTS
     * leaves an existing table on its old definition, so if it differs,
     * drop and re-index. DROP only removes index shadow tables ('entries'
     * is the content source); the check just avoids re-indexing every boot. */
    int fts_needs_porter = 0;
    sqlite3_stmt *fts_chk;
    if (sqlite3_prepare_v2(db, "SELECT sql FROM sqlite_master WHERE name='entries_fts'",
                           -1, &fts_chk, NULL) == SQLITE_OK) {
        if (sqlite3_step(fts_chk) == SQLITE_ROW) {
            const char *fsql = (const char *)sqlite3_column_text(fts_chk, 0);
            if (fsql && !strstr(fsql, "porter")) fts_needs_porter = 1;
        }
        sqlite3_finalize(fts_chk);
    }
    if (fts_needs_porter) {
        rc = sqlite3_exec(db,
            "DROP TABLE entries_fts;"
            "CREATE VIRTUAL TABLE entries_fts USING fts5("
            "  content, content=entries, content_rowid=id,"
            "  tokenize='porter unicode61');"
            "INSERT INTO entries_fts(entries_fts) VALUES('rebuild');",
            NULL, NULL, &err);
        if (rc != SQLITE_OK) {
            LOG_WARN_("db_ensure_schema fts rebuild failed err=%s", err);
            sqlite3_free(err);  /* non-fatal — recall degrades, chat still works */
        }
    }

    /* Stamp the schema generation. Only reached for a fresh or already-current
     * DB — db_schema_compat() refuses stale ones before we get here. */
    char pragma[48];
    snprintf(pragma, sizeof pragma, "PRAGMA user_version=%d", CCLAW_SCHEMA_VERSION);
    sqlite3_exec(db, pragma, NULL, NULL, NULL);
    return 0;
}

/* Gate startup on schema compatibility. Returns 1 if the DB is safe to use
 * (fresh/empty, or stamped with the current generation), 0 if it predates or
 * postdates this build and must be deleted (no migrations). */
int db_schema_compat(sqlite3 *db) {
    int uv = 0, has_tables = 0;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) uv = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    if (uv == CCLAW_SCHEMA_VERSION) return 1;       /* current */
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM sqlite_master WHERE type='table' LIMIT 1",
            -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) has_tables = 1;
        sqlite3_finalize(s);
    }
    return (uv == 0 && !has_tables);                /* fresh DB: schema stamps it */
}

/* mmap + reduced cache + relaxed sync for child processes (short-lived). */
void db_set_child_pragmas(sqlite3 *db) {
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA mmap_size=67108864;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA cache_size=-512;", NULL, NULL, NULL);
}

/* Seed default provider/model/tool rows on first run. Call once from main().
 * Keyed on the providers table — config rows come from config_registry_sync(),
 * not seed data. */
int db_seed_defaults(sqlite3 *db) {
    if (!db) return -1;
    sqlite3_stmt *cnt;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM providers", -1, &cnt, NULL) != SQLITE_OK)
        return -1;
    int empty = 0;
    if (sqlite3_step(cnt) == SQLITE_ROW && sqlite3_column_int(cnt, 0) == 0)
        empty = 1;
    sqlite3_finalize(cnt);
    if (!empty) return 0;

    char *err = NULL;
    int rc = sqlite3_exec(db, TPL_SEED_SQL, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        LOG_ERROR_("db_seed_defaults failed err=%s", err);
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

void db_close(sqlite3 *db) {
    if (db) sqlite3_close(db);
}

int64_t db_scalar_i64(sqlite3 *db, const char *sql, int64_t arg, int64_t dflt) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return dflt;
    sqlite3_bind_int64(stmt, 1, arg);
    int64_t result = dflt;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        result = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return result;
}

char *db_scalar_text(sqlite3 *db, const char *sql, int64_t arg) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int64(stmt, 1, arg);
    char *result = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *val = (const char *)sqlite3_column_text(stmt, 0);
        if (val) result = strdup(val);
    }
    sqlite3_finalize(stmt);
    return result;
}

int64_t session_create_filtered(sqlite3 *db, const char *name, const char *agent_name,
                                int64_t parent_session_id, int depth,
                                const char *tool_filter) {
    /* tool_filter must be a JSON array or NULL — reject anything else rather
     * than storing garbage that downstream json_each would misread. */
    const char *sql =
        "INSERT INTO sessions (name, agent_name, parent_session_id, depth, tool_filter)"
        " SELECT ?1, ?2, ?3, ?4, ?5"
        " WHERE ?5 IS NULL OR (json_valid(?5) AND json_type(?5)='array');";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    if (name) sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC); else sqlite3_bind_null(stmt, 1);
    if (agent_name) sqlite3_bind_text(stmt, 2, agent_name, -1, SQLITE_STATIC); else sqlite3_bind_null(stmt, 2);
    sqlite3_bind_int64(stmt, 3, parent_session_id > 0 ? parent_session_id : -1);
    sqlite3_bind_int(stmt, 4, depth);
    if (tool_filter) sqlite3_bind_text(stmt, 5, tool_filter, -1, SQLITE_STATIC);
    else sqlite3_bind_null(stmt, 5);
    if (sqlite3_step(stmt) != SQLITE_DONE || sqlite3_changes(db) == 0) {
        sqlite3_finalize(stmt);
        return -1;
    }
    int64_t id = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);
    return id;
}

int64_t session_create(sqlite3 *db, const char *name, const char *agent_name,
                       int64_t parent_session_id, int depth) {
    return session_create_filtered(db, name, agent_name, parent_session_id,
                                   depth, NULL);
}

int session_tool_allowed(sqlite3 *db, int64_t session_id, const char *tool_name) {
    if (!db || !tool_name) return 0;
    /* Fail closed on a missing session; NULL filter = unrestricted. */
    const char *sql =
        "SELECT tool_filter IS NULL"
        " OR EXISTS(SELECT 1 FROM json_each(tool_filter) WHERE value=?2)"
        " FROM sessions WHERE id=?1;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_text(stmt, 2, tool_name, -1, SQLITE_STATIC);
    int allowed = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        allowed = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return allowed;
}

Session *session_list(sqlite3 *db, int *count) {
    *count = 0;
    const char *sql = "SELECT id, name, leaf_id, agent_name, created_at, updated_at FROM sessions ORDER BY id;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;

    int cap = 8;
    Session *list = malloc((size_t)cap * sizeof(Session));
    if (!list) { sqlite3_finalize(stmt); return NULL; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (*count >= cap) {
            cap *= 2;
            Session *tmp = realloc(list, (size_t)cap * sizeof(Session));
            if (!tmp) {
                session_list_free(list, *count);
                *count = 0;
                sqlite3_finalize(stmt);
                return NULL;
            }
            list = tmp;
        }
        Session *s = &list[*count];
        s->id = sqlite3_column_int64(stmt, 0);
        const char *n = (const char *)sqlite3_column_text(stmt, 1);
        s->name = n ? strdup(n) : NULL;
        s->leaf_id = sqlite3_column_int64(stmt, 2);
        const char *an = (const char *)sqlite3_column_text(stmt, 3);
        s->agent_name = an ? strdup(an) : NULL;
        s->created_at = (time_t)sqlite3_column_int64(stmt, 4);
        s->updated_at = (time_t)sqlite3_column_int64(stmt, 5);
        (*count)++;
    }
    sqlite3_finalize(stmt);
    if (*count == 0) { free(list); return NULL; }
    return list;
}

void session_list_free(Session *sessions, int count) {
    if (!sessions) return;
    for (int i = 0; i < count; i++) {
        free(sessions[i].name);
        free(sessions[i].agent_name);
    }
    free(sessions);
}

/* Serialize tool_calls array to JSON string. Caller must free. Returns NULL if none. */
static int role_to_int(Role r) {
    switch (r) {
        case ROLE_SYSTEM:    return 0;
        case ROLE_USER:      return 1;
        case ROLE_ASSISTANT: return 2;
        case ROLE_TOOL:      return 3;
        case ROLE_COMPACTION: return 4;
    }
    return 1;
}

static Role int_to_role(int i) {
    switch (i) {
        case 0: return ROLE_SYSTEM;
        case 1: return ROLE_USER;
        case 2: return ROLE_ASSISTANT;
        case 3: return ROLE_TOOL;
        case 4: return ROLE_COMPACTION;
    }
    return ROLE_USER;
}

static int stop_reason_to_int(StopReason sr) {
    switch (sr) {
        case STOP_REASON_NONE:     return 0;
        case STOP_REASON_STOP:     return 1;
        case STOP_REASON_LENGTH:   return 2;
        case STOP_REASON_TOOL_USE: return 3;
        case STOP_REASON_ERROR:    return 4;
        case STOP_REASON_ABORTED:  return 5;
    }
    return 0;
}

static StopReason int_to_stop_reason(int i) {
    switch (i) {
        case 1: return STOP_REASON_STOP;
        case 2: return STOP_REASON_LENGTH;
        case 3: return STOP_REASON_TOOL_USE;
        case 4: return STOP_REASON_ERROR;
        case 5: return STOP_REASON_ABORTED;
    }
    return STOP_REASON_NONE;
}

/* Serialize tool_calls array to provider-neutral JSON via SQLite.
 * Format: [{"id":"...","name":"...","args":{...}},...] */
static char *serialize_tool_calls(sqlite3 *db, const ToolCall *tcs, size_t count) {
    if (!tcs || count == 0 || !db) return NULL;

    sqlite3_exec(db, "DROP TABLE IF EXISTS _tc_ser;", NULL, NULL, NULL);
    sqlite3_exec(db, "CREATE TEMP TABLE _tc_ser(pos INTEGER,id TEXT,name TEXT,args TEXT);", NULL, NULL, NULL);

    sqlite3_stmt *ins;
    if (sqlite3_prepare_v2(db, "INSERT INTO _tc_ser VALUES(?,?,?,?)", -1, &ins, NULL) != SQLITE_OK)
        return NULL;
    for (size_t i = 0; i < count; i++) {
        sqlite3_bind_int(ins, 1, (int)i);
        sqlite3_bind_text(ins, 2, tcs[i].id ? tcs[i].id : "", -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 3, tcs[i].name ? tcs[i].name : "", -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 4, tcs[i].arguments ? tcs[i].arguments : "{}", -1, SQLITE_STATIC);
        sqlite3_step(ins); sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);

    char *result = NULL;
    sqlite3_stmt *sel;
    if (sqlite3_prepare_v2(db,
        "SELECT json_group_array(json_object('id',id,'name',name,'args',"
        "CASE WHEN json_valid(args) THEN json(args) ELSE json('{}') END) ORDER BY pos)"
        " FROM _tc_ser", -1, &sel, NULL) == SQLITE_OK) {
        if (sqlite3_step(sel) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(sel, 0);
            if (v) result = strdup(v);
        }
        sqlite3_finalize(sel);
    }
    sqlite3_exec(db, "DROP TABLE IF EXISTS _tc_ser;", NULL, NULL, NULL);
    return result;
}

/* Deserialize tool_calls from provider-neutral JSON array via SQLite.
 * Sets *out_count. Returns heap array or NULL. */
static ToolCall *deserialize_tool_calls(sqlite3 *db, const char *json, size_t *out_count) {
    *out_count = 0;
    if (!json || !db) return NULL;

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
        "SELECT json_extract(value,'$.id'), json_extract(value,'$.name'),"
        " json_extract(value,'$.args') FROM json_each(?)", -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(stmt, 1, json, -1, SQLITE_STATIC);

    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) n++;
    if (n == 0) { sqlite3_finalize(stmt); return NULL; }
    sqlite3_reset(stmt);

    ToolCall *tcs = malloc((size_t)n * sizeof(ToolCall));
    int i = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && i < n) {
        const char *id = (const char *)sqlite3_column_text(stmt, 0);
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        const char *args = (const char *)sqlite3_column_text(stmt, 2);
        tcs[i].id = id ? strdup(id) : NULL;
        tcs[i].name = name ? strdup(name) : NULL;
        tcs[i].arguments = args ? strdup(args) : NULL;
        i++;
    }
    *out_count = (size_t)i;
    sqlite3_finalize(stmt);
    return tcs;
}

/* Populate Message from split columns read from DB row */
static void read_entry_from_columns(sqlite3 *db, sqlite3_stmt *stmt, int col_role, int col_content,
                                    int col_tool_calls, int col_tool_call_id,
                                    int col_stop_reason, Message *msg) {
    memset(msg, 0, sizeof(*msg));
    msg->role = int_to_role(sqlite3_column_int(stmt, col_role));
    msg->stop_reason = int_to_stop_reason(sqlite3_column_int(stmt, col_stop_reason));

    const char *content = (const char *)sqlite3_column_text(stmt, col_content);
    if (content) msg->content = strdup(content);

    if (msg->role == ROLE_TOOL) {
        const char *tcid = (const char *)sqlite3_column_text(stmt, col_tool_call_id);
        msg->tool_result = malloc(sizeof(ToolResult));
        msg->tool_result->tool_call_id = tcid ? strdup(tcid) : NULL;
        msg->tool_result->content = msg->content;
        msg->content = NULL;
    } else if (msg->role == ROLE_ASSISTANT) {
        const char *tc_json = (const char *)sqlite3_column_text(stmt, col_tool_calls);
        if (tc_json) {
            msg->tool_calls = deserialize_tool_calls(db, tc_json, &msg->tool_call_count);
        }
    }
}
/* walk parent_id chain from leaf→root, return in root→leaf order */
Entry *session_get_branch(sqlite3 *db, int64_t session_id, int *count) {
    *count = 0;

    /* Get leaf_id for session */
    const char *leaf_sql = "SELECT leaf_id FROM sessions WHERE id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, leaf_sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(stmt, 1, session_id);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return NULL;
    }
    int64_t leaf_id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);

    if (leaf_id < 0) return NULL; /* no entries yet */

    /* Walk chain using recursive CTE — read split columns.
     * Use level counter for ordering (not ORDER BY id) since compaction
     * entries may have higher ids than entries they precede in the path.
     * Depth capped at 10000 to prevent infinite loops from data corruption. */
    const char *branch_sql =
        "WITH RECURSIVE branch(id, parent_id, original_parent_id, session_id, created_at, role, content, tool_calls, tool_call_id, stop_reason, lvl) AS ("
        "  SELECT id, parent_id, original_parent_id, session_id, created_at, role, content, tool_calls, tool_call_id, stop_reason, 0"
        "    FROM entries WHERE id=? AND session_id=?"
        "  UNION ALL"
        "  SELECT e.id, e.parent_id, e.original_parent_id, e.session_id, e.created_at, e.role, e.content, e.tool_calls, e.tool_call_id, e.stop_reason, b.lvl+1"
        "    FROM entries e JOIN branch b ON e.id=b.parent_id"
        "    WHERE b.lvl < 10000"
        ") SELECT id, parent_id, original_parent_id, session_id, created_at, role, content, tool_calls, tool_call_id, stop_reason FROM branch ORDER BY lvl DESC;";

    if (sqlite3_prepare_v2(db, branch_sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(stmt, 1, leaf_id);
    sqlite3_bind_int64(stmt, 2, session_id);

    int cap = 16;
    Entry *entries = malloc((size_t)cap * sizeof(Entry));
    if (!entries) { sqlite3_finalize(stmt); return NULL; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (*count >= cap) {
            cap *= 2;
            Entry *tmp = realloc(entries, (size_t)cap * sizeof(Entry));
            if (!tmp) { break; }
            entries = tmp;
        }
        Entry *e = &entries[*count];
        e->id = sqlite3_column_int64(stmt, 0);
        e->parent_id = sqlite3_column_int64(stmt, 1);
        e->original_parent_id = sqlite3_column_type(stmt, 2) == SQLITE_NULL ? -1 : sqlite3_column_int64(stmt, 2);
        e->session_id = sqlite3_column_int64(stmt, 3);
        e->created_at = (time_t)sqlite3_column_int64(stmt, 4);
        /* cols: 5=role, 6=content, 7=tool_calls, 8=tool_call_id, 9=stop_reason */
        read_entry_from_columns(db, stmt, 5, 6, 7, 8, 9, &e->message);
        (*count)++;
    }
    sqlite3_finalize(stmt);
    if (*count == 0) { free(entries); return NULL; }
    return entries;
}

/* Resolve deliverable response text from session branch.
 * Walks backward from leaf: returns first non-empty assistant content found.
 * Skips empty/null final entries (provider glitch) to reach intermediate texts. */

void entry_branch_free(Entry *entries, int count) {
    if (!entries) return;
    for (int i = 0; i < count; i++) {
        free(entries[i].message.content);
        if (entries[i].message.tool_calls) {
            for (size_t t = 0; t < entries[i].message.tool_call_count; t++) {
                free(entries[i].message.tool_calls[t].id);
                free(entries[i].message.tool_calls[t].name);
                free(entries[i].message.tool_calls[t].arguments);
            }
            free(entries[i].message.tool_calls);
        }
        if (entries[i].message.tool_result) {
            free(entries[i].message.tool_result->tool_call_id);
            free(entries[i].message.tool_result->content);
            free(entries[i].message.tool_result);
        }
    }
    free(entries);
}

/* Get latest assistant response text from session branch.
 * Walks parent chain from leaf; returns first non-empty assistant content,
 * stops at user boundary. No full-branch materialization.
 * Returns heap-allocated string or NULL if no deliverable content. */
char *get_response_text(sqlite3 *db, int64_t session_id) {
    /* Get leaf_id */
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "SELECT leaf_id FROM sessions WHERE id=?",
                           -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(stmt, 1, session_id);
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return NULL; }
    int64_t leaf_id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    if (leaf_id < 0) return NULL;

    const char *sql =
        "WITH RECURSIVE branch(id, parent_id, role, content, lvl) AS ("
        "  SELECT id, parent_id, role, content, 0"
        "    FROM entries WHERE id=? AND session_id=?"
        "  UNION ALL"
        "  SELECT e.id, e.parent_id, e.role, e.content, b.lvl+1"
        "    FROM entries e JOIN branch b ON e.id=b.parent_id"
        "    WHERE b.lvl < 10000"
        ") SELECT role, content FROM branch"
        "  WHERE (role=2 AND content IS NOT NULL AND content != '') OR role=1"
        "  ORDER BY lvl ASC LIMIT 1;";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(stmt, 1, leaf_id);
    sqlite3_bind_int64(stmt, 2, session_id);

    char *result = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int role = sqlite3_column_int(stmt, 0);
        if (role == 2) {
            const char *c = (const char *)sqlite3_column_text(stmt, 1);
            if (c) result = strdup(c);
        }
    }
    sqlite3_finalize(stmt);
    return result;
}

char *session_get_agent_name(sqlite3 *db, int64_t session_id) {
    return db_scalar_text(db, "SELECT agent_name FROM sessions WHERE id=?;", session_id);
}

int session_get_depth(sqlite3 *db, int64_t session_id) {
    return (int)db_scalar_i64(db, "SELECT depth FROM sessions WHERE id=?;", session_id, 0);
}


/* Derive entry type string from Role (backward compat for legacy callers) */
static const char *type_from_role(const Message *msg) {
    switch (msg->role) {
        case ROLE_SYSTEM:    return "system";
        case ROLE_USER:      return "user_message";
        case ROLE_ASSISTANT: return "assistant_message";
        case ROLE_TOOL:      return "tool_result";
        case ROLE_COMPACTION: return "compaction";
    }
    return "user_message";
}

/* Insert compaction summary entry and reparent.
 * - Inserts role=COMPACTION entry with parent_id = last_kept_id
 * - Reparents first_after_id to point to the new summary node
 * - Sets original_parent_id on reparented entry
 * Returns new compaction entry id (>0) or -1 on error. */
int64_t entry_compact(sqlite3 *db, int64_t session_id, int64_t last_kept_id,
                      int64_t first_after_id, const char *summary) {
    if (!db || !summary) return -1;

    /* Insert compaction entry as child of last_kept_id */
    const char *ins_sql =
        "INSERT INTO entries (parent_id, session_id, type, role, content, stop_reason,"
        " token_estimate, content_bytes, tool_call_count)"
        " VALUES (?,?,'compaction',4,?,0,?,?,0);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, ins_sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, last_kept_id);
    sqlite3_bind_int64(stmt, 2, session_id);
    sqlite3_bind_text(stmt, 3, summary, -1, SQLITE_TRANSIENT);
    int len = (int)strlen(summary);
    sqlite3_bind_int(stmt, 4, (len / 4) + 4);
    sqlite3_bind_int(stmt, 5, len);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;

    int64_t compact_id = sqlite3_last_insert_rowid(db);

    /* Reparent first_after_id → compact_id, save original_parent_id */
    const char *reparent_sql =
        "UPDATE entries SET original_parent_id = parent_id, parent_id = ?"
        " WHERE id = ? AND session_id = ?;";
    if (sqlite3_prepare_v2(db, reparent_sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, compact_id);
    sqlite3_bind_int64(stmt, 2, first_after_id);
    sqlite3_bind_int64(stmt, 3, session_id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;

    return compact_id;
}


/* FTS5 search over message content */
Entry *entry_search(sqlite3 *db, const char *query, int64_t session_id, int *count) {
    *count = 0;
    const char *sql =
        "SELECT e.id, e.parent_id, e.session_id, e.created_at,"
        " e.role, e.content, e.tool_calls, e.tool_call_id, e.stop_reason"
        " FROM entries_fts f JOIN entries e ON e.id = f.rowid"
        " WHERE entries_fts MATCH ? AND e.session_id = ?"
        " ORDER BY rank LIMIT 50;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(stmt, 1, query, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, session_id);

    int cap = 8;
    Entry *entries = malloc((size_t)cap * sizeof(Entry));
    if (!entries) { sqlite3_finalize(stmt); return NULL; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (*count >= cap) {
            cap *= 2;
            Entry *tmp = realloc(entries, (size_t)cap * sizeof(Entry));
            if (!tmp) {
                entry_branch_free(entries, *count);
                *count = 0;
                sqlite3_finalize(stmt);
                return NULL;
            }
            entries = tmp;
        }
        Entry *e = &entries[*count];
        e->id = sqlite3_column_int64(stmt, 0);
        e->parent_id = sqlite3_column_int64(stmt, 1);
        e->original_parent_id = -1; /* not loaded in search results */
        e->session_id = sqlite3_column_int64(stmt, 2);
        e->created_at = (time_t)sqlite3_column_int64(stmt, 3);
        /* cols: 4=role, 5=content, 6=tool_calls, 7=tool_call_id, 8=stop_reason */
        read_entry_from_columns(db, stmt, 4, 5, 6, 7, 8, &e->message);
        (*count)++;
    }
    sqlite3_finalize(stmt);
    if (*count == 0) { free(entries); return NULL; }
    return entries;
}

/* Sum cost_nano for all entries in a session */
int64_t session_cost(sqlite3 *db, int64_t session_id) {
    return db_scalar_i64(db, "SELECT COALESCE(SUM(cost_nano),0) FROM entries WHERE session_id=?;", session_id, 0);
}

/* Secret key for secrets-table encryption. Set via db_set_secret_key(). */
static uint8_t s_secret_key[32];
static int s_secret_key_loaded = 0;

void db_set_secret_key(const uint8_t key[32]) {
    memcpy(s_secret_key, key, 32);
    s_secret_key_loaded = 1;
}

/* Wipe the master key from this process's memory. Called in the forked
 * tool-exec broker (which holds no DB handle and never needs the key) so the
 * relay process — exposed to untrusted, model-driven traffic — carries no key
 * material. The daemon's own copy is untouched (separate process post-fork). */
void db_wipe_secret_key(void) {
    crypto_wipe(s_secret_key, sizeof(s_secret_key));
    s_secret_key_loaded = 0;
}

int db_secret_key_loaded(void) {
    return s_secret_key_loaded;
}

/* Channel→agent binding */

char *db_channel_binding_get(sqlite3 *db, const char *channel_type, const char *channel_id) {
    const char *sql = "SELECT agent_name FROM channel_routes WHERE channel_name=? AND channel_id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(stmt, 1, channel_type, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, channel_id, -1, SQLITE_STATIC);
    char *result = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *val = (const char *)sqlite3_column_text(stmt, 0);
        if (val) result = strdup(val);
    }
    sqlite3_finalize(stmt);
    return result;
}


/* sub-agent limits — count active child sessions */

int session_count_children(sqlite3 *db, int64_t parent_session_id) {
    return (int)db_scalar_i64(db,
        "SELECT COUNT(*) FROM sessions WHERE parent_session_id=? AND state != 'idle';",
        parent_session_id, -1);
}

int session_count_active_agents(sqlite3 *db) {
    const char *sql =
        "SELECT COUNT(*) FROM sessions WHERE parent_session_id > 0 AND state != 'idle';";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

/* next turn_id for a session */
int64_t db_next_turn_id(sqlite3 *db, int64_t session_id) {
    return db_scalar_i64(db, "SELECT COALESCE(MAX(turn_id), 0) + 1 FROM entries WHERE session_id=?;", session_id, 1);
}

/* Retained for tests and callers outside append paths (trigger handles append) */
int session_set_leaf(sqlite3 *db, int64_t session_id, int64_t leaf_id) {
    const char *sql = "UPDATE sessions SET leaf_id=?, updated_at=unixepoch() WHERE id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, leaf_id);
    sqlite3_bind_int64(stmt, 2, session_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE && sqlite3_changes(db) > 0) ? 0 : -1;
}

/* append entry with explicit turn_id */
int64_t entry_append_with_turn(sqlite3 *db, int64_t session_id, const Message *msg, int64_t turn_id) {
    /* parent_id and turn_id computed as subqueries — atomic with the INSERT
     * via the entries_leaf_ai trigger (no separate SELECT + set_leaf). */
    const char *ins_sql = turn_id > 0
        ? "INSERT INTO entries (parent_id, session_id, turn_id, type, part_index, role, content, tool_calls,"
          " tool_call_id, tool_name, is_error, stop_reason, model,"
          " usage_in, usage_out, cost_nano, token_estimate, content_bytes, tool_call_count, data)"
          " VALUES ((SELECT leaf_id FROM sessions WHERE id=?1),?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19);"
        : "INSERT INTO entries (parent_id, session_id, turn_id, type, part_index, role, content, tool_calls,"
          " tool_call_id, tool_name, is_error, stop_reason, model,"
          " usage_in, usage_out, cost_nano, token_estimate, content_bytes, tool_call_count, data)"
          " VALUES ((SELECT leaf_id FROM sessions WHERE id=?1),?1,"
          "(SELECT COALESCE(MAX(turn_id),0)+1 FROM entries WHERE session_id=?1),"
          "?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18);";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, ins_sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    int b; /* next bind index */
    sqlite3_bind_int64(stmt, 1, session_id);
    if (turn_id > 0) {
        sqlite3_bind_int64(stmt, 2, turn_id);
        b = 3;
    } else {
        b = 2;
    }

    sqlite3_bind_text(stmt, b, type_from_role(msg), -1, SQLITE_STATIC); b++;
    sqlite3_bind_int(stmt, b, 0); b++; /* part_index */
    sqlite3_bind_int(stmt, b, role_to_int(msg->role)); b++;

    const char *content_val = NULL;
    if (msg->role == ROLE_TOOL && msg->tool_result)
        content_val = msg->tool_result->content;
    else
        content_val = msg->content;
    if (content_val) sqlite3_bind_text(stmt, b, content_val, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, b);
    b++;

    char *tc_json = NULL;
    int tc_count = 0;
    if (msg->role == ROLE_ASSISTANT && msg->tool_calls && msg->tool_call_count > 0) {
        tc_json = serialize_tool_calls(db, msg->tool_calls, msg->tool_call_count);
        tc_count = (int)msg->tool_call_count;
    }
    if (tc_json) sqlite3_bind_text(stmt, b, tc_json, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, b);
    b++;

    if (msg->role == ROLE_TOOL && msg->tool_result && msg->tool_result->tool_call_id)
        sqlite3_bind_text(stmt, b, msg->tool_result->tool_call_id, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, b);
    b++;

    if (msg->tool_name) sqlite3_bind_text(stmt, b, msg->tool_name, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, b);
    b++;

    sqlite3_bind_int(stmt, b, msg->is_error); b++;
    sqlite3_bind_int(stmt, b, stop_reason_to_int(msg->stop_reason)); b++;

    if (msg->model) sqlite3_bind_text(stmt, b, msg->model, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, b);
    b++;

    if (msg->usage_in > 0) sqlite3_bind_int(stmt, b, msg->usage_in);
    else sqlite3_bind_null(stmt, b);
    b++;
    if (msg->usage_out > 0) sqlite3_bind_int(stmt, b, msg->usage_out);
    else sqlite3_bind_null(stmt, b);
    b++;

    if (msg->cost_nano > 0) sqlite3_bind_int64(stmt, b, msg->cost_nano);
    else sqlite3_bind_null(stmt, b);
    b++;

    int content_len = content_val ? (int)strlen(content_val) : 0;
    int tc_len = tc_json ? (int)strlen(tc_json) : 0;
    int total_bytes = content_len + tc_len;
    sqlite3_bind_int(stmt, b, (total_bytes / 4) + 4); b++;
    sqlite3_bind_int(stmt, b, total_bytes); b++;
    sqlite3_bind_int(stmt, b, tc_count); b++;
    if (msg->metadata_json) sqlite3_bind_text(stmt, b, msg->metadata_json, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, b);
    free(tc_json);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;

    return sqlite3_last_insert_rowid(db);
}

/* Append a flat typed entry — the new event-sourced API. */
int64_t entry_append_typed(sqlite3 *db, int64_t session_id, int64_t turn_id,
                           const char *type, int part_index, const char *content,
                           const char *tool_call_id, const char *tool_name,
                           int is_error, StopReason stop_reason,
                           const char *model, int usage_in, int usage_out,
                           int64_t cost_nano) {
    /* Derive role from type for backward compat */
    int role = 1;
    if (!type) type = "user_message";
    if (strcmp(type, "system") == 0) role = 0;
    else if (strcmp(type, "user_message") == 0) role = 1;
    else if (strcmp(type, "assistant_message") == 0) role = 2;
    else if (strcmp(type, "reasoning") == 0) role = 2;
    else if (strcmp(type, "tool_call") == 0) role = 2;
    else if (strcmp(type, "tool_result") == 0) role = 3;
    else if (strcmp(type, "compaction") == 0) role = 4;

    /* parent_id via subquery — atomic with INSERT via entries_leaf_ai trigger */
    const char *sql =
        "INSERT INTO entries (parent_id, session_id, turn_id, type, part_index, role,"
        " content, tool_call_id, tool_name, is_error, stop_reason, model,"
        " usage_in, usage_out, cost_nano, token_estimate, content_bytes)"
        " VALUES ((SELECT leaf_id FROM sessions WHERE id=?1),?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_int64(stmt, 2, turn_id);
    sqlite3_bind_text(stmt, 3, type, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, part_index);
    sqlite3_bind_int(stmt, 5, role);
    if (content) sqlite3_bind_text(stmt, 6, content, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 6);
    if (tool_call_id) sqlite3_bind_text(stmt, 7, tool_call_id, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 7);
    if (tool_name) sqlite3_bind_text(stmt, 8, tool_name, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 8);
    sqlite3_bind_int(stmt, 9, is_error);
    sqlite3_bind_int(stmt, 10, stop_reason_to_int(stop_reason));
    if (model) sqlite3_bind_text(stmt, 11, model, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 11);
    if (usage_in > 0) sqlite3_bind_int(stmt, 12, usage_in);
    else sqlite3_bind_null(stmt, 12);
    if (usage_out > 0) sqlite3_bind_int(stmt, 13, usage_out);
    else sqlite3_bind_null(stmt, 13);
    if (cost_nano > 0) sqlite3_bind_int64(stmt, 14, cost_nano);
    else sqlite3_bind_null(stmt, 14);
    int clen = content ? (int)strlen(content) : 0;
    sqlite3_bind_int(stmt, 15, (clen / 4) + 4);
    sqlite3_bind_int(stmt, 16, clen);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;

    return sqlite3_last_insert_rowid(db);
}

/* State transition guard. Busy states (llm_running/tool_running/compacting/
 * rate_limited) are reachable from idle or each other (a turn moves
 * llm_running → tool_running → llm_running, and ends llm_running → compacting);
 * idle is reachable from any busy state plus awaiting_agent/awaiting_approval;
 * awaiting_approval is reachable from llm_running/tool_running;
 * tool_running is also reachable from awaiting_approval (approval resolved). */
int session_set_state(sqlite3 *db, int64_t session_id, const char *state) {
    /* Stamp ownership in the same guarded CAS: idle clears the owner (invariant
     * state='idle' ⟺ owner_instance IS NULL), any busy state stamps this
     * process's instance id. ?2 is g_owner_instance bound as NULL when empty
     * (untouched unit tests), so a busy transition without an instance stamps
     * NULL rather than "". */
    const char *sql =
        "UPDATE sessions SET state=?1, updated_at=unixepoch(),"
        " turn_iteration = CASE WHEN ?1='idle' THEN 0 ELSE turn_iteration END,"
        " owner_instance = CASE WHEN ?1='idle' THEN NULL ELSE ?2 END"
        " WHERE id=?3 AND ("
        "  (?1 IN ('llm_running','tool_running','compacting','rate_limited')"
        "     AND state IN ('idle','llm_running','tool_running','compacting','rate_limited','awaiting_approval')) OR"
        "  (?1 = 'awaiting_agent' AND state IN ('idle','llm_running','tool_running')) OR"
        "  (?1 = 'awaiting_approval' AND state IN ('llm_running','tool_running')) OR"
        "  (?1 = 'idle' AND state IN"
        "     ('llm_running','tool_running','compacting','rate_limited','awaiting_agent','awaiting_approval'))"
        ");";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, state, -1, SQLITE_STATIC);
    if (g_owner_instance[0])
        sqlite3_bind_text(stmt, 2, g_owner_instance, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 2);
    sqlite3_bind_int64(stmt, 3, session_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE && sqlite3_changes(db) == 1) ? 0 : -1;
}

/* Turn iteration accessors */

int session_get_iteration(sqlite3 *db, int64_t session_id) {
    return (int)db_scalar_i64(db, "SELECT turn_iteration FROM sessions WHERE id=?", session_id, -1);
}

int session_set_iteration(sqlite3 *db, int64_t session_id, int iter) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "UPDATE sessions SET turn_iteration=? WHERE id=?",
                           -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, iter);
    sqlite3_bind_int64(stmt, 2, session_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int session_bump_iteration(sqlite3 *db, int64_t session_id) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
        "UPDATE sessions SET turn_iteration = turn_iteration + 1 WHERE id=?"
        " RETURNING turn_iteration", -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, session_id);
    int val = (sqlite3_step(stmt) == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : -1;
    sqlite3_finalize(stmt);
    return val;
}

/* Parent info for sub-agent completion */

SessionParentInfo session_get_parent_info(sqlite3 *db, int64_t session_id) {
    SessionParentInfo info = { .parent_session_id = -1, .parent_tool_call_id = NULL };
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
        "SELECT parent_session_id, parent_tool_call_id FROM sessions WHERE id=?",
        -1, &stmt, NULL) != SQLITE_OK) return info;
    sqlite3_bind_int64(stmt, 1, session_id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        info.parent_session_id = sqlite3_column_int64(stmt, 0);
        const char *tc = (const char *)sqlite3_column_text(stmt, 1);
        if (tc) info.parent_tool_call_id = strdup(tc);
    }
    sqlite3_finalize(stmt);
    return info;
}

int session_set_parent_tool_call_id(sqlite3 *db, int64_t session_id, const char *call_id) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
        "UPDATE sessions SET parent_tool_call_id=? WHERE id=?",
        -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, call_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, session_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_entry_set_network_hosts(sqlite3 *db, int64_t entry_id, const char *hosts_json) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
        "UPDATE entries SET network_hosts=? WHERE id=?",
        -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, hosts_json, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, entry_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* Inbox primitives */

int64_t inbox_insert(sqlite3 *db, int64_t session_id, const char *source, const char *payload) {
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO inbox (session_id, source, payload) VALUES (?, ?, ?)", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_text(stmt, 2, source ? source : "cli", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, payload, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    return sqlite3_last_insert_rowid(db);
}

int64_t inbox_insert_scanned(sqlite3 *db, int64_t session_id, const char *source, const char *payload) {
    if (!payload || !payload[0])
        return inbox_insert(db, session_id, source, payload);
    /* Strip invisible Unicode BEFORE the secret scan — zero-width chars would
     * otherwise split a credential across the scanner's pattern window. */
    size_t len = strlen(payload);
    char *clean = unicode_strip_invisible(payload, len, &len);
    const char *scanned = clean ? clean : payload;
    if (!clean) len = strlen(payload);

    /* The classic capture case (a user pastes a live key in chat): quarantine
     * into the secret store instead of shredding, so it becomes a pending
     * secret behind a {{SECRET:...}} placeholder rather than being destroyed. */
    char *quarantined = tool_result_postprocess_q(db, scanned, NULL, 0);
    int64_t id = inbox_insert(db, session_id, source, quarantined ? quarantined : scanned);
    free(clean);
    free(quarantined);
    return id;
}

InboxItem *inbox_peek(sqlite3 *db, int64_t session_id, int limit, int *count) {
    *count = 0;
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db,
        "SELECT id, session_id, source, payload, created_at FROM inbox "
        "WHERE session_id = ? AND consumed = 0 ORDER BY id ASC LIMIT ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return NULL;
    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_int(stmt, 2, limit);

    int cap = limit < 16 ? 16 : limit;
    InboxItem *items = malloc(cap * sizeof(InboxItem));
    if (!items) { sqlite3_finalize(stmt); return NULL; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        InboxItem *it = &items[*count];
        it->id = sqlite3_column_int64(stmt, 0);
        it->session_id = sqlite3_column_int64(stmt, 1);
        const char *src = (const char *)sqlite3_column_text(stmt, 2);
        it->source = src ? strdup(src) : NULL;
        const char *pay = (const char *)sqlite3_column_text(stmt, 3);
        it->payload = pay ? strdup(pay) : NULL;
        it->created_at = sqlite3_column_int64(stmt, 4);
        (*count)++;
    }
    sqlite3_finalize(stmt);
    if (*count == 0) { free(items); return NULL; }
    return items;
}

void inbox_items_free(InboxItem *items, int count) {
    for (int i = 0; i < count; i++) {
        free(items[i].source);
        free(items[i].payload);
    }
    free(items);
}

int inbox_count(sqlite3 *db, int64_t session_id) {
    return (int)db_scalar_i64(db, "SELECT COUNT(*) FROM inbox WHERE session_id=? AND consumed=0", session_id, -1);
}

/* Atomically consume inbox items into session entries.
 * _locked contract: caller MUST hold BEGIN IMMEDIATE. The loop below writes
 * while the SELECT is still open, which is only safe because the write lock
 * was taken up front — without it, a concurrent commit would make the
 * read→write snapshot upgrade fail with an immediate SQLITE_BUSY
 * (busy_timeout does not apply; see channel_consume_events). */
int inbox_consume_into_entries_locked(sqlite3 *db, int64_t session_id, int limit) {
    /* Peek unconsumed items */
    sqlite3_stmt *sel;
    if (sqlite3_prepare_v2(db,
        "SELECT id, payload FROM inbox WHERE session_id = ? AND consumed = 0 ORDER BY id ASC LIMIT ?",
        -1, &sel, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(sel, 1, session_id);
    sqlite3_bind_int(sel, 2, limit);

    /* Get current leaf */
    sqlite3_stmt *leaf_stmt;
    if (sqlite3_prepare_v2(db, "SELECT leaf_id FROM sessions WHERE id=?", -1, &leaf_stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(sel);
        return -1;
    }
    sqlite3_bind_int64(leaf_stmt, 1, session_id);
    if (sqlite3_step(leaf_stmt) != SQLITE_ROW) {
        sqlite3_finalize(leaf_stmt);
        sqlite3_finalize(sel);
        return -1;
    }
    int64_t parent_id = sqlite3_column_int64(leaf_stmt, 0);
    sqlite3_finalize(leaf_stmt);

    int consumed = 0;
    while (sqlite3_step(sel) == SQLITE_ROW) {
        int64_t inbox_id = sqlite3_column_int64(sel, 0);
        const char *payload = (const char *)sqlite3_column_text(sel, 1);
        const char *content_val = payload ? payload : "";
        int content_len = (int)strlen(content_val);

        /* Insert entry with split columns (role=1 = user) */
        sqlite3_stmt *ins;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO entries (parent_id, session_id, role, content, token_estimate, content_bytes)"
            " VALUES (?,?,1,?,?,?)",
            -1, &ins, NULL) != SQLITE_OK) {
            sqlite3_finalize(sel);
            return -1;
        }
        sqlite3_bind_int64(ins, 1, parent_id);
        sqlite3_bind_int64(ins, 2, session_id);
        sqlite3_bind_text(ins, 3, content_val, content_len, SQLITE_TRANSIENT);
        sqlite3_bind_int(ins, 4, (content_len / 4) + 4);
        sqlite3_bind_int(ins, 5, content_len);
        int rc = sqlite3_step(ins);
        sqlite3_finalize(ins);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(sel);
            return -1;
        }
        parent_id = sqlite3_last_insert_rowid(db);

        /* Mark consumed */
        sqlite3_stmt *upd;
        if (sqlite3_prepare_v2(db,
            "UPDATE inbox SET consumed = 1 WHERE id = ?", -1, &upd, NULL) != SQLITE_OK) {
            sqlite3_finalize(sel);
            return -1;
        }
        sqlite3_bind_int64(upd, 1, inbox_id);
        rc = sqlite3_step(upd);
        sqlite3_finalize(upd);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(sel);
            return -1;
        }
        consumed++;
    }
    sqlite3_finalize(sel);

    /* Update session leaf_id to last inserted entry */
    if (consumed > 0) {
        sqlite3_stmt *lf;
        if (sqlite3_prepare_v2(db,
            "UPDATE sessions SET leaf_id=?, updated_at=unixepoch() WHERE id=?",
            -1, &lf, NULL) != SQLITE_OK) {
            return -1;
        }
        sqlite3_bind_int64(lf, 1, parent_id);
        sqlite3_bind_int64(lf, 2, session_id);
        int rc = sqlite3_step(lf);
        sqlite3_finalize(lf);
        if (rc != SQLITE_DONE) {
            return -1;
        }
    }

    return consumed;
}

int inbox_consume_into_entries(sqlite3 *db, int64_t session_id, int limit) {
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return -1;
    int rc = inbox_consume_into_entries_locked(db, session_id, limit);
    if (rc < 0) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return rc;
}

/* agents table operations */

AgentRow *db_agent_get(sqlite3 *db, const char *name) {
    if (!db || !name) return NULL;
    const char *sql = "SELECT name, system_prompt, description, created_at FROM agents WHERE name = ?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);

    AgentRow *row = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        row = calloc(1, sizeof(AgentRow));
        if (row) {
            const char *v = (const char *)sqlite3_column_text(stmt, 0);
            row->name = v ? strdup(v) : NULL;
            v = (const char *)sqlite3_column_text(stmt, 1);
            row->system_prompt = v ? strdup(v) : NULL;
            v = (const char *)sqlite3_column_text(stmt, 2);
            row->description = v ? strdup(v) : NULL;
            row->created_at = sqlite3_column_int64(stmt, 3);
        }
    }
    sqlite3_finalize(stmt);
    return row;
}

/* List all agent names from agents table */
char **db_agent_list(sqlite3 *db, int *count) {
    *count = 0;
    if (!db) return NULL;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "SELECT name FROM agents ORDER BY name", -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    int cap = 8;
    char **names = malloc((size_t)cap * sizeof(char *));
    if (!names) { sqlite3_finalize(stmt); return NULL; }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *n = (const char *)sqlite3_column_text(stmt, 0);
        if (!n) continue;
        if (*count >= cap) {
            cap *= 2;
            char **tmp = realloc(names, (size_t)cap * sizeof(char *));
            if (!tmp) {
                for (int i = 0; i < *count; i++) free(names[i]);
                free(names);
                *count = 0;
                sqlite3_finalize(stmt);
                return NULL;
            }
            names = tmp;
        }
        names[*count] = strdup(n);
        (*count)++;
    }
    sqlite3_finalize(stmt);
    if (*count == 0) { free(names); return NULL; }
    return names;
}

char **db_sensitive_hosts(sqlite3 *db, int *count) {
    *count = 0;
    if (!db) return NULL;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
            "SELECT value FROM sensitive_targets WHERE kind='host' ORDER BY value",
            -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    int cap = 8;
    char **hosts = malloc((size_t)cap * sizeof(char *));
    if (!hosts) { sqlite3_finalize(stmt); return NULL; }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(stmt, 0);
        if (!v) continue;
        if (*count >= cap) {
            cap *= 2;
            char **tmp = realloc(hosts, (size_t)cap * sizeof(char *));
            if (!tmp) {
                for (int i = 0; i < *count; i++) free(hosts[i]);
                free(hosts);
                *count = 0;
                sqlite3_finalize(stmt);
                return NULL;
            }
            hosts = tmp;
        }
        hosts[*count] = strdup(v);
        (*count)++;
    }
    sqlite3_finalize(stmt);
    if (*count == 0) { free(hosts); return NULL; }
    return hosts;
}

static int sensitive_host_write(sqlite3 *db, const char *sql, const char *host) {
    if (!db || !host || !host[0]) return -1;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, host, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_sensitive_host_add(sqlite3 *db, const char *host) {
    return sensitive_host_write(db,
        "INSERT OR IGNORE INTO sensitive_targets(kind, value) VALUES('host', ?1)", host);
}

int db_sensitive_host_rm(sqlite3 *db, const char *host) {
    return sensitive_host_write(db,
        "DELETE FROM sensitive_targets WHERE kind='host' AND value=?1", host);
}

char **db_secret_hosts(sqlite3 *db, const char *secret_name, int *count) {
    *count = 0;
    if (!db || !secret_name) return NULL;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
            "SELECT host FROM secret_hosts WHERE secret_name=?1 ORDER BY host",
            -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(stmt, 1, secret_name, -1, SQLITE_STATIC);
    int cap = 8;
    char **hosts = malloc((size_t)cap * sizeof(char *));
    if (!hosts) { sqlite3_finalize(stmt); return NULL; }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(stmt, 0);
        if (!v) continue;
        if (*count >= cap) {
            cap *= 2;
            char **tmp = realloc(hosts, (size_t)cap * sizeof(char *));
            if (!tmp) {
                for (int i = 0; i < *count; i++) free(hosts[i]);
                free(hosts);
                *count = 0;
                sqlite3_finalize(stmt);
                return NULL;
            }
            hosts = tmp;
        }
        hosts[*count] = strdup(v);
        (*count)++;
    }
    sqlite3_finalize(stmt);
    if (*count == 0) { free(hosts); return NULL; }
    return hosts;
}

static int secret_host_write(sqlite3 *db, const char *sql,
                             const char *name, const char *host) {
    if (!db || !name || !name[0] || !host || !host[0]) return -1;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, host, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_secret_host_bind(sqlite3 *db, const char *secret_name, const char *host) {
    return secret_host_write(db,
        "INSERT OR IGNORE INTO secret_hosts(secret_name, host) VALUES(?1, ?2)",
        secret_name, host);
}

int db_secret_host_unbind(sqlite3 *db, const char *secret_name, const char *host) {
    return secret_host_write(db,
        "DELETE FROM secret_hosts WHERE secret_name=?1 AND host=?2",
        secret_name, host);
}

/* Secret store (specs/security.md): DB-backed secrets, encrypted at rest. */

int db_secret_set(sqlite3 *db, const char *name, const char *value,
                  const char *source, const char *status, const char *scope) {
    if (!db || !name || !name[0] || !value) return -1;
    if (!s_secret_key_loaded) return -1;
    char *enc = secret_encrypt(s_secret_key, value);
    if (!enc) return -1;
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO secrets(name, value, source, status, scope) VALUES(?1, ?2, ?3, ?4, ?5) "
        "ON CONFLICT(name) DO UPDATE SET value=excluded.value, source=excluded.source, "
        "status=excluded.status, scope=excluded.scope",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) { free(enc); return -1; }
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, enc, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, source ? source : "operator", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, status ? status : "active", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, scope ? scope : "agent", -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    free(enc);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* Decrypted value of one scope='system' secret (provider API keys). NULL if
 * absent, wrong scope, or the master key isn't loaded. */
char *db_secret_get_system(sqlite3 *db, const char *name) {
    if (!db || !name || !name[0] || !s_secret_key_loaded) return NULL;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
            "SELECT value FROM secrets WHERE name=?1 AND scope='system'",
            -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    char *plain = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *enc = (const char *)sqlite3_column_text(stmt, 0);
        if (enc) plain = secret_decrypt(s_secret_key, enc);
    }
    sqlite3_finalize(stmt);
    return plain;
}

int db_secret_rm(sqlite3 *db, const char *name) {
    if (!db || !name || !name[0]) return -1;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "DELETE FROM secrets WHERE name=?1", -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_secret_exists(sqlite3 *db, const char *name) {
    if (!db || !name || !name[0]) return 0;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM secrets WHERE name=?1", -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    int exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return exists;
}

ShellSecret *db_secrets_load(sqlite3 *db, size_t *count) {
    *count = 0;
    if (!db || !s_secret_key_loaded) return NULL;
    sqlite3_stmt *stmt;
    /* agent scope only: system secrets (provider keys) must never enter the
     * interpolation/injection snapshot. */
    if (sqlite3_prepare_v2(db,
            "SELECT name, value FROM secrets WHERE scope='agent' ORDER BY name",
            -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    size_t cap = 8, n = 0;
    ShellSecret *out = calloc(cap, sizeof(ShellSecret));
    if (!out) { sqlite3_finalize(stmt); return NULL; }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *nm = (const char *)sqlite3_column_text(stmt, 0);
        const char *enc = (const char *)sqlite3_column_text(stmt, 1);
        if (!nm || !enc) continue;
        char *plaintext = secret_decrypt(s_secret_key, enc);
        if (!plaintext) continue;  /* tamper/corruption: skip, don't fail the whole load */
        if (n == cap) {
            cap *= 2;
            ShellSecret *tmp = realloc(out, cap * sizeof(ShellSecret));
            if (!tmp) { crypto_wipe(plaintext, strlen(plaintext)); free(plaintext); break; }
            out = tmp;
        }
        out[n].name = strdup(nm);
        out[n].value = plaintext;
        n++;
    }
    sqlite3_finalize(stmt);
    *count = n;
    if (n == 0) { free(out); return NULL; }
    return out;
}

int db_secret_pending_count(sqlite3 *db) {
    if (!db) return 0;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM secrets WHERE status='pending'",
                          -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    int n = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return n;
}

int db_secret_set_status(sqlite3 *db, const char *name, const char *status) {
    if (!db || !name || !name[0] || !status) return -1;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "UPDATE secrets SET status=?1 WHERE name=?2",
                          -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, status, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_agent_upsert(sqlite3 *db, const char *name, const char *description,
                    const char *system_prompt) {
    if (!db || !name) return -1;
    const char *sql =
        "INSERT INTO agents (name, description, system_prompt) "
        "VALUES (?, ?, ?) "
        "ON CONFLICT(name) DO UPDATE SET "
        "description=COALESCE(excluded.description, agents.description),"
        "system_prompt=COALESCE(excluded.system_prompt, agents.system_prompt)";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, description, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, system_prompt, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* Read file contents, return heap-allocated string or NULL */
static char *read_file_str(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)len, f);
    buf[len] = '\0';
    fclose(f);
    return buf;
}

void memory_blocks_seed(sqlite3 *db, const char *agent_name, const char *agent_json_str) {
    if (!agent_json_str || !agent_name) return;
    const char *sql =
        "SELECT json_extract(value,'$.label'), json_extract(value,'$.description'),"
        " json_extract(value,'$.value'), json_extract(value,'$.char_limit'),"
        " json_extract(value,'$.read_only'), json_extract(value,'$.placement')"
        " FROM json_each(?1, '$.memory_blocks')";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, agent_json_str, -1, SQLITE_STATIC);
    /* Writes mid-iteration are safe only because json_each over a bound
     * parameter opens no b-tree cursor, so this SELECT pins no read snapshot.
     * If the query ever touches a real table, restructure to collect-then-
     * write (see channel_consume_events). */
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *lbl = (const char *)sqlite3_column_text(stmt, 0);
        if (!lbl) continue;
        MemoryBlock *existing = memory_block_get(db, agent_name, lbl);
        if (existing) { memory_block_free(existing); continue; }
        const char *desc = (const char *)sqlite3_column_text(stmt, 1);
        const char *val = (const char *)sqlite3_column_text(stmt, 2);
        int cl = sqlite3_column_type(stmt, 3) == SQLITE_INTEGER ? sqlite3_column_int(stmt, 3) : 5000;
        int ro = sqlite3_column_int(stmt, 4);
        const char *placement = (const char *)sqlite3_column_text(stmt, 5);
        int64_t id = memory_block_create(db, agent_name, lbl, desc, val, cl, placement);
        if (id > 0 && ro) {
            const char *ro_sql = "UPDATE memory_blocks SET read_only=1 WHERE id=?;";
            sqlite3_stmt *rstmt;
            if (sqlite3_prepare_v2(db, ro_sql, -1, &rstmt, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(rstmt, 1, id);
                sqlite3_step(rstmt);
                sqlite3_finalize(rstmt);
            }
        }
    }
    sqlite3_finalize(stmt);
}

AgentRow *db_agent_seed(sqlite3 *db, const char *agents_dir, const char *name) {
    if (!db || !name) return NULL;
    if (!is_valid_agent_name(name)) {
        fprintf(stderr, "db_agent_seed: skipping invalid agent name '%s' (must be PascalCase)\n", name);
        return NULL;
    }

    /* Check DB first — authoritative after seed */
    AgentRow *existing = db_agent_get(db, name);
    if (existing) {
        /* seed memory blocks even if agent row exists (blocks may be new) */
        return existing;
    }

    /* Not in DB — seed from disk */
    char path[1024];
    char *config_json = NULL;
    char *sys_prompt = NULL;
    char *description = NULL;

    if (agents_dir) {
        snprintf(path, sizeof(path), "%s/%s/agent.json", agents_dir, name);
        config_json = read_file_str(path);

        snprintf(path, sizeof(path), "%s/%s/system.md", agents_dir, name);
        sys_prompt = read_file_str(path);

        /* Extract description from agent.json via SQLite JSON */
        if (config_json) {
            sqlite3_stmt *jstmt;
            const char *jsql = "SELECT json_extract(?1, '$.description')";
            if (sqlite3_prepare_v2(db, jsql, -1, &jstmt, NULL) == SQLITE_OK) {
                sqlite3_bind_text(jstmt, 1, config_json, -1, SQLITE_STATIC);
                if (sqlite3_step(jstmt) == SQLITE_ROW) {
                    const char *v = (const char *)sqlite3_column_text(jstmt, 0);
                    if (v) description = strdup(v);
                }
                sqlite3_finalize(jstmt);
            }
        }
    }

    db_agent_upsert(db, name, description, sys_prompt);

    /* seed memory blocks from agent.json */
    if (config_json)
        memory_blocks_seed(db, name, config_json);

    free(description);
    free(config_json);
    free(sys_prompt);

    return db_agent_get(db, name);
}

void agent_row_free(AgentRow *row) {
    if (!row) return;
    free(row->name);
    free(row->description);
    free(row->system_prompt);
    free(row);
}

int agent_rename(sqlite3 *db, const char *old_name, const char *new_name,
                 int64_t requesting_session_id) {
    if (!db || !old_name || !new_name) return -3;
    if (!is_valid_agent_name(new_name)) return -3;

    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return -1;

    /* Check exists, busy sessions, and name conflict in one query */
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
        "SELECT EXISTS(SELECT 1 FROM agents WHERE name=?1),"
        " (SELECT COUNT(*) FROM sessions WHERE agent_name=?1 AND state!='idle' AND id!=?2),"
        " EXISTS(SELECT 1 FROM agents WHERE name=?3)",
        -1, &s, NULL) != SQLITE_OK) goto rollback_busy;
    sqlite3_bind_text(s, 1, old_name, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 2, requesting_session_id);
    sqlite3_bind_text(s, 3, new_name, -1, SQLITE_STATIC);

    int found = 0, busy = 0, conflict = 0;
    if (sqlite3_step(s) == SQLITE_ROW) {
        found = sqlite3_column_int(s, 0);
        busy = sqlite3_column_int(s, 1);
        conflict = sqlite3_column_int(s, 2);
    }
    sqlite3_finalize(s);

    if (!found) { sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL); return -4; }
    if (busy > 0) { sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL); return -1; }
    if (conflict) { sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL); return -2; }

    /* Cascade rename across all tables */
    const char *updates[] = {
        "UPDATE agents SET name=?1 WHERE name=?2",
        "UPDATE agent_extensions SET agent_name=?1 WHERE agent_name=?2",
        "UPDATE channel_routes SET agent_name=?1 WHERE agent_name=?2",
        "UPDATE sessions SET agent_name=?1 WHERE agent_name=?2",
        "UPDATE cron_jobs SET agent_name=?1 WHERE agent_name=?2",
        "UPDATE memory_blocks SET agent_name=?1 WHERE agent_name=?2",
        /* match effective value: a NULL override still means the default */
        "UPDATE config SET value=?1 WHERE key='default_agent' AND COALESCE(value, default_value)=?2",
    };
    for (size_t i = 0; i < sizeof(updates)/sizeof(updates[0]); i++) {
        if (sqlite3_prepare_v2(db, updates[i], -1, &s, NULL) != SQLITE_OK)
            goto rollback_busy;
        sqlite3_bind_text(s, 1, new_name, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, old_name, -1, SQLITE_STATIC);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }

    if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
        goto rollback_busy;
    return 0;

rollback_busy:
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return -1;
}

/* tool_calls status helpers */

int db_tool_call_set_status(sqlite3 *db, int64_t session_id, const char *call_id,
                            const char *status, const char *resolved_by) {
    const char *sql = resolved_by
        ? "UPDATE tool_calls SET status=?, resolved_by=?, resolved_at=unixepoch() WHERE session_id=? AND call_id=?;"
        : "UPDATE tool_calls SET status=? WHERE session_id=? AND call_id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, status, -1, SQLITE_STATIC);
    if (resolved_by) {
        sqlite3_bind_text(stmt, 2, resolved_by, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 3, session_id);
        sqlite3_bind_text(stmt, 4, call_id, -1, SQLITE_STATIC);
    } else {
        sqlite3_bind_int64(stmt, 2, session_id);
        sqlite3_bind_text(stmt, 3, call_id, -1, SQLITE_STATIC);
    }
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE && sqlite3_changes(db) > 0) ? 0 : -1;
}

int db_tool_call_complete_by_call(sqlite3 *db, int64_t session_id,
                                  const char *call_id, int64_t result_entry_id) {
    if (!db || !call_id) return -1;
    const char *sql =
        "UPDATE tool_calls SET status='done', result_entry_id=?"
        " WHERE session_id=? AND call_id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, result_entry_id);
    sqlite3_bind_int64(stmt, 2, session_id);
    sqlite3_bind_text(stmt, 3, call_id, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE && sqlite3_changes(db) > 0) ? 0 : -1;
}

int db_tool_call_any_running(sqlite3 *db, int64_t session_id) {
    return (int)db_scalar_i64(db,
        "SELECT EXISTS(SELECT 1 FROM tool_calls WHERE session_id=? AND status='running');",
        session_id, 0);
}

PendingToolCall *db_tool_call_get_pending(sqlite3 *db, int64_t session_id, int *out_count) {
    *out_count = 0;
    const char *sql =
        "SELECT tc.call_id, tc.name, tc.arguments, tc.entry_id, e.turn_id"
        " FROM tool_calls tc JOIN entries e ON e.id=tc.entry_id"
        " WHERE tc.session_id=? AND tc.status='pending'"
        " ORDER BY tc.rowid;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int64(stmt, 1, session_id);
    int cap = 8, count = 0;
    PendingToolCall *list = malloc((size_t)cap * sizeof(PendingToolCall));
    if (!list) { sqlite3_finalize(stmt); return NULL; }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            PendingToolCall *tmp = realloc(list, (size_t)cap * sizeof(PendingToolCall));
            if (!tmp) { db_tool_call_free_pending(list, count); sqlite3_finalize(stmt); return NULL; }
            list = tmp;
        }
        const char *s;
        s = (const char *)sqlite3_column_text(stmt, 0);
        list[count].call_id = s ? strdup(s) : strdup("");
        s = (const char *)sqlite3_column_text(stmt, 1);
        list[count].name = s ? strdup(s) : strdup("");
        s = (const char *)sqlite3_column_text(stmt, 2);
        list[count].arguments = s ? strdup(s) : strdup("{}");
        list[count].entry_id = sqlite3_column_int64(stmt, 3);
        list[count].turn_id = sqlite3_column_int64(stmt, 4);
        count++;
    }
    sqlite3_finalize(stmt);
    *out_count = count;
    if (count == 0) { free(list); return NULL; }
    return list;
}

void db_tool_call_free_pending(PendingToolCall *list, int count) {
    if (!list) return;
    for (int i = 0; i < count; i++) {
        free(list[i].call_id);
        free(list[i].name);
        free(list[i].arguments);
    }
    free(list);
}
/* memory_blocks CRUD */

void memory_block_free(MemoryBlock *mb) {
    if (!mb) return;
    free(mb->agent_name);
    free(mb->label);
    free(mb->value);
    free(mb->description);
    free(mb->placement);
    free(mb);
}

void memory_block_list_free(MemoryBlock *list, int count) {
    if (!list) return;
    for (int i = 0; i < count; i++) {
        free(list[i].agent_name);
        free(list[i].label);
        free(list[i].value);
        free(list[i].description);
        free(list[i].placement);
    }
    free(list);
}

int64_t memory_block_create(sqlite3 *db, const char *agent_name, const char *label,
                            const char *description, const char *value, int char_limit,
                            const char *placement) {
    if (!label || !is_valid_name(label)) return -1;
    const char *sql = "INSERT INTO memory_blocks(agent_name, label, description, value, char_limit, placement)"
                      " VALUES(?,?,?,?,?,?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, agent_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, label, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, description ? description : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, value ? value : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 5, char_limit > 0 ? char_limit : 5000);
    sqlite3_bind_text(stmt, 6, placement && placement[0] ? placement : "system", -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? sqlite3_last_insert_rowid(db) : -1;
}

MemoryBlock *memory_block_get(sqlite3 *db, const char *agent_name, const char *label) {
    const char *sql = "SELECT id, agent_name, label, value, description, char_limit, read_only,"
                      " placement, created_at, updated_at FROM memory_blocks WHERE agent_name=? AND label=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(stmt, 1, agent_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, label, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return NULL; }
    MemoryBlock *mb = calloc(1, sizeof(MemoryBlock));
    mb->id = sqlite3_column_int64(stmt, 0);
    const char *s;
    s = (const char *)sqlite3_column_text(stmt, 1); mb->agent_name = s ? strdup(s) : strdup("");
    s = (const char *)sqlite3_column_text(stmt, 2); mb->label = s ? strdup(s) : strdup("");
    s = (const char *)sqlite3_column_text(stmt, 3); mb->value = s ? strdup(s) : strdup("");
    s = (const char *)sqlite3_column_text(stmt, 4); mb->description = s ? strdup(s) : strdup("");
    mb->char_limit = sqlite3_column_int(stmt, 5);
    mb->read_only = sqlite3_column_int(stmt, 6);
    s = (const char *)sqlite3_column_text(stmt, 7); mb->placement = s ? strdup(s) : strdup("system");
    mb->created_at = sqlite3_column_int64(stmt, 8);
    mb->updated_at = sqlite3_column_int64(stmt, 9);
    sqlite3_finalize(stmt);
    return mb;
}

MemoryBlock *memory_block_list(sqlite3 *db, const char *agent_name, int *count) {
    *count = 0;
    const char *sql = "SELECT id, agent_name, label, value, description, char_limit, read_only,"
                      " placement, created_at, updated_at FROM memory_blocks WHERE agent_name=? ORDER BY id;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(stmt, 1, agent_name, -1, SQLITE_STATIC);
    int cap = 8;
    MemoryBlock *list = calloc((size_t)cap, sizeof(MemoryBlock));
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (*count >= cap) {
            cap *= 2;
            MemoryBlock *tmp = realloc(list, (size_t)cap * sizeof(MemoryBlock));
            if (!tmp) { memory_block_list_free(list, *count); sqlite3_finalize(stmt); *count = 0; return NULL; }
            list = tmp;
        }
        MemoryBlock *mb = &list[*count];
        mb->id = sqlite3_column_int64(stmt, 0);
        const char *s;
        s = (const char *)sqlite3_column_text(stmt, 1); mb->agent_name = s ? strdup(s) : strdup("");
        s = (const char *)sqlite3_column_text(stmt, 2); mb->label = s ? strdup(s) : strdup("");
        s = (const char *)sqlite3_column_text(stmt, 3); mb->value = s ? strdup(s) : strdup("");
        s = (const char *)sqlite3_column_text(stmt, 4); mb->description = s ? strdup(s) : strdup("");
        mb->char_limit = sqlite3_column_int(stmt, 5);
        mb->read_only = sqlite3_column_int(stmt, 6);
        s = (const char *)sqlite3_column_text(stmt, 7); mb->placement = s ? strdup(s) : strdup("system");
        mb->created_at = sqlite3_column_int64(stmt, 8);
        mb->updated_at = sqlite3_column_int64(stmt, 9);
        (*count)++;
    }
    sqlite3_finalize(stmt);
    if (*count == 0) { free(list); return NULL; }
    return list;
}

int memory_block_set_value(sqlite3 *db, const char *agent_name, const char *label, const char *value) {
    /* Guarded UPDATE: read_only + char_limit enforced in WHERE (TOCTOU-free). */
    const char *sql = "UPDATE memory_blocks SET value=?1, updated_at=unixepoch()"
                      " WHERE agent_name=?2 AND label=?3 AND read_only=0"
                      " AND length(?1) <= char_limit;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, value ? value : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, agent_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, label, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE && sqlite3_changes(db) == 1) ? 0 : -1;
}

/* memory_entries CRUD */

MemoryEntry *memory_entries_list(sqlite3 *db, const char *agent_name,
                                 const char *block_label, int *count) {
    *count = 0;
    const char *sql = "SELECT pos, text FROM memory_entries"
                      " WHERE agent_name=? AND block_label=? ORDER BY pos;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(stmt, 1, agent_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, block_label, -1, SQLITE_STATIC);
    int cap = 8;
    MemoryEntry *list = calloc((size_t)cap, sizeof(MemoryEntry));
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (*count >= cap) {
            cap *= 2;
            MemoryEntry *tmp = realloc(list, (size_t)cap * sizeof(MemoryEntry));
            if (!tmp) { memory_entries_free(list, *count); sqlite3_finalize(stmt); *count = 0; return NULL; }
            list = tmp;
        }
        MemoryEntry *e = &list[*count];
        e->pos = sqlite3_column_int(stmt, 0);
        const char *s = (const char *)sqlite3_column_text(stmt, 1);
        e->text = s ? strdup(s) : strdup("");
        (*count)++;
    }
    sqlite3_finalize(stmt);
    if (*count == 0) { free(list); return NULL; }
    return list;
}

void memory_entries_free(MemoryEntry *list, int count) {
    if (!list) return;
    for (int i = 0; i < count; i++) free(list[i].text);
    free(list);
}

int memory_entry_add(sqlite3 *db, const char *agent_name,
                     const char *block_label, const char *text) {
    const char *sql = "INSERT INTO memory_entries(agent_name, block_label, pos, text)"
                      " VALUES(?1,?2,(SELECT COALESCE(MAX(pos),0)+1 FROM memory_entries"
                      " WHERE agent_name=?1 AND block_label=?2),?3);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, agent_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, block_label, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, text, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    /* Retrieve the pos of the just-inserted row */
    int64_t rowid = sqlite3_last_insert_rowid(db);
    const char *pos_sql = "SELECT pos FROM memory_entries WHERE id=?;";
    if (sqlite3_prepare_v2(db, pos_sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, rowid);
    int pos = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) pos = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return pos;
}

int memory_entry_add_guarded(sqlite3 *db, const char *agent_name,
                             const char *block_label, const char *text) {
    /* Append entry only if sum of existing + new fits char_limit and block is writable. */
    const char *sql =
        "INSERT INTO memory_entries (agent_name, block_label, pos, text)"
        " SELECT ?1, ?2,"
        " (SELECT COALESCE(MAX(pos),0)+1 FROM memory_entries WHERE agent_name=?1 AND block_label=?2),"
        " ?3"
        " WHERE EXISTS ("
        "  SELECT 1 FROM memory_blocks b"
        "  WHERE b.agent_name=?1 AND b.label=?2 AND b.read_only=0"
        "   AND (SELECT COALESCE(SUM(length(text)),0) FROM memory_entries"
        "        WHERE agent_name=?1 AND block_label=?2) + length(?3) <= b.char_limit"
        ");";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, agent_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, block_label, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, text, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    return (sqlite3_changes(db) == 1) ? 1 : 0;
}

int memory_entry_set(sqlite3 *db, const char *agent_name,
                     const char *block_label, int number, const char *text) {
    const char *sql = "UPDATE memory_entries SET text=?"
                      " WHERE agent_name=? AND block_label=? AND pos=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, text, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, agent_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, block_label, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, number);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (sqlite3_changes(db) > 0) ? 0 : -1;
}

int memory_entries_delete(sqlite3 *db, const char *agent_name,
                          const char *block_label, const int *numbers, int n_numbers) {
    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
    int deleted = 0;
    const char *del_sql = "DELETE FROM memory_entries"
                          " WHERE agent_name=? AND block_label=? AND pos=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, del_sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return 0;
    }
    for (int i = 0; i < n_numbers; i++) {
        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, agent_name, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, block_label, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 3, numbers[i]);
        sqlite3_step(stmt);
        deleted += sqlite3_changes(db);
    }
    sqlite3_finalize(stmt);
    /* Renumber survivors contiguously */
    const char *renum_sql =
        "WITH ranked AS ("
        "  SELECT id, ROW_NUMBER() OVER (ORDER BY pos) AS rn"
        "  FROM memory_entries WHERE agent_name=?1 AND block_label=?2)"
        " UPDATE memory_entries"
        "  SET pos = (SELECT rn FROM ranked WHERE ranked.id = memory_entries.id)"
        "  WHERE agent_name=?1 AND block_label=?2;";
    if (sqlite3_prepare_v2(db, renum_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, agent_name, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, block_label, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    } else {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    }
    return deleted;
}

int rate_limit_check(sqlite3 *db, const char *provider_name) {
    if (!db) return 1;
    int limit = 0;
    char model_buf[128] = "";

    if (provider_name) {
        const char *lsql = "SELECT token_rate_limit, default_model_id FROM providers WHERE name=?;";
        sqlite3_stmt *ls;
        if (sqlite3_prepare_v2(db, lsql, -1, &ls, NULL) != SQLITE_OK) return 1;
        sqlite3_bind_text(ls, 1, provider_name, -1, SQLITE_STATIC);
        if (sqlite3_step(ls) == SQLITE_ROW) {
            limit = sqlite3_column_int(ls, 0);
            const char *m = (const char *)sqlite3_column_text(ls, 1);
            if (m) snprintf(model_buf, sizeof(model_buf), "%s", m);
        }
        sqlite3_finalize(ls);
    }
    if (limit <= 0) return 1; /* unlimited or not found */

    const char *usql = model_buf[0]
        ? "SELECT COALESCE(SUM(usage_in+usage_out),0) FROM entries"
          " WHERE model=? AND created_at > unixepoch()-3600;"
        : "SELECT COALESCE(SUM(usage_in+usage_out),0) FROM entries"
          " WHERE created_at > unixepoch()-3600;";
    sqlite3_stmt *us;
    if (sqlite3_prepare_v2(db, usql, -1, &us, NULL) != SQLITE_OK) return 1;
    if (model_buf[0]) sqlite3_bind_text(us, 1, model_buf, -1, SQLITE_STATIC);
    int used = 0;
    if (sqlite3_step(us) == SQLITE_ROW) used = sqlite3_column_int(us, 0);
    sqlite3_finalize(us);
    return used < limit ? 1 : 0;
}

/* Set of session states that mean "a turn was in flight" — a crash here leaves
 * the edge-triggered loop with no event to re-advance them. */
#define RECOVER_STALE_STATES \
    "('llm_running','tool_running','compacting','awaiting_agent','awaiting_approval','rate_limited')"

/* Dead-owner predicate: a stale-state session whose owner is gone. owner NULL
 * (legacy/idle-races) or an owner with no live processes row both qualify; a
 * live peer's sessions are spared because its instance_id is still registered. */
#define RECOVER_DEAD_OWNER \
    "state IN " RECOVER_STALE_STATES \
    " AND (owner_instance IS NULL" \
    "      OR owner_instance NOT IN (SELECT instance_id FROM processes))"

/* Crash recovery, owner-scoped: any live instance may call this (startup and
 * the periodic path) and it reclaims only dead-owned stale sessions — a live
 * peer's in-flight sessions are never stomped. For each such session: reconcile
 * orphaned tool_calls (synthetic error result + mark done), drop its inert
 * llm_jobs dead-letters, deny its stale pending approvals, then reset it to
 * idle. Reconcile-before-reset matters: every step keys on the dead-owner
 * predicate, which includes the stale state, so the reset must run last or it
 * would clear the state the earlier steps select on. Returns 0, -1 on error. */
int db_recover_stale_sessions(sqlite3 *db) {
    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK)
        return -1;

    /* a) Collect orphaned tool_calls for dead-owned sessions — anything that
     * hadn't produced a result when the owner died. 'pending' = never
     * dispatched; 'running' = a forked tool or sub-agent was in flight. Both
     * leave the assistant turn with an unanswered tool_call, so reconcile them
     * identically: synthetic error result + mark done. */
    const char *orphan_sql =
        "SELECT session_id, call_id, name FROM tool_calls"
        " WHERE status IN ('pending','running')"
        "   AND NOT EXISTS (SELECT 1 FROM entries e"
        "                   WHERE e.session_id = tool_calls.session_id"
        "                     AND e.tool_call_id = tool_calls.call_id"
        "                     AND e.type='tool_result')"
        "   AND session_id IN (SELECT id FROM sessions WHERE " RECOVER_DEAD_OWNER ");";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, orphan_sql, -1, &stmt, NULL) != SQLITE_OK)
        goto rollback;

    typedef struct { int64_t session_id; char *call_id; char *name; } OrphanRow;
    int cap = 8, count = 0;
    OrphanRow *rows = malloc(cap * sizeof(OrphanRow));
    if (!rows) { sqlite3_finalize(stmt); goto rollback; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            OrphanRow *tmp = realloc(rows, cap * sizeof(OrphanRow));
            if (!tmp) { sqlite3_finalize(stmt); goto free_rollback; }
            rows = tmp;
        }
        rows[count].session_id = sqlite3_column_int64(stmt, 0);
        rows[count].call_id = strdup((const char *)sqlite3_column_text(stmt, 1));
        rows[count].name = strdup((const char *)sqlite3_column_text(stmt, 2));
        count++;
    }
    sqlite3_finalize(stmt);

    /* Write synthetic error results + mark done */
    for (int i = 0; i < count; i++) {
        ToolResult tr = {.tool_call_id = rows[i].call_id,
                         .content = "error: interrupted by daemon restart"};
        Message msg = {.role = ROLE_TOOL, .tool_result = &tr,
                       .tool_name = rows[i].name, .is_error = 1};
        if (entry_append_with_turn(db, rows[i].session_id, &msg, 0) < 0)
            goto free_rollback;
        if (db_tool_call_set_status(db, rows[i].session_id, rows[i].call_id,
                                    "done", "recovery") != 0)
            goto free_rollback;
    }

    for (int i = 0; i < count; i++) { free(rows[i].call_id); free(rows[i].name); }
    free(rows);

    /* b) Drop inert llm_jobs for dead-owned sessions. A crashed process's job
     * rows are dead-letters (jobs only ever enter via in-process pool_push, and
     * a worker deletes its own row), so no live peer will claim them — reclaim
     * is just deletion. */
    const char *orphan_jobs_sql =
        "DELETE FROM llm_jobs WHERE session_id IN"
        " (SELECT id FROM sessions WHERE " RECOVER_DEAD_OWNER ");";
    if (sqlite3_exec(db, orphan_jobs_sql, NULL, NULL, NULL) != SQLITE_OK)
        goto rollback;

    /* c) Deny pending approvals for dead-owned sessions (their approver is
     * gone). Keyed on the dead-owner predicate directly — must precede the
     * reset, which would otherwise clear the stale state this selects on. */
    const char *orphan_approvals_sql =
        "UPDATE approvals SET state='denied', decided_via='recovery'"
        " WHERE state='pending'"
        "   AND session_id IN (SELECT id FROM sessions WHERE " RECOVER_DEAD_OWNER ");";
    if (sqlite3_exec(db, orphan_approvals_sql, NULL, NULL, NULL) != SQLITE_OK)
        goto rollback;

    /* d) Reset the dead-owned stale sessions to idle (clearing owner + zeroing
     * iteration). Runs last so the predicate still matched for steps a–c. */
    const char *reset_sql =
        "UPDATE sessions SET state='idle', turn_iteration=0, owner_instance=NULL,"
        " updated_at=unixepoch() WHERE " RECOVER_DEAD_OWNER ";";
    if (sqlite3_exec(db, reset_sql, NULL, NULL, NULL) != SQLITE_OK)
        goto rollback;

    if (sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK)
        return -1;
    return 0;

free_rollback:
    for (int i = 0; i < count; i++) { free(rows[i].call_id); free(rows[i].name); }
    free(rows);
rollback:
    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    return -1;
}


/* ── Periodic pruning: inbox ──────────────────────────────────────────── */
void db_prune_inbox(sqlite3 *db) {
    int ret = config_default_int("inbox_retention_sec");
    if (ret <= 0) return;
    const char *sql =
        "DELETE FROM inbox WHERE consumed=1"
        " AND created_at < (unixepoch() - ?1);";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(s, 1, (int64_t)ret);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

/* ── Periodic pruning: channel_outbox ─────────────────────────────────── */
void db_prune_outbox(sqlite3 *db) {
    int ret = config_default_int("outbox_retention_sec");
    if (ret <= 0) return;
    const char *sql =
        "DELETE FROM channel_outbox"
        " WHERE (status='delivered' OR status LIKE 'failed%')"
        " AND COALESCE(acked_at, created_at) < (unixepoch() - ?1);";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(s, 1, (int64_t)ret);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

/* ── WAL checkpoint ──────────────────────────────────────────────────── */
/* Best-effort WAL truncate checkpoint.  TRUNCATE resets the -wal file to
   zero bytes after checkpointing, so a long-lived reader that stalled the
   passive auto-checkpoint can't leave the WAL growing unbounded. */
void db_wal_checkpoint(sqlite3 *db) {
    if (!db) return;
    sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
}
