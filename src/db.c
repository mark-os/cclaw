#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS sessions ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  name TEXT,"
    "  leaf_id INTEGER DEFAULT -1,"
    "  created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
    "  updated_at INTEGER NOT NULL DEFAULT (unixepoch())"
    ");"
    "CREATE TABLE IF NOT EXISTS entries ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  parent_id INTEGER NOT NULL DEFAULT -1,"
    "  session_id INTEGER NOT NULL REFERENCES sessions(id),"
    "  created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
    "  role INTEGER NOT NULL,"
    "  content TEXT,"
    "  tool_calls_json TEXT,"
    "  tool_result_json TEXT"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_entries_session ON entries(session_id);"
    "CREATE INDEX IF NOT EXISTS idx_entries_parent ON entries(parent_id);"
    /* V7: FTS5 index over message content */
    "CREATE VIRTUAL TABLE IF NOT EXISTS entries_fts USING fts5("
    "  content, content=entries, content_rowid=id"
    ");"
    "CREATE TRIGGER IF NOT EXISTS entries_ai AFTER INSERT ON entries BEGIN"
    "  INSERT INTO entries_fts(rowid, content) VALUES (new.id, new.content);"
    "END;"
    "CREATE TRIGGER IF NOT EXISTS entries_ad AFTER DELETE ON entries BEGIN"
    "  INSERT INTO entries_fts(entries_fts, rowid, content) VALUES('delete', old.id, old.content);"
    "END;"
    "CREATE TRIGGER IF NOT EXISTS entries_au AFTER UPDATE ON entries BEGIN"
    "  INSERT INTO entries_fts(entries_fts, rowid, content) VALUES('delete', old.id, old.content);"
    "  INSERT INTO entries_fts(rowid, content) VALUES (new.id, new.content);"
    "END;"
    "CREATE TABLE IF NOT EXISTS kv ("
    "  key TEXT PRIMARY KEY,"
    "  value TEXT NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS tg_chat_sessions ("
    "  chat_id INTEGER PRIMARY KEY,"
    "  session_id INTEGER NOT NULL REFERENCES sessions(id)"
    ");"
    "CREATE TABLE IF NOT EXISTS js_tools ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  session_id INTEGER NOT NULL REFERENCES sessions(id),"
    "  name TEXT NOT NULL,"
    "  description TEXT,"
    "  parameters_json TEXT,"
    "  code TEXT NOT NULL,"
    "  UNIQUE(session_id, name)"
    ");"
    "CREATE TABLE IF NOT EXISTS cron_jobs ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  name TEXT NOT NULL,"
    "  cron_expr TEXT NOT NULL,"
    "  session_id INTEGER NOT NULL REFERENCES sessions(id),"
    "  task TEXT NOT NULL,"
    "  enabled INTEGER NOT NULL DEFAULT 1,"
    "  next_run_at INTEGER NOT NULL DEFAULT 0,"
    "  last_run_at INTEGER,"
    "  created_at INTEGER NOT NULL DEFAULT (unixepoch())"
    ");";

sqlite3 *db_open(const char *path) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_open: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }

    /* V4: WAL mode */
    char *err = NULL;
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, &err);
    if (err) { sqlite3_free(err); err = NULL; }

    /* V4: busy_timeout >= 5000ms */
    sqlite3_exec(db, "PRAGMA busy_timeout=5000;", NULL, NULL, &err);
    if (err) { sqlite3_free(err); err = NULL; }

    /* Foreign keys */
    sqlite3_exec(db, "PRAGMA foreign_keys=ON;", NULL, NULL, &err);
    if (err) { sqlite3_free(err); err = NULL; }

    /* Create tables */
    rc = sqlite3_exec(db, SCHEMA_SQL, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_open schema: %s\n", err);
        sqlite3_free(err);
        sqlite3_close(db);
        return NULL;
    }

    return db;
}

void db_close(sqlite3 *db) {
    if (db) sqlite3_close(db);
}

int64_t session_create(sqlite3 *db, const char *name) {
    const char *sql = "INSERT INTO sessions (name) VALUES (?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
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
    const char *sql = "SELECT id, name, leaf_id, created_at, updated_at FROM sessions ORDER BY id;";
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
            if (!tmp) { break; }
            list = tmp;
        }
        Session *s = &list[*count];
        s->id = sqlite3_column_int64(stmt, 0);
        const char *n = (const char *)sqlite3_column_text(stmt, 1);
        s->name = n ? strdup(n) : NULL;
        s->leaf_id = sqlite3_column_int64(stmt, 2);
        s->created_at = (time_t)sqlite3_column_int64(stmt, 3);
        s->updated_at = (time_t)sqlite3_column_int64(stmt, 4);
        (*count)++;
    }
    sqlite3_finalize(stmt);
    if (*count == 0) { free(list); return NULL; }
    return list;
}

