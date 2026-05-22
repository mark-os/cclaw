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
    "  state TEXT NOT NULL DEFAULT 'idle',"
    "  lock_holder TEXT,"
    "  lock_acquired_at INTEGER,"
    "  error_count INTEGER NOT NULL DEFAULT 0,"
    "  created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
    "  updated_at INTEGER NOT NULL DEFAULT (unixepoch())"
    ");"
    "CREATE TABLE IF NOT EXISTS entries ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  session_id INTEGER NOT NULL REFERENCES sessions(id),"
    "  parent_id INTEGER NOT NULL DEFAULT -1,"
    "  turn_id INTEGER,"
    "  created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
    "  data TEXT NOT NULL,"
    /* Generated columns — relational indexes without JSON parsing at query time */
    "  type TEXT GENERATED ALWAYS AS (json_extract(data, '$.type')) STORED,"
    "  role TEXT GENERATED ALWAYS AS (json_extract(data, '$.role')) STORED"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_entries_session ON entries(session_id, id);"
    "CREATE INDEX IF NOT EXISTS idx_entries_parent ON entries(parent_id);"
    "CREATE INDEX IF NOT EXISTS idx_entries_session_type ON entries(session_id, type);"
    "CREATE INDEX IF NOT EXISTS idx_entries_session_role ON entries(session_id, role);"
    "CREATE INDEX IF NOT EXISTS idx_entries_turn ON entries(session_id, turn_id);"
    /* V7: FTS5 index over extracted text content */
    "CREATE VIRTUAL TABLE IF NOT EXISTS entries_fts USING fts5("
    "  content, content=entries, content_rowid=id"
    ");"
    /* FTS5 triggers: extract searchable text from JSON data */
    "CREATE TRIGGER IF NOT EXISTS entries_ai AFTER INSERT ON entries BEGIN"
    "  INSERT INTO entries_fts(rowid, content) VALUES ("
    "    new.id,"
    "    CASE json_extract(new.data, '$.role')"
    "      WHEN 'user' THEN json_extract(new.data, '$.content')"
    "      WHEN 'system' THEN json_extract(new.data, '$.content')"
    "      WHEN 'tool_result' THEN COALESCE(json_extract(new.data, '$.name'),'') || ' ' || COALESCE(json_extract(new.data, '$.content'),'')"
    "      WHEN 'assistant' THEN ("
    "        SELECT group_concat(txt, ' ') FROM ("
    "          SELECT CASE json_extract(j.value, '$.type')"
    "            WHEN 'text' THEN json_extract(j.value, '$.text')"
    "            WHEN 'tool_call' THEN json_extract(j.value, '$.name') || ' ' || json_extract(j.value, '$.arguments')"
    "          END AS txt"
    "          FROM json_each(json_extract(new.data, '$.content')) j"
    "        )"
    "      )"
    "      ELSE COALESCE(json_extract(new.data, '$.summary'),'')"
    "    END"
    "  );"
    "END;"
    /* No delete/update triggers — entries are append-only */
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
    ");"
    "CREATE TABLE IF NOT EXISTS sub_agents ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  parent_session_id INTEGER NOT NULL REFERENCES sessions(id),"
    "  session_id INTEGER NOT NULL REFERENCES sessions(id),"
    "  pid INTEGER NOT NULL DEFAULT 0,"
    "  depth INTEGER NOT NULL DEFAULT 1,"
    "  status TEXT NOT NULL DEFAULT 'running',"
    "  task TEXT NOT NULL,"
    "  result TEXT,"
    "  created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
    "  finished_at INTEGER"
    ");"
    "CREATE TABLE IF NOT EXISTS inbox ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  session_id INTEGER NOT NULL REFERENCES sessions(id),"
    "  source TEXT NOT NULL,"
    "  payload TEXT NOT NULL,"
    "  created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
    "  consumed INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_inbox_pending ON inbox(session_id, consumed) WHERE consumed = 0;";

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
static const char *role_to_str(Role r) {
    switch (r) {
        case ROLE_SYSTEM: return "system";
        case ROLE_USER: return "user";
        case ROLE_ASSISTANT: return "assistant";
        case ROLE_TOOL: return "tool_result";
    }
    return "user";
}

