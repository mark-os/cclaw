#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "secret_scan.h"
#include "agent_config.h"
#include "secret.h"
#include "validate.h"
#include "templates.h"
#include "monocypher.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

static const char *SCHEMA_SQL = TPL_SCHEMA_SQL;

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
sqlite3 *db_open(const char *path) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_open: %s (extended=%d)\n",
                sqlite3_errmsg(db), sqlite3_extended_errcode(db));
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA busy_timeout=5000;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA foreign_keys=OFF;", NULL, NULL, NULL);
    return db;
}

/* Apply schema (CREATE TABLE IF NOT EXISTS). Call once from main at startup. */
int db_ensure_schema(sqlite3 *db) {
    char *err = NULL;
    int rc = sqlite3_exec(db, SCHEMA_SQL, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_ensure_schema: %s\n", err);
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
            fprintf(stderr, "db_ensure_schema: fts rebuild: %s\n", err);
            sqlite3_free(err);  /* non-fatal — recall degrades, chat still works */
        }
    }
    return 0;
}

/* V57: mmap + reduced cache + relaxed sync for child processes (short-lived). */
void db_set_child_pragmas(sqlite3 *db) {
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA mmap_size=67108864;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA cache_size=-512;", NULL, NULL, NULL);
}

/* Seed default config + provider on first run. Call once from main(). */
int db_seed_defaults(sqlite3 *db) {
    if (!db) return -1;
    sqlite3_stmt *cnt;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM config", -1, &cnt, NULL) != SQLITE_OK)
        return -1;
    int empty = 0;
    if (sqlite3_step(cnt) == SQLITE_ROW && sqlite3_column_int(cnt, 0) == 0)
        empty = 1;
    sqlite3_finalize(cnt);
    if (!empty) return 0;

    char *err = NULL;
    int rc = sqlite3_exec(db, TPL_SEED_SQL, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_seed_defaults: %s\n", err);
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

int64_t session_create(sqlite3 *db, const char *name, const char *agent_name,
                       int64_t parent_session_id, int depth) {
    const char *sql = "INSERT INTO sessions (name, agent_name, parent_session_id, depth) VALUES (?, ?, ?, ?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    if (name) sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC); else sqlite3_bind_null(stmt, 1);
    if (agent_name) sqlite3_bind_text(stmt, 2, agent_name, -1, SQLITE_STATIC); else sqlite3_bind_null(stmt, 2);
    sqlite3_bind_int64(stmt, 3, parent_session_id > 0 ? parent_session_id : -1);
    sqlite3_bind_int(stmt, 4, depth);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return -1;
    }
    int64_t id = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);
    return id;
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
/* V14: walk parent_id chain from leaf→root, return in root→leaf order */
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
     * V58: use level counter for ordering (not ORDER BY id) since compaction
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

/* T263: Get latest assistant response text from session branch.
 * Note: OpenClaw concatenates ALL non-empty assistant texts from the turn as
 * fallback; we only return the last non-empty one. Revisit if needed.
 * Returns heap-allocated string or NULL if no deliverable content. */
char *get_response_text(sqlite3 *db, int64_t session_id) {
    int count = 0;
    Entry *entries = session_get_branch(db, session_id, &count);
    if (!entries || count == 0) return NULL;

    char *result = NULL;
    for (int i = count - 1; i >= 0; i--) {
        if (entries[i].message.role == ROLE_ASSISTANT &&
            entries[i].message.content && entries[i].message.content[0]) {
            result = strdup(entries[i].message.content);
            break;
        }
        /* Stop at user boundary — don't leak previous turn's response */
        if (entries[i].message.role == ROLE_USER) break;
    }
    entry_branch_free(entries, count);
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

/* V58,V59: Insert compaction summary entry and reparent.
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

    /* Reparent first_after_id → compact_id, save original_parent_id (V59) */
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


/* V7: FTS5 search over message content */
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

/* T268: Sum cost_nano for all entries in a session */
int64_t session_cost(sqlite3 *db, int64_t session_id) {
    return db_scalar_i64(db, "SELECT COALESCE(SUM(cost_nano),0) FROM entries WHERE session_id=?;", session_id, 0);
}

/* Key-value store for persistent settings (e.g. Telegram offset) */
char *db_kv_get(sqlite3 *db, const char *key) {
    const char *sql = "SELECT value FROM config WHERE key=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    char *val = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(stmt, 0);
        if (v) val = strdup(v);
    }
    sqlite3_finalize(stmt);
    return val;
}

int db_kv_set(sqlite3 *db, const char *key, const char *value) {
    const char *sql = "INSERT OR REPLACE INTO config (key, value) VALUES (?, ?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, value, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* V52,T171: Secret key for kv encryption. Set via db_set_secret_key(). */
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

/* V52,T171: Secret-aware kv access with ChaCha20-Poly1305 AEAD. */
char *db_kv_get_secret(sqlite3 *db, const char *key) {
    char *raw = db_kv_get(db, key);
    if (!raw) return NULL;
    if (strncmp(raw, "enc:", 4) != 0) return raw; /* plaintext legacy value */
    if (!s_secret_key_loaded) { free(raw); return NULL; }
    char *plaintext = secret_decrypt(s_secret_key, raw);
    free(raw);
    return plaintext;
}

int db_kv_set_secret(sqlite3 *db, const char *key, const char *value) {
    if (!s_secret_key_loaded) return db_kv_set(db, key, value);
    char *encrypted = secret_encrypt(s_secret_key, value);
    if (!encrypted) return -1;
    int rc = db_kv_set(db, key, encrypted);
    free(encrypted);
    return rc;
}

/* T193/V69: Channel→agent binding */

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


/* V3: sub-agent limits — count active child sessions */

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

/* V17: next turn_id for a session */
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

/* V17: append entry with explicit turn_id */
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
    const char *sql =
        "UPDATE sessions SET state=?, updated_at=unixepoch(),"
        " turn_iteration = CASE WHEN ?='idle' THEN 0 ELSE turn_iteration END"
        " WHERE id=? AND ("
        "  (? IN ('llm_running','tool_running','compacting','rate_limited')"
        "     AND state IN ('idle','llm_running','tool_running','compacting','rate_limited','awaiting_approval')) OR"
        "  (? = 'awaiting_agent' AND state IN ('idle','llm_running','tool_running')) OR"
        "  (? = 'awaiting_approval' AND state IN ('llm_running','tool_running')) OR"
        "  (? = 'idle' AND state IN"
        "     ('llm_running','tool_running','compacting','rate_limited','awaiting_agent','awaiting_approval'))"
        ");";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, state, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, state, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, session_id);
    sqlite3_bind_text(stmt, 4, state, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, state, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, state, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, state, -1, SQLITE_STATIC);
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

/* V18: Inbox primitives */

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
    size_t len = strlen(payload);
    ScanFinding f[SCAN_MAX_FINDINGS];
    int n = secret_scan(payload, len, f, SCAN_MAX_FINDINGS);
    if (n == 0)
        return inbox_insert(db, session_id, source, payload);
    /* Findings present — copy and redact */
    size_t cap = len + 1 + (size_t)n * 80;
    char *buf = malloc(cap);
    if (!buf)
        return inbox_insert(db, session_id, source, payload);
    memcpy(buf, payload, len + 1);
    secret_scan_redact(buf, &len, cap);
    int64_t id = inbox_insert(db, session_id, source, buf);
    free(buf);
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

/* V18: Atomically consume inbox items into session entries */
int inbox_consume_into_entries(sqlite3 *db, int64_t session_id, int limit) {
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return -1;

    /* Peek unconsumed items */
    sqlite3_stmt *sel;
    if (sqlite3_prepare_v2(db,
        "SELECT id, payload FROM inbox WHERE session_id = ? AND consumed = 0 ORDER BY id ASC LIMIT ?",
        -1, &sel, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    sqlite3_bind_int64(sel, 1, session_id);
    sqlite3_bind_int(sel, 2, limit);

    /* Get current leaf */
    sqlite3_stmt *leaf_stmt;
    if (sqlite3_prepare_v2(db, "SELECT leaf_id FROM sessions WHERE id=?", -1, &leaf_stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(sel);
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    sqlite3_bind_int64(leaf_stmt, 1, session_id);
    if (sqlite3_step(leaf_stmt) != SQLITE_ROW) {
        sqlite3_finalize(leaf_stmt);
        sqlite3_finalize(sel);
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
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
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
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
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            return -1;
        }
        parent_id = sqlite3_last_insert_rowid(db);

        /* Mark consumed */
        sqlite3_stmt *upd;
        if (sqlite3_prepare_v2(db,
            "UPDATE inbox SET consumed = 1 WHERE id = ?", -1, &upd, NULL) != SQLITE_OK) {
            sqlite3_finalize(sel);
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            return -1;
        }
        sqlite3_bind_int64(upd, 1, inbox_id);
        rc = sqlite3_step(upd);
        sqlite3_finalize(upd);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(sel);
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
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
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            return -1;
        }
        sqlite3_bind_int64(lf, 1, parent_id);
        sqlite3_bind_int64(lf, 2, session_id);
        int rc = sqlite3_step(lf);
        sqlite3_finalize(lf);
        if (rc != SQLITE_DONE) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            return -1;
        }
    }

    if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        return -1;
    }
    return consumed;
}

/* T88/T202: Spawn queue — cclaw.db only (V73). Daemon inserts inline in reap_children. */

SpawnRequest *spawn_queue_peek_pending(sqlite3 *db, int *count) {
    *count = 0;
    const char *sql =
        "SELECT id, parent_session_id, task, background, depth, tool_call_id, child_agent"
        " FROM spawn_queue WHERE status='pending' ORDER BY id ASC;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;

    int cap = 8;
    SpawnRequest *list = malloc((size_t)cap * sizeof(SpawnRequest));
    if (!list) { sqlite3_finalize(stmt); return NULL; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (*count >= cap) {
            cap *= 2;
            SpawnRequest *tmp = realloc(list, (size_t)cap * sizeof(SpawnRequest));
            if (!tmp) {
                for (int i = 0; i < *count; i++) {
                    free(list[i].task);
                    free(list[i].tool_call_id);
                    free(list[i].child_agent);
                }
                free(list);
                *count = 0;
                sqlite3_finalize(stmt);
                return NULL;
            }
            list = tmp;
        }
        SpawnRequest *r = &list[*count];
        r->id = sqlite3_column_int64(stmt, 0);
        r->parent_session_id = sqlite3_column_int64(stmt, 1);
        const char *t = (const char *)sqlite3_column_text(stmt, 2);
        r->task = t ? strdup(t) : NULL;
        r->background = sqlite3_column_int(stmt, 3);
        r->depth = sqlite3_column_int(stmt, 4);
        const char *tc = (const char *)sqlite3_column_text(stmt, 5);
        r->tool_call_id = tc ? strdup(tc) : NULL;
        const char *ca = (const char *)sqlite3_column_text(stmt, 6);
        r->child_agent = ca ? strdup(ca) : NULL;
        (*count)++;
    }
    sqlite3_finalize(stmt);
    if (*count == 0) { free(list); return NULL; }
    return list;
}


/* T119: agents table operations */

AgentRow *db_agent_get(sqlite3 *db, const char *name) {
    if (!db || !name) return NULL;
    const char *sql = "SELECT name, system_prompt, created_at FROM agents WHERE name = ?";
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
            row->created_at = sqlite3_column_int64(stmt, 2);
            row->id = 0;
            row->config = NULL;
            row->heartbeat = NULL;
            row->updated_at = row->created_at;
        }
    }
    sqlite3_finalize(stmt);
    return row;
}