/* Serialize tool_calls array to JSON string. Caller must free. Returns NULL if none. */
static char *serialize_tool_calls(const ToolCall *calls, size_t count) {
    if (!calls || count == 0) return NULL;
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "id", calls[i].id ? calls[i].id : "");
        cJSON_AddStringToObject(obj, "name", calls[i].name ? calls[i].name : "");
        cJSON_AddStringToObject(obj, "arguments", calls[i].arguments ? calls[i].arguments : "");
        cJSON_AddItemToArray(arr, obj);
    }
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}

/* Serialize tool_result to JSON string. Caller must free. Returns NULL if none. */
static char *serialize_tool_result(const ToolResult *tr) {
    if (!tr) return NULL;
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "tool_call_id", tr->tool_call_id ? tr->tool_call_id : "");
    cJSON_AddStringToObject(obj, "content", tr->content ? tr->content : "");
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return json;
}

/* Deserialize tool_calls JSON into heap-allocated array. Sets *count. */
static ToolCall *deserialize_tool_calls(const char *json, size_t *count) {
    *count = 0;
    if (!json) return NULL;
    cJSON *arr = cJSON_Parse(json);
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(arr); return NULL; }
    int n = cJSON_GetArraySize(arr);
    if (n <= 0) { cJSON_Delete(arr); return NULL; }
    ToolCall *calls = malloc((size_t)n * sizeof(ToolCall));
    if (!calls) { cJSON_Delete(arr); return NULL; }
    for (int i = 0; i < n; i++) {
        cJSON *obj = cJSON_GetArrayItem(arr, i);
        cJSON *id = cJSON_GetObjectItem(obj, "id");
        cJSON *name = cJSON_GetObjectItem(obj, "name");
        cJSON *args = cJSON_GetObjectItem(obj, "arguments");
        calls[i].id = (id && id->valuestring) ? strdup(id->valuestring) : NULL;
        calls[i].name = (name && name->valuestring) ? strdup(name->valuestring) : NULL;
        calls[i].arguments = (args && args->valuestring) ? strdup(args->valuestring) : NULL;
    }
    *count = (size_t)n;
    cJSON_Delete(arr);
    return calls;
}

/* Deserialize tool_result JSON into heap-allocated struct. */
static ToolResult *deserialize_tool_result(const char *json) {
    if (!json) return NULL;
    cJSON *obj = cJSON_Parse(json);
    if (!obj || !cJSON_IsObject(obj)) { cJSON_Delete(obj); return NULL; }
    ToolResult *tr = malloc(sizeof(ToolResult));
    if (!tr) { cJSON_Delete(obj); return NULL; }
    cJSON *tcid = cJSON_GetObjectItem(obj, "tool_call_id");
    cJSON *content = cJSON_GetObjectItem(obj, "content");
    tr->tool_call_id = (tcid && tcid->valuestring) ? strdup(tcid->valuestring) : NULL;
    tr->content = (content && content->valuestring) ? strdup(content->valuestring) : NULL;
    cJSON_Delete(obj);
    return tr;
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

    /* Walk chain using recursive CTE */
    const char *branch_sql =
        "WITH RECURSIVE branch(id, parent_id, session_id, created_at, role, content, tool_calls_json, tool_result_json) AS ("
        "  SELECT id, parent_id, session_id, created_at, role, content, tool_calls_json, tool_result_json"
        "    FROM entries WHERE id=? AND session_id=?"
        "  UNION ALL"
        "  SELECT e.id, e.parent_id, e.session_id, e.created_at, e.role, e.content, e.tool_calls_json, e.tool_result_json"
        "    FROM entries e JOIN branch b ON e.id=b.parent_id"
        ") SELECT id, parent_id, session_id, created_at, role, content, tool_calls_json, tool_result_json FROM branch ORDER BY id;";

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
        e->session_id = sqlite3_column_int64(stmt, 2);
        e->created_at = (time_t)sqlite3_column_int64(stmt, 3);
        e->message.role = (Role)sqlite3_column_int(stmt, 4);
        const char *c = (const char *)sqlite3_column_text(stmt, 5);
        e->message.content = c ? strdup(c) : NULL;
        const char *tc = (const char *)sqlite3_column_text(stmt, 6);
        e->message.tool_calls = deserialize_tool_calls(tc, &e->message.tool_call_count);
        const char *tr = (const char *)sqlite3_column_text(stmt, 7);
        e->message.tool_result = deserialize_tool_result(tr);
        (*count)++;
    }
    sqlite3_finalize(stmt);
    if (*count == 0) { free(entries); return NULL; }
    return entries;
}

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