static Role str_to_role(const char *s) {
    if (!s) return ROLE_USER;
    if (strcmp(s, "system") == 0) return ROLE_SYSTEM;
    if (strcmp(s, "assistant") == 0) return ROLE_ASSISTANT;
    if (strcmp(s, "tool_result") == 0) return ROLE_TOOL;
    return ROLE_USER;
}

/* Serialize Message to §D JSON data format. Caller must free. */
static char *serialize_entry_data(const Message *msg) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "type", "message");
    cJSON_AddStringToObject(obj, "role", role_to_str(msg->role));

    if (msg->role == ROLE_ASSISTANT) {
        /* §D: assistant content is always an array of content blocks */
        cJSON *content_arr = cJSON_CreateArray();
        if (msg->content) {
            cJSON *text_block = cJSON_CreateObject();
            cJSON_AddStringToObject(text_block, "type", "text");
            cJSON_AddStringToObject(text_block, "text", msg->content);
            cJSON_AddItemToArray(content_arr, text_block);
        }
        if (msg->tool_calls) {
            for (size_t i = 0; i < msg->tool_call_count; i++) {
                cJSON *tc = cJSON_CreateObject();
                cJSON_AddStringToObject(tc, "type", "tool_call");
                cJSON_AddStringToObject(tc, "id", msg->tool_calls[i].id ? msg->tool_calls[i].id : "");
                cJSON_AddStringToObject(tc, "name", msg->tool_calls[i].name ? msg->tool_calls[i].name : "");
                cJSON *args = cJSON_Parse(msg->tool_calls[i].arguments);
                if (args)
                    cJSON_AddItemToObject(tc, "arguments", args);
                else
                    cJSON_AddStringToObject(tc, "arguments", msg->tool_calls[i].arguments ? msg->tool_calls[i].arguments : "{}");
                cJSON_AddItemToArray(content_arr, tc);
            }
        }
        cJSON_AddItemToObject(obj, "content", content_arr);
    } else if (msg->role == ROLE_TOOL && msg->tool_result) {
        /* Tool result */
        cJSON_AddStringToObject(obj, "tool_call_id", msg->tool_result->tool_call_id ? msg->tool_result->tool_call_id : "");
        cJSON_AddStringToObject(obj, "content", msg->tool_result->content ? msg->tool_result->content : "");
    } else {
        /* User, system, or assistant without tool_calls */
        cJSON_AddStringToObject(obj, "content", msg->content ? msg->content : "");
    }

    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return json;
}