/* T271: List all agent names from agents table */
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

int db_agent_upsert(sqlite3 *db, const char *name, const char *config,
                    const char *system_prompt, const char *heartbeat) {
    (void)config; (void)heartbeat;
    if (!db || !name) return -1;
    const char *sql =
        "INSERT INTO agents (name, system_prompt) "
        "VALUES (?, ?) "
        "ON CONFLICT(name) DO UPDATE SET "
        "system_prompt=COALESCE(excluded.system_prompt, agents.system_prompt)";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, system_prompt, -1, SQLITE_STATIC);
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
        " json_extract(value,'$.read_only')"
        " FROM json_each(?1, '$.memory_blocks')";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, agent_json_str, -1, SQLITE_STATIC);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *lbl = (const char *)sqlite3_column_text(stmt, 0);
        if (!lbl) continue;
        MemoryBlock *existing = memory_block_get(db, agent_name, lbl);
        if (existing) { memory_block_free(existing); continue; }
        const char *desc = (const char *)sqlite3_column_text(stmt, 1);
        const char *val = (const char *)sqlite3_column_text(stmt, 2);
        int cl = sqlite3_column_type(stmt, 3) == SQLITE_INTEGER ? sqlite3_column_int(stmt, 3) : 5000;
        int ro = sqlite3_column_int(stmt, 4);
        int64_t id = memory_block_create(db, agent_name, lbl, desc, val, cl);
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

    /* Check DB first — authoritative after seed */
    AgentRow *existing = db_agent_get(db, name);
    if (existing) {
        /* T152: seed memory blocks even if agent row exists (blocks may be new) */
        if (existing->config)
            memory_blocks_seed(db, name, existing->config);
        return existing;
    }

    /* Not in DB — seed from disk */
    char path[1024];
    char *config_json = NULL;
    char *sys_prompt = NULL;

    if (agents_dir) {
        snprintf(path, sizeof(path), "%s/%s/agent.json", agents_dir, name);
        config_json = read_file_str(path);

        snprintf(path, sizeof(path), "%s/%s/system.md", agents_dir, name);
        sys_prompt = read_file_str(path);
    }

    db_agent_upsert(db, name, config_json, sys_prompt, NULL);

    /* T152: seed memory blocks from agent.json */
    if (config_json)
        memory_blocks_seed(db, name, config_json);

    free(config_json);
    free(sys_prompt);

    return db_agent_get(db, name);
}

