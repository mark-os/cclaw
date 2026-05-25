#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "cJSON.h"
#include "templates.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *SCHEMA_SQL = TPL_SCHEMA_SQL;

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
    sqlite3_exec(db, "PRAGMA foreign_keys=OFF;", NULL, NULL, &err);
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

int64_t session_create(sqlite3 *db, const char *name, const char *agent_name,
                       int64_t parent_session_id, int depth) {
    const char *sql = "INSERT INTO sessions (name, agent_name, parent_session_id, depth) VALUES (?, ?, ?, ?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    if (agent_name)
        sqlite3_bind_text(stmt, 2, agent_name, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 2);
    if (parent_session_id > 0)
        sqlite3_bind_int64(stmt, 3, parent_session_id);
    else
        sqlite3_bind_int64(stmt, 3, -1);
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
            if (!tmp) { break; }
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

/* V35: StopReason → string for JSON storage */
static const char *stop_reason_to_str(StopReason sr) {
    switch (sr) {
        case STOP_REASON_STOP:     return "stop";
        case STOP_REASON_LENGTH:   return "length";
        case STOP_REASON_TOOL_USE: return "tool_use";
        case STOP_REASON_ERROR:    return "error";
        case STOP_REASON_ABORTED:  return "aborted";
        default:                   return NULL;
    }
}

/* V35: string → StopReason from JSON storage */
static StopReason str_to_stop_reason(const char *s) {
    if (!s) return STOP_REASON_NONE;
    if (strcmp(s, "stop") == 0)     return STOP_REASON_STOP;
    if (strcmp(s, "length") == 0)   return STOP_REASON_LENGTH;
    if (strcmp(s, "tool_use") == 0) return STOP_REASON_TOOL_USE;
    if (strcmp(s, "error") == 0)    return STOP_REASON_ERROR;
    if (strcmp(s, "aborted") == 0)  return STOP_REASON_ABORTED;
    return STOP_REASON_NONE;
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

    /* V35: store stop_reason for assistant messages */
    if (msg->role == ROLE_ASSISTANT && msg->stop_reason != STOP_REASON_NONE) {
        const char *sr = stop_reason_to_str(msg->stop_reason);
        if (sr) cJSON_AddStringToObject(obj, "stop_reason", sr);
    }

    /* Store metadata (reasoning, usage, logprobs) if present */
    if (msg->metadata_json) {
        cJSON *meta = cJSON_Parse(msg->metadata_json);
        if (meta) cJSON_AddItemToObject(obj, "metadata", meta);
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

    /* V35: read stop_reason for assistant messages */
    if (msg->role == ROLE_ASSISTANT) {
        cJSON *sr = cJSON_GetObjectItem(obj, "stop_reason");
        if (sr && sr->valuestring)
            msg->stop_reason = str_to_stop_reason(sr->valuestring);
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

char *session_get_agent_name(sqlite3 *db, int64_t session_id) {
    const char *sql = "SELECT agent_name FROM sessions WHERE id=?;";
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

int session_get_depth(sqlite3 *db, int64_t session_id) {
    const char *sql = "SELECT depth FROM sessions WHERE id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(stmt, 1, session_id);
    int depth = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        depth = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return depth;
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
    for (int i = 0; i < count; i++) {
        free(sessions[i].name);
        free(sessions[i].agent_name);
    }
    free(sessions);
}

/* V14: insert entry with given parent_id, update session leaf */
int64_t entry_append_at(sqlite3 *db, int64_t session_id, int64_t parent_id, const Message *msg) {
    const char *sql =
        "INSERT INTO entries (parent_id, session_id, data, token_estimate, content_bytes)"
        " VALUES (?,?,?,?,?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, parent_id);
    sqlite3_bind_int64(stmt, 2, session_id);

    char *data = serialize_entry_data(msg);
    if (!data) { sqlite3_finalize(stmt); return -1; }
    int data_len = (int)strlen(data);
    sqlite3_bind_text(stmt, 3, data, data_len, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, (data_len / 4) + 4);
    sqlite3_bind_int(stmt, 5, data_len);
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

/* V3: sub-agent limits — count active child sessions */

int session_count_children(sqlite3 *db, int64_t parent_session_id) {
    const char *sql =
        "SELECT COUNT(*) FROM sessions WHERE parent_session_id=? AND state IN ('running','waiting');";
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

int session_count_active_agents(sqlite3 *db) {
    const char *sql =
        "SELECT COUNT(*) FROM sessions WHERE parent_session_id > 0 AND state IN ('running','waiting');";
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
    const char *sql = "SELECT COALESCE(MAX(turn_id), 0) + 1 FROM entries WHERE session_id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 1;
    sqlite3_bind_int64(stmt, 1, session_id);
    int64_t tid = 1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        tid = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return tid;
}

/* V17: append entry with explicit turn_id */
int64_t entry_append_with_turn(sqlite3 *db, int64_t session_id, const Message *msg, int64_t turn_id) {
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

    const char *ins_sql =
        "INSERT INTO entries (parent_id, session_id, turn_id, data, token_estimate, content_bytes)"
        " VALUES (?,?,?,?,?,?);";
    if (sqlite3_prepare_v2(db, ins_sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, parent_id);
    sqlite3_bind_int64(stmt, 2, session_id);
    sqlite3_bind_int64(stmt, 3, turn_id);

    char *data = serialize_entry_data(msg);
    if (!data) { sqlite3_finalize(stmt); return -1; }
    int data_len = (int)strlen(data);
    sqlite3_bind_text(stmt, 4, data, data_len, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, (data_len / 4) + 4);
    sqlite3_bind_int(stmt, 6, data_len);
    free(data);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;

    int64_t entry_id = sqlite3_last_insert_rowid(db);
    if (session_set_leaf(db, session_id, entry_id) != 0)
        return -1;
    return entry_id;
}

/* State transition with concurrency guard. Only valid transitions succeed:
 * idle → running, running → idle|waiting|error, waiting → idle, error → idle */
int session_set_state(sqlite3 *db, int64_t session_id, const char *state) {
    const char *sql =
        "UPDATE sessions SET state=?, updated_at=unixepoch()"
        " WHERE id=? AND ("
        "  (? = 'running' AND state = 'idle') OR"
        "  (? IN ('idle','waiting','error') AND state = 'running') OR"
        "  (? = 'idle' AND state IN ('waiting','error'))"
        ");";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, state, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, session_id);
    sqlite3_bind_text(stmt, 3, state, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, state, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, state, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE && sqlite3_changes(db) == 1) ? 0 : -1;
}

/* V18: Inbox primitives */

int64_t inbox_insert(sqlite3 *db, int64_t session_id, const char *source, const char *payload) {
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO inbox (session_id, source, payload) VALUES (?, ?, ?)", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_text(stmt, 2, source, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, payload, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    return sqlite3_last_insert_rowid(db);
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
        it->source = strdup((const char *)sqlite3_column_text(stmt, 2));
        it->payload = strdup((const char *)sqlite3_column_text(stmt, 3));
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
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM inbox WHERE session_id = ? AND consumed = 0",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, session_id);
    int count = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
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

        /* Build user message JSON: {"type":"message","role":"user","content":"<payload>"} */
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "type", "message");
        cJSON_AddStringToObject(obj, "role", "user");
        cJSON_AddStringToObject(obj, "content", payload ? payload : "");
        char *data = cJSON_PrintUnformatted(obj);
        cJSON_Delete(obj);
        if (!data) {
            sqlite3_finalize(sel);
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            return -1;
        }

        /* Insert entry */
        sqlite3_stmt *ins;
        if (sqlite3_prepare_v2(db,
            "INSERT INTO entries (parent_id, session_id, data, token_estimate, content_bytes)"
            " VALUES (?,?,?,?,?)",
            -1, &ins, NULL) != SQLITE_OK) {
            free(data);
            sqlite3_finalize(sel);
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            return -1;
        }
        int data_len = (int)strlen(data);
        sqlite3_bind_int64(ins, 1, parent_id);
        sqlite3_bind_int64(ins, 2, session_id);
        sqlite3_bind_text(ins, 3, data, data_len, SQLITE_TRANSIENT);
        sqlite3_bind_int(ins, 4, (data_len / 4) + 4);
        sqlite3_bind_int(ins, 5, data_len);
        free(data);
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

/* T88: Spawn queue — agent processes post requests, daemon picks up + forks */

int64_t spawn_queue_insert(sqlite3 *db, int64_t parent_session_id, const char *task,
                           int background, int depth, const char *tool_call_id) {
    const char *sql =
        "INSERT INTO spawn_queue (parent_session_id, task, background, depth, tool_call_id)"
        " VALUES (?,?,?,?,?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, parent_session_id);
    sqlite3_bind_text(stmt, 2, task, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, background);
    sqlite3_bind_int(stmt, 4, depth);
    if (tool_call_id)
        sqlite3_bind_text(stmt, 5, tool_call_id, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 5);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    return sqlite3_last_insert_rowid(db);
}

SpawnRequest *spawn_queue_peek_pending(sqlite3 *db, int *count) {
    *count = 0;
    const char *sql =
        "SELECT id, parent_session_id, task, background, depth, tool_call_id"
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
            if (!tmp) break;
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
        (*count)++;
    }
    sqlite3_finalize(stmt);
    if (*count == 0) { free(list); return NULL; }
    return list;
}

int spawn_queue_mark(sqlite3 *db, int64_t id, const char *status, int64_t child_session_id) {
    const char *sql =
        "UPDATE spawn_queue SET status=?, child_session_id=? WHERE id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, status, -1, SQLITE_STATIC);
    if (child_session_id > 0)
        sqlite3_bind_int64(stmt, 2, child_session_id);
    else
        sqlite3_bind_null(stmt, 2);
    sqlite3_bind_int64(stmt, 3, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

void spawn_request_free(SpawnRequest *list, int count) {
    for (int i = 0; i < count; i++) {
        free(list[i].task);
        free(list[i].tool_call_id);
    }
    free(list);
}

/* T119: agents table operations */

AgentRow *db_agent_get(sqlite3 *db, const char *name) {
    if (!db || !name) return NULL;
    const char *sql = "SELECT id, name, config, system_prompt, soul, memory, heartbeat, "
                      "created_at, updated_at FROM agents WHERE name = ?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);

    AgentRow *row = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        row = calloc(1, sizeof(AgentRow));
        if (row) {
            row->id = sqlite3_column_int64(stmt, 0);
            const char *v = (const char *)sqlite3_column_text(stmt, 1);
            row->name = v ? strdup(v) : NULL;
            v = (const char *)sqlite3_column_text(stmt, 2);
            row->config = v ? strdup(v) : NULL;
            v = (const char *)sqlite3_column_text(stmt, 3);
            row->system_prompt = v ? strdup(v) : NULL;
            v = (const char *)sqlite3_column_text(stmt, 4);
            row->soul = v ? strdup(v) : NULL;
            v = (const char *)sqlite3_column_text(stmt, 5);
            row->memory = v ? strdup(v) : NULL;
            v = (const char *)sqlite3_column_text(stmt, 6);
            row->heartbeat = v ? strdup(v) : NULL;
            row->created_at = sqlite3_column_int64(stmt, 7);
            row->updated_at = sqlite3_column_int64(stmt, 8);
        }
    }
    sqlite3_finalize(stmt);
    return row;
}

int db_agent_upsert(sqlite3 *db, const char *name, const char *config,
                    const char *system_prompt, const char *soul,
                    const char *memory, const char *heartbeat) {
    if (!db || !name) return -1;
    const char *sql =
        "INSERT INTO agents (name, config, system_prompt, soul, memory, heartbeat) "
        "VALUES (?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(name) DO UPDATE SET "
        "config=excluded.config, system_prompt=excluded.system_prompt, "
        "soul=excluded.soul, memory=excluded.memory, heartbeat=excluded.heartbeat, "
        "updated_at=unixepoch()";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, config, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, system_prompt, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, soul, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, memory, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, heartbeat, -1, SQLITE_STATIC);
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

AgentRow *db_agent_seed(sqlite3 *db, const char *agents_dir, const char *name) {
    if (!db || !name) return NULL;

    /* Check DB first — authoritative after seed */
    AgentRow *existing = db_agent_get(db, name);
    if (existing) return existing;

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

    db_agent_upsert(db, name, config_json, sys_prompt, NULL, NULL, NULL);
    free(config_json);
    free(sys_prompt);

    return db_agent_get(db, name);
}

/* T120: Update agent soul text */
int db_agent_set_soul(sqlite3 *db, const char *name, const char *soul) {
    if (!db || !name) return -1;
    const char *sql = "UPDATE agents SET soul = ?, updated_at = unixepoch() WHERE name = ?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, soul, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    return sqlite3_changes(db) > 0 ? 0 : -1;
}

/* T121: Update agent memory text */
int db_agent_set_memory(sqlite3 *db, const char *name, const char *memory) {
    if (!db || !name) return -1;
    const char *sql = "UPDATE agents SET memory = ?, updated_at = unixepoch() WHERE name = ?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, memory, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    return sqlite3_changes(db) > 0 ? 0 : -1;
}

void agent_row_free(AgentRow *row) {
    if (!row) return;
    free(row->name);
    free(row->config);
    free(row->system_prompt);
    free(row->soul);
    free(row->memory);
    free(row->heartbeat);
    free(row);
}

/* T146: approvals CRUD (V54) */

int64_t approval_insert(sqlite3 *db, int64_t session_id, const char *agent_name,
                        const char *type, const char *payload) {
    const char *sql = "INSERT INTO approvals (session_id, agent_name, type, payload) VALUES (?, ?, ?, ?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_text(stmt, 2, agent_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, type, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, payload, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); return -1; }
    int64_t id = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);
    return id;
}

static Approval *approval_from_stmt(sqlite3_stmt *stmt) {
    Approval *a = calloc(1, sizeof(Approval));
    if (!a) return NULL;
    a->id = sqlite3_column_int64(stmt, 0);
    a->session_id = sqlite3_column_int64(stmt, 1);
    const char *s = (const char *)sqlite3_column_text(stmt, 2);
    a->agent_name = s ? strdup(s) : NULL;
    s = (const char *)sqlite3_column_text(stmt, 3);
    a->type = s ? strdup(s) : NULL;
    s = (const char *)sqlite3_column_text(stmt, 4);
    a->payload = s ? strdup(s) : NULL;
    s = (const char *)sqlite3_column_text(stmt, 5);
    a->status = s ? strdup(s) : NULL;
    a->admin_chat_id = sqlite3_column_int64(stmt, 6);
    a->created_at = sqlite3_column_int64(stmt, 7);
    a->resolved_at = sqlite3_column_int64(stmt, 8);
    return a;
}

Approval *approval_list_pending(sqlite3 *db, int *count) {
    *count = 0;
    const char *sql = "SELECT id, session_id, agent_name, type, payload, status, admin_chat_id, created_at, resolved_at "
                      "FROM approvals WHERE status='pending' ORDER BY id;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    int cap = 4;
    Approval *list = malloc((size_t)cap * sizeof(Approval));
    if (!list) { sqlite3_finalize(stmt); return NULL; }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (*count >= cap) {
            cap *= 2;
            Approval *tmp = realloc(list, (size_t)cap * sizeof(Approval));
            if (!tmp) break;
            list = tmp;
        }
        Approval *a = &list[*count];
        a->id = sqlite3_column_int64(stmt, 0);
        a->session_id = sqlite3_column_int64(stmt, 1);
        const char *s = (const char *)sqlite3_column_text(stmt, 2);
        a->agent_name = s ? strdup(s) : NULL;
        s = (const char *)sqlite3_column_text(stmt, 3);
        a->type = s ? strdup(s) : NULL;
        s = (const char *)sqlite3_column_text(stmt, 4);
        a->payload = s ? strdup(s) : NULL;
        s = (const char *)sqlite3_column_text(stmt, 5);
        a->status = s ? strdup(s) : NULL;
        a->admin_chat_id = sqlite3_column_int64(stmt, 6);
        a->created_at = sqlite3_column_int64(stmt, 7);
        a->resolved_at = sqlite3_column_int64(stmt, 8);
        (*count)++;
    }
    sqlite3_finalize(stmt);
    if (*count == 0) { free(list); return NULL; }
    return list;
}

Approval *approval_get(sqlite3 *db, int64_t id) {
    const char *sql = "SELECT id, session_id, agent_name, type, payload, status, admin_chat_id, created_at, resolved_at "
                      "FROM approvals WHERE id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int64(stmt, 1, id);
    Approval *a = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) a = approval_from_stmt(stmt);
    sqlite3_finalize(stmt);
    return a;
}

int approval_resolve(sqlite3 *db, int64_t id, const char *status, int64_t admin_chat_id) {
    const char *sql = "UPDATE approvals SET status=?, admin_chat_id=?, resolved_at=unixepoch() WHERE id=? AND status='pending';";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, status, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, admin_chat_id);
    sqlite3_bind_int64(stmt, 3, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    return sqlite3_changes(db) > 0 ? 0 : -1;
}

void approval_free(Approval *a) {
    if (!a) return;
    free(a->agent_name);
    free(a->type);
    free(a->payload);
    free(a->status);
    free(a);
}

void approval_list_free(Approval *list, int count) {
    if (!list) return;
    for (int i = 0; i < count; i++) {
        free(list[i].agent_name);
        free(list[i].type);
        free(list[i].payload);
        free(list[i].status);
    }
    free(list);
}

/* T148: get pending approvals not yet notified to admins */
Approval *approval_list_unnotified(sqlite3 *db, int *count) {
    *count = 0;
    const char *sql = "SELECT id, session_id, agent_name, type, payload, status, admin_chat_id, created_at, resolved_at "
                      "FROM approvals WHERE status='pending' AND notified=0 ORDER BY id;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    int cap = 4;
    Approval *list = malloc((size_t)cap * sizeof(Approval));
    if (!list) { sqlite3_finalize(stmt); return NULL; }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (*count >= cap) {
            cap *= 2;
            Approval *tmp = realloc(list, (size_t)cap * sizeof(Approval));
            if (!tmp) break;
            list = tmp;
        }
        Approval *a = &list[*count];
        a->id = sqlite3_column_int64(stmt, 0);
        a->session_id = sqlite3_column_int64(stmt, 1);
        const char *s = (const char *)sqlite3_column_text(stmt, 2);
        a->agent_name = s ? strdup(s) : NULL;
        s = (const char *)sqlite3_column_text(stmt, 3);
        a->type = s ? strdup(s) : NULL;
        s = (const char *)sqlite3_column_text(stmt, 4);
        a->payload = s ? strdup(s) : NULL;
        s = (const char *)sqlite3_column_text(stmt, 5);
        a->status = s ? strdup(s) : NULL;
        a->admin_chat_id = sqlite3_column_int64(stmt, 6);
        a->created_at = sqlite3_column_int64(stmt, 7);
        a->resolved_at = sqlite3_column_int64(stmt, 8);
        (*count)++;
    }
    sqlite3_finalize(stmt);
    if (*count == 0) { free(list); return NULL; }
    return list;
}

int approval_mark_notified(sqlite3 *db, int64_t id) {
    const char *sql = "UPDATE approvals SET notified=1 WHERE id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}