/* Parse §D JSON data into Message. Caller owns returned strings. */
static void deserialize_entry_data(const char *json, Message *msg) {
    memset(msg, 0, sizeof(*msg));
    if (!json) return;

    cJSON *obj = cJSON_Parse(json);
    if (!obj) return;

    cJSON *role = cJSON_GetObjectItem(obj, "role");
    msg->role = str_to_role(role ? role->valuestring : NULL);

    if (msg->role == ROLE_TOOL) {
        /* Tool result format */
        cJSON *tc_id = cJSON_GetObjectItem(obj, "tool_call_id");
        cJSON *content = cJSON_GetObjectItem(obj, "content");
        msg->tool_result = malloc(sizeof(ToolResult));
        msg->tool_result->tool_call_id = (tc_id && tc_id->valuestring) ? strdup(tc_id->valuestring) : NULL;
        msg->tool_result->content = (content && content->valuestring) ? strdup(content->valuestring) : NULL;
    } else if (msg->role == ROLE_ASSISTANT) {
        cJSON *content = cJSON_GetObjectItem(obj, "content");
        if (cJSON_IsArray(content)) {
            /* Array content: text blocks + tool_calls */
            int n = cJSON_GetArraySize(content);
            /* Count tool_calls */
            size_t tc_count = 0;
            for (int i = 0; i < n; i++) {
                cJSON *item = cJSON_GetArrayItem(content, i);
                cJSON *type = cJSON_GetObjectItem(item, "type");
                if (type && type->valuestring && strcmp(type->valuestring, "tool_call") == 0)
                    tc_count++;
            }
            if (tc_count > 0) {
                msg->tool_calls = malloc(tc_count * sizeof(ToolCall));
                msg->tool_call_count = tc_count;
            }
            size_t tc_idx = 0;
            for (int i = 0; i < n; i++) {
                cJSON *item = cJSON_GetArrayItem(content, i);
                cJSON *type = cJSON_GetObjectItem(item, "type");
                if (!type || !type->valuestring) continue;
                if (strcmp(type->valuestring, "text") == 0) {
                    cJSON *text = cJSON_GetObjectItem(item, "text");
                    if (text && text->valuestring)
                        msg->content = strdup(text->valuestring);
                } else if (strcmp(type->valuestring, "tool_call") == 0 && tc_idx < tc_count) {
                    cJSON *id = cJSON_GetObjectItem(item, "id");
                    cJSON *name = cJSON_GetObjectItem(item, "name");
                    cJSON *args = cJSON_GetObjectItem(item, "arguments");
                    msg->tool_calls[tc_idx].id = (id && id->valuestring) ? strdup(id->valuestring) : NULL;
                    msg->tool_calls[tc_idx].name = (name && name->valuestring) ? strdup(name->valuestring) : NULL;
                    if (args) {
                        char *args_str = cJSON_PrintUnformatted(args);
                        msg->tool_calls[tc_idx].arguments = args_str;
                    } else {
                        msg->tool_calls[tc_idx].arguments = NULL;
                    }
                    tc_idx++;
                }
            }
        } else if (cJSON_IsString(content)) {
            msg->content = strdup(content->valuestring);
        }
    } else {
        /* User or system: content is string */
        cJSON *content = cJSON_GetObjectItem(obj, "content");
        if (content && content->valuestring)
            msg->content = strdup(content->valuestring);
    }

    cJSON_Delete(obj);
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
        "WITH RECURSIVE branch(id, parent_id, session_id, created_at, data) AS ("
        "  SELECT id, parent_id, session_id, created_at, data"
        "    FROM entries WHERE id=? AND session_id=?"
        "  UNION ALL"
        "  SELECT e.id, e.parent_id, e.session_id, e.created_at, e.data"
        "    FROM entries e JOIN branch b ON e.id=b.parent_id"
        ") SELECT id, parent_id, session_id, created_at, data FROM branch ORDER BY id;";

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
        const char *data = (const char *)sqlite3_column_text(stmt, 4);
        deserialize_entry_data(data, &e->message);
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
        "INSERT INTO entries (parent_id, session_id, data)"
        " VALUES (?,?,?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, parent_id);
    sqlite3_bind_int64(stmt, 2, session_id);

    char *data = serialize_entry_data(msg);
    if (!data) { sqlite3_finalize(stmt); return -1; }
    sqlite3_bind_text(stmt, 3, data, -1, SQLITE_TRANSIENT);
    free(data);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

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
        "SELECT e.id, e.parent_id, e.session_id, e.created_at, e.data"
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
        const char *data = (const char *)sqlite3_column_text(stmt, 4);
        deserialize_entry_data(data, &e->message);
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

/* V3: sub-agent tracking */

int64_t subagent_create(sqlite3 *db, int64_t parent_session_id, int64_t session_id,
                        pid_t pid, int depth, const char *task) {
    const char *sql =
        "INSERT INTO sub_agents (parent_session_id, session_id, pid, depth, status, task)"
        " VALUES (?,?,?,?,'running',?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, parent_session_id);
    sqlite3_bind_int64(stmt, 2, session_id);
    sqlite3_bind_int64(stmt, 3, (int64_t)pid);
    sqlite3_bind_int(stmt, 4, depth);
    sqlite3_bind_text(stmt, 5, task, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    return sqlite3_last_insert_rowid(db);
}

int subagent_count_by_parent(sqlite3 *db, int64_t parent_session_id) {
    const char *sql =
        "SELECT COUNT(*) FROM sub_agents WHERE parent_session_id=? AND status='running';";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, parent_session_id);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

int subagent_count_total(sqlite3 *db) {
    const char *sql = "SELECT COUNT(*) FROM sub_agents WHERE status='running';";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

int subagent_finish(sqlite3 *db, int64_t agent_id, const char *status, const char *result) {
    const char *sql =
        "UPDATE sub_agents SET status=?, result=?, finished_at=unixepoch() WHERE id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, status, -1, SQLITE_STATIC);
    if (result)
        sqlite3_bind_text(stmt, 2, result, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 2);
    sqlite3_bind_int64(stmt, 3, agent_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

SubAgentInfo *subagent_get(sqlite3 *db, int64_t agent_id) {
    const char *sql =
        "SELECT id, parent_session_id, session_id, pid, depth, status, task, result"
        " FROM sub_agents WHERE id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(stmt, 1, agent_id);
    SubAgentInfo *info = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        info = malloc(sizeof(SubAgentInfo));
        if (info) {
            info->id = sqlite3_column_int64(stmt, 0);
            info->parent_session_id = sqlite3_column_int64(stmt, 1);
            info->session_id = sqlite3_column_int64(stmt, 2);
            info->pid = (pid_t)sqlite3_column_int64(stmt, 3);
            info->depth = sqlite3_column_int(stmt, 4);
            const char *s = (const char *)sqlite3_column_text(stmt, 5);
            info->status = s ? strdup(s) : strdup("unknown");
            const char *t = (const char *)sqlite3_column_text(stmt, 6);
            info->task = t ? strdup(t) : NULL;
            const char *r = (const char *)sqlite3_column_text(stmt, 7);
            info->result = r ? strdup(r) : NULL;
        }
    }
    sqlite3_finalize(stmt);
    return info;
}

void subagent_info_free(SubAgentInfo *info) {
    if (!info) return;
    free(info->status);
    free(info->task);
    free(info->result);
    free(info);
}

/* V16,V19: Atomic CAS acquire — idle→running with lock_holder */
int session_try_acquire(sqlite3 *db, int64_t session_id, const char *lock_holder) {
    const char *sql =
        "UPDATE sessions SET state='running', lock_holder=?, lock_acquired_at=unixepoch(), updated_at=unixepoch()"
        " WHERE id=? AND state='idle' AND lock_holder IS NULL;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, lock_holder, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, session_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE && sqlite3_changes(db) == 1) ? 0 : -1;
}

/* V16,V19: Atomic CAS release — running→idle, only if lock_holder matches */
int session_release(sqlite3 *db, int64_t session_id, const char *lock_holder) {
    const char *sql =
        "UPDATE sessions SET state='idle', lock_holder=NULL, lock_acquired_at=NULL, updated_at=unixepoch()"
        " WHERE id=? AND state='running' AND lock_holder=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_text(stmt, 2, lock_holder, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE && sqlite3_changes(db) == 1) ? 0 : -1;
}

SubAgentInfo *subagent_list_running(sqlite3 *db, int *count) {
    *count = 0;
    const char *sql =
        "SELECT id, parent_session_id, session_id, pid, depth, status, task, result"
        " FROM sub_agents WHERE status='running';";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;

    int cap = 16;
    SubAgentInfo *list = malloc((size_t)cap * sizeof(SubAgentInfo));
    if (!list) { sqlite3_finalize(stmt); return NULL; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (*count >= cap) {
            cap *= 2;
            SubAgentInfo *tmp = realloc(list, (size_t)cap * sizeof(SubAgentInfo));
            if (!tmp) break;
            list = tmp;
        }
        SubAgentInfo *info = &list[*count];
        info->id = sqlite3_column_int64(stmt, 0);
        info->parent_session_id = sqlite3_column_int64(stmt, 1);
        info->session_id = sqlite3_column_int64(stmt, 2);
        info->pid = (pid_t)sqlite3_column_int64(stmt, 3);
        info->depth = sqlite3_column_int(stmt, 4);
        const char *s = (const char *)sqlite3_column_text(stmt, 5);
        info->status = s ? strdup(s) : strdup("unknown");
        const char *t = (const char *)sqlite3_column_text(stmt, 6);
        info->task = t ? strdup(t) : NULL;
        info->result = NULL;
        (*count)++;
    }
    sqlite3_finalize(stmt);
    if (*count == 0) { free(list); return NULL; }
    return list;
}