void session_list_free(Session *sessions, int count) {
    if (!sessions) return;
    for (int i = 0; i < count; i++)
        free(sessions[i].name);
    free(sessions);
}

/* V14: insert entry with given parent_id, update session leaf */
int64_t entry_append_at(sqlite3 *db, int64_t session_id, int64_t parent_id, const Message *msg) {
    const char *sql =
        "INSERT INTO entries (parent_id, session_id, role, content, tool_calls_json, tool_result_json)"
        " VALUES (?,?,?,?,?,?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, parent_id);
    sqlite3_bind_int64(stmt, 2, session_id);
    sqlite3_bind_int(stmt, 3, (int)msg->role);
    if (msg->content)
        sqlite3_bind_text(stmt, 4, msg->content, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 4);

    /* Serialize tool_calls and tool_result */
    char *tc_json = serialize_tool_calls(msg->tool_calls, msg->tool_call_count);
    char *tr_json = serialize_tool_result(msg->tool_result);

    if (tc_json)
        sqlite3_bind_text(stmt, 5, tc_json, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, 5);
    if (tr_json)
        sqlite3_bind_text(stmt, 6, tr_json, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, 6);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    free(tc_json);
    free(tr_json);

    if (rc != SQLITE_DONE)
        return -1;

    int64_t entry_id = sqlite3_last_insert_rowid(db);

    if (session_set_leaf(db, session_id, entry_id) != 0)
        return -1;

    return entry_id;
}

/* V14: append as child of current leaf (linear continuation) */
int64_t entry_append(sqlite3 *db, int64_t session_id, const Message *msg) {
    /* Get current leaf_id */
    const char *sql = "SELECT leaf_id FROM sessions WHERE id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, session_id);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return -1;
    }
    int64_t parent_id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);

    /* leaf_id == -1 means no entries yet → parent_id stays -1 (root) */
    return entry_append_at(db, session_id, parent_id, msg);
}

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

/* V7: FTS5 search over message content */
Entry *entry_search(sqlite3 *db, const char *query, int64_t session_id, int *count) {
    *count = 0;
    const char *sql =
        "SELECT e.id, e.parent_id, e.session_id, e.created_at, e.role, e.content"
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
            if (!tmp) break;
            entries = tmp;
        }
        Entry *e = &entries[*count];
        e->id = sqlite3_column_int64(stmt, 0);
        e->parent_id = sqlite3_column_int64(stmt, 1);
        e->session_id = sqlite3_column_int64(stmt, 2);
        e->created_at = (time_t)sqlite3_column_int64(stmt, 3);
        e->message.role = (Role)sqlite3_column_int(stmt, 4);
        const char *c = (const char *)sqlite3_column_text(stmt, 5);
        e->message.content = c ? strdup(c) : NULL;
        e->message.tool_calls = NULL;
        e->message.tool_call_count = 0;
        e->message.tool_result = NULL;
        (*count)++;
    }
    sqlite3_finalize(stmt);
    if (*count == 0) { free(entries); return NULL; }
    return entries;
}

/* Key-value store for persistent settings (e.g. Telegram offset) */
char *db_kv_get(sqlite3 *db, const char *key) {
    const char *sql = "SELECT value FROM kv WHERE key=?;";
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
    const char *sql = "INSERT OR REPLACE INTO kv (key, value) VALUES (?, ?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, value, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int64_t db_tg_get_session(sqlite3 *db, int64_t chat_id) {
    const char *sql = "SELECT session_id FROM tg_chat_sessions WHERE chat_id = ?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, chat_id);
    int64_t session_id = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        session_id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return session_id;
}

int db_tg_set_session(sqlite3 *db, int64_t chat_id, int64_t session_id) {
    const char *sql = "INSERT OR REPLACE INTO tg_chat_sessions (chat_id, session_id) VALUES (?, ?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, chat_id);
    sqlite3_bind_int64(stmt, 2, session_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}