void agent_row_free(AgentRow *row) {
    if (!row) return;
    free(row->name);
    free(row->config);
    free(row->system_prompt);
    free(row->heartbeat);
    free(row);
}

int agent_rename(sqlite3 *db, const char *old_name, const char *new_name,
                 int64_t requesting_session_id) {
    if (!db || !old_name || !new_name) return -3;
    if (!is_valid_name(new_name)) return -3;

    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return -1;

    /* Check agent exists */
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM agents WHERE name=?", -1, &s, NULL) != SQLITE_OK)
        goto rollback_busy;
    sqlite3_bind_text(s, 1, old_name, -1, SQLITE_STATIC);
    int found = (sqlite3_step(s) == SQLITE_ROW);
    sqlite3_finalize(s);
    if (!found) { sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL); return -4; }

    /* Check no active sessions (excluding the requesting one) */
    if (sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM sessions WHERE agent_name=? AND state!='idle' AND id!=?",
        -1, &s, NULL) != SQLITE_OK) goto rollback_busy;
    sqlite3_bind_text(s, 1, old_name, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 2, requesting_session_id);
    int busy = 0;
    if (sqlite3_step(s) == SQLITE_ROW) busy = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    if (busy > 0) { sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL); return -1; }

    /* Check new name not taken */
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM agents WHERE name=?", -1, &s, NULL) != SQLITE_OK)
        goto rollback_busy;
    sqlite3_bind_text(s, 1, new_name, -1, SQLITE_STATIC);
    int conflict = (sqlite3_step(s) == SQLITE_ROW);
    sqlite3_finalize(s);
    if (conflict) { sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL); return -2; }

    /* Cascade rename across all tables */
    const char *updates[] = {
        "UPDATE agents SET name=?1 WHERE name=?2",
        "UPDATE agent_extensions SET agent_name=?1 WHERE agent_name=?2",
        "UPDATE channel_routes SET agent_name=?1 WHERE agent_name=?2",
        "UPDATE sessions SET agent_name=?1 WHERE agent_name=?2",
        "UPDATE cron_jobs SET agent_name=?1 WHERE agent_name=?2",
        "UPDATE memory_blocks SET agent_name=?1 WHERE agent_name=?2",
        "UPDATE config SET value=?1 WHERE key='default_agent' AND value=?2",
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
        if (count >= cap) { cap *= 2; list = realloc(list, (size_t)cap * sizeof(PendingToolCall)); }
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
/* T152: memory_blocks CRUD (V55) */

void memory_block_free(MemoryBlock *mb) {
    if (!mb) return;
    free(mb->agent_name);
    free(mb->label);
    free(mb->value);
    free(mb->description);
    free(mb);
}

void memory_block_list_free(MemoryBlock *list, int count) {
    if (!list) return;
    for (int i = 0; i < count; i++) {
        free(list[i].agent_name);
        free(list[i].label);
        free(list[i].value);
        free(list[i].description);
    }
    free(list);
}

int64_t memory_block_create(sqlite3 *db, const char *agent_name, const char *label,
                            const char *description, const char *value, int char_limit) {
    if (!label || !is_valid_name(label)) return -1;
    const char *sql = "INSERT INTO memory_blocks(agent_name, label, description, value, char_limit)"
                      " VALUES(?,?,?,?,?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, agent_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, label, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, description ? description : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, value ? value : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 5, char_limit > 0 ? char_limit : 5000);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? sqlite3_last_insert_rowid(db) : -1;
}

MemoryBlock *memory_block_get(sqlite3 *db, const char *agent_name, const char *label) {
    const char *sql = "SELECT id, agent_name, label, value, description, char_limit, read_only,"
                      " created_at, updated_at FROM memory_blocks WHERE agent_name=? AND label=?;";
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
    mb->created_at = sqlite3_column_int64(stmt, 7);
    mb->updated_at = sqlite3_column_int64(stmt, 8);
    sqlite3_finalize(stmt);
    return mb;
}

MemoryBlock *memory_block_list(sqlite3 *db, const char *agent_name, int *count) {
    *count = 0;
    const char *sql = "SELECT id, agent_name, label, value, description, char_limit, read_only,"
                      " created_at, updated_at FROM memory_blocks WHERE agent_name=? ORDER BY id;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(stmt, 1, agent_name, -1, SQLITE_STATIC);
    int cap = 8;
    MemoryBlock *list = calloc((size_t)cap, sizeof(MemoryBlock));
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (*count >= cap) { cap *= 2; list = realloc(list, (size_t)cap * sizeof(MemoryBlock)); }
        MemoryBlock *mb = &list[*count];
        mb->id = sqlite3_column_int64(stmt, 0);
        const char *s;
        s = (const char *)sqlite3_column_text(stmt, 1); mb->agent_name = s ? strdup(s) : strdup("");
        s = (const char *)sqlite3_column_text(stmt, 2); mb->label = s ? strdup(s) : strdup("");
        s = (const char *)sqlite3_column_text(stmt, 3); mb->value = s ? strdup(s) : strdup("");
        s = (const char *)sqlite3_column_text(stmt, 4); mb->description = s ? strdup(s) : strdup("");
        mb->char_limit = sqlite3_column_int(stmt, 5);
        mb->read_only = sqlite3_column_int(stmt, 6);
        mb->created_at = sqlite3_column_int64(stmt, 7);
        mb->updated_at = sqlite3_column_int64(stmt, 8);
        (*count)++;
    }
    sqlite3_finalize(stmt);
    if (*count == 0) { free(list); return NULL; }
    return list;
}

int memory_block_set_value(sqlite3 *db, const char *agent_name, const char *label, const char *value) {
    /* V55: check read_only + char_limit */
    MemoryBlock *mb = memory_block_get(db, agent_name, label);
    if (!mb) return -1;
    if (mb->read_only) { memory_block_free(mb); return -1; }
    if (value && (int)strlen(value) > mb->char_limit) { memory_block_free(mb); return -1; }
    memory_block_free(mb);

    const char *sql = "UPDATE memory_blocks SET value=?, updated_at=unixepoch()"
                      " WHERE agent_name=? AND label=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, value ? value : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, agent_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, label, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE && sqlite3_changes(db) > 0) ? 0 : -1;
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
        if (*count >= cap) { cap *= 2; list = realloc(list, (size_t)cap * sizeof(MemoryEntry)); }
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

char *session_get_last_route(sqlite3 *db, int64_t session_id) {
    return db_scalar_text(db, "SELECT last_route FROM sessions WHERE id=?;", session_id);
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
