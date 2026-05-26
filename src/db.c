#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "cJSON.h"
#include "templates.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *SCHEMA_SQL = TPL_SCHEMA_SQL;

/* T156: detect old entries schema (has `data TEXT NOT NULL` without `role INTEGER`) */
static int entries_needs_migration(sqlite3 *db) {
    sqlite3_stmt *stmt;
    /* If 'role' column is INTEGER, we're on new schema */
    if (sqlite3_prepare_v2(db,
        "SELECT typeof(role) FROM entries LIMIT 0", -1, &stmt, NULL) != SQLITE_OK)
        return 0; /* table doesn't exist yet — fresh DB */
    sqlite3_finalize(stmt);

    /* Check if role column is text (generated from json) or integer (new schema) */
    if (sqlite3_prepare_v2(db,
        "SELECT sql FROM sqlite_master WHERE type='table' AND name='entries'",
        -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    int needs = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *sql = (const char *)sqlite3_column_text(stmt, 0);
        /* Old schema has "data TEXT NOT NULL" and generated role column */
        if (sql && strstr(sql, "data TEXT NOT NULL") && strstr(sql, "GENERATED ALWAYS"))
            needs = 1;
    }
    sqlite3_finalize(stmt);
    return needs;
}

/* T156: role string → integer mapping for migration */
static int role_str_to_int(const char *s) {
    if (!s) return 1;
    if (strcmp(s, "system") == 0) return 0;
    if (strcmp(s, "user") == 0) return 1;
    if (strcmp(s, "assistant") == 0) return 2;
    if (strcmp(s, "tool_result") == 0) return 3;
    return 1;
}

/* T156: stop_reason string → integer mapping for migration */
static int stop_reason_str_to_int(const char *s) {
    if (!s) return 0;
    if (strcmp(s, "stop") == 0) return 1;
    if (strcmp(s, "length") == 0) return 2;
    if (strcmp(s, "tool_use") == 0) return 3;
    if (strcmp(s, "error") == 0) return 4;
    if (strcmp(s, "aborted") == 0) return 5;
    return 0;
}

/* T156: migrate old entries table to split-column schema */
static int migrate_entries_split_columns(sqlite3 *db) {
    char *err = NULL;

    /* Drop old FTS and trigger first */
    sqlite3_exec(db, "DROP TRIGGER IF EXISTS entries_ai;", NULL, NULL, NULL);
    sqlite3_exec(db, "DROP TABLE IF EXISTS entries_fts;", NULL, NULL, NULL);

    /* Rename old table */
    if (sqlite3_exec(db, "ALTER TABLE entries RENAME TO entries_old;", NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "T156 migrate rename: %s\n", err);
        sqlite3_free(err);
        return -1;
    }

    /* Create new table */
    const char *create_sql =
        "CREATE TABLE entries ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  session_id INTEGER NOT NULL,"
        "  parent_id INTEGER NOT NULL DEFAULT -1,"
        "  original_parent_id INTEGER,"
        "  turn_id INTEGER,"
        "  role INTEGER NOT NULL DEFAULT 1,"
        "  content TEXT,"
        "  tool_calls TEXT,"
        "  tool_call_id TEXT,"
        "  tool_name TEXT,"
        "  is_error INTEGER NOT NULL DEFAULT 0,"
        "  stop_reason INTEGER NOT NULL DEFAULT 0,"
        "  model TEXT,"
        "  usage_in INTEGER,"
        "  usage_out INTEGER,"
        "  token_estimate INTEGER,"
        "  content_bytes INTEGER,"
        "  tool_call_count INTEGER NOT NULL DEFAULT 0,"
        "  data TEXT,"
        "  created_at INTEGER NOT NULL DEFAULT (unixepoch())"
        ");";
    if (sqlite3_exec(db, create_sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "T156 migrate create: %s\n", err);
        sqlite3_free(err);
        return -1;
    }

    /* Migrate data row by row (need cJSON to extract tool_calls properly) */
    sqlite3_stmt *sel;
    if (sqlite3_prepare_v2(db,
        "SELECT id, session_id, parent_id, turn_id, created_at, data, token_estimate, content_bytes FROM entries_old",
        -1, &sel, NULL) != SQLITE_OK)
        return -1;

    sqlite3_stmt *ins;
    const char *ins_sql =
        "INSERT INTO entries (id, session_id, parent_id, turn_id, role, content, tool_calls,"
        " tool_call_id, tool_name, stop_reason, token_estimate, content_bytes, tool_call_count, data, created_at)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
    if (sqlite3_prepare_v2(db, ins_sql, -1, &ins, NULL) != SQLITE_OK) {
        sqlite3_finalize(sel);
        return -1;
    }

    while (sqlite3_step(sel) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(sel, 0);
        int64_t session_id = sqlite3_column_int64(sel, 1);
        int64_t parent_id = sqlite3_column_int64(sel, 2);
        int64_t turn_id_val = sqlite3_column_int64(sel, 3);
        int has_turn = sqlite3_column_type(sel, 3) != SQLITE_NULL;
        int64_t created_at = sqlite3_column_int64(sel, 4);
        const char *data = (const char *)sqlite3_column_text(sel, 5);
        int tok_est = sqlite3_column_int(sel, 6);
        int cb = sqlite3_column_int(sel, 7);

        /* Parse data JSON to extract split columns */
        int role_int = 1;
        const char *content_str = NULL;
        const char *tool_calls_str = NULL;
        const char *tool_call_id_str = NULL;
        const char *tool_name_str = NULL;
        int stop_reason_int = 0;
        int tool_call_count_val = 0;
        char *tool_calls_alloc = NULL;

        cJSON *obj = data ? cJSON_Parse(data) : NULL;
        if (obj) {
            cJSON *role_j = cJSON_GetObjectItem(obj, "role");
            role_int = role_str_to_int(role_j ? role_j->valuestring : NULL);

            cJSON *sr = cJSON_GetObjectItem(obj, "stop_reason");
            stop_reason_int = stop_reason_str_to_int(sr ? sr->valuestring : NULL);

            if (role_int == 3) { /* tool_result */
                cJSON *c = cJSON_GetObjectItem(obj, "content");
                content_str = (c && c->valuestring) ? c->valuestring : NULL;
                cJSON *tcid = cJSON_GetObjectItem(obj, "tool_call_id");
                tool_call_id_str = (tcid && tcid->valuestring) ? tcid->valuestring : NULL;
            } else if (role_int == 2) { /* assistant */
                cJSON *content_arr = cJSON_GetObjectItem(obj, "content");
                if (cJSON_IsArray(content_arr)) {
                    int n = cJSON_GetArraySize(content_arr);
                    /* Extract text content */
                    for (int i = 0; i < n; i++) {
                        cJSON *item = cJSON_GetArrayItem(content_arr, i);
                        cJSON *type = cJSON_GetObjectItem(item, "type");
                        if (type && type->valuestring && strcmp(type->valuestring, "text") == 0) {
                            cJSON *t = cJSON_GetObjectItem(item, "text");
                            if (t && t->valuestring) content_str = t->valuestring;
                        }
                    }
                    /* Extract tool_calls into provider-neutral array */
                    cJSON *tc_arr = cJSON_CreateArray();
                    for (int i = 0; i < n; i++) {
                        cJSON *item = cJSON_GetArrayItem(content_arr, i);
                        cJSON *type = cJSON_GetObjectItem(item, "type");
                        if (!type || !type->valuestring || strcmp(type->valuestring, "tool_call") != 0)
                            continue;
                        cJSON *tc = cJSON_CreateObject();
                        cJSON *tc_id = cJSON_GetObjectItem(item, "id");
                        cJSON *tc_name = cJSON_GetObjectItem(item, "name");
                        cJSON *tc_args = cJSON_GetObjectItem(item, "arguments");
                        cJSON_AddStringToObject(tc, "id", tc_id && tc_id->valuestring ? tc_id->valuestring : "");
                        cJSON_AddStringToObject(tc, "name", tc_name && tc_name->valuestring ? tc_name->valuestring : "");
                        if (tc_args)
                            cJSON_AddItemToObject(tc, "args", cJSON_Duplicate(tc_args, 1));
                        else
                            cJSON_AddRawToObject(tc, "args", "{}");
                        cJSON_AddItemToArray(tc_arr, tc);
                        tool_call_count_val++;
                    }
                    if (tool_call_count_val > 0) {
                        tool_calls_alloc = cJSON_PrintUnformatted(tc_arr);
                        tool_calls_str = tool_calls_alloc;
                    }
                    cJSON_Delete(tc_arr);
                } else if (cJSON_IsString(content_arr)) {
                    content_str = content_arr->valuestring;
                }
            } else { /* user/system */
                cJSON *c = cJSON_GetObjectItem(obj, "content");
                content_str = (c && c->valuestring) ? c->valuestring : NULL;
            }
        }

        sqlite3_bind_int64(ins, 1, id);
        sqlite3_bind_int64(ins, 2, session_id);
        sqlite3_bind_int64(ins, 3, parent_id);
        if (has_turn) sqlite3_bind_int64(ins, 4, turn_id_val);
        else sqlite3_bind_null(ins, 4);
        sqlite3_bind_int(ins, 5, role_int);
        if (content_str) sqlite3_bind_text(ins, 6, content_str, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(ins, 6);
        if (tool_calls_str) sqlite3_bind_text(ins, 7, tool_calls_str, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(ins, 7);
        if (tool_call_id_str) sqlite3_bind_text(ins, 8, tool_call_id_str, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(ins, 8);
        if (tool_name_str) sqlite3_bind_text(ins, 9, tool_name_str, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(ins, 9);
        sqlite3_bind_int(ins, 10, stop_reason_int);
        sqlite3_bind_int(ins, 11, tok_est);
        sqlite3_bind_int(ins, 12, cb);
        sqlite3_bind_int(ins, 13, tool_call_count_val);
        if (data) sqlite3_bind_text(ins, 14, data, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(ins, 14);
        sqlite3_bind_int64(ins, 15, created_at);

        sqlite3_step(ins);
        sqlite3_reset(ins);
        sqlite3_clear_bindings(ins);

        free(tool_calls_alloc);
        if (obj) cJSON_Delete(obj);
    }
    sqlite3_finalize(sel);
    sqlite3_finalize(ins);

    /* Drop old table */
    sqlite3_exec(db, "DROP TABLE entries_old;", NULL, NULL, NULL);

    /* Create indexes + FTS + trigger (from schema) */
    sqlite3_exec(db, "CREATE INDEX idx_entries_session ON entries(session_id, id);", NULL, NULL, NULL);
    sqlite3_exec(db, "CREATE INDEX idx_entries_parent ON entries(parent_id);", NULL, NULL, NULL);
    sqlite3_exec(db, "CREATE INDEX idx_entries_session_role ON entries(session_id, role);", NULL, NULL, NULL);
    sqlite3_exec(db, "CREATE INDEX idx_entries_turn ON entries(session_id, turn_id);", NULL, NULL, NULL);
    sqlite3_exec(db, "CREATE INDEX idx_entries_stop_reason ON entries(session_id, stop_reason) WHERE stop_reason != 0;", NULL, NULL, NULL);
    sqlite3_exec(db, "CREATE INDEX idx_entries_plan ON entries(parent_id, session_id, id, role, stop_reason, token_estimate, tool_call_count);", NULL, NULL, NULL);
    sqlite3_exec(db,
        "CREATE VIRTUAL TABLE entries_fts USING fts5(content, content=entries, content_rowid=id);",
        NULL, NULL, NULL);
    sqlite3_exec(db,
        "CREATE TRIGGER entries_ai AFTER INSERT ON entries BEGIN"
        "  INSERT INTO entries_fts(rowid, content) VALUES ("
        "    new.id,"
        "    CASE new.role"
        "      WHEN 1 THEN new.content"
        "      WHEN 0 THEN new.content"
        "      WHEN 3 THEN COALESCE(new.tool_name,'') || ' ' || COALESCE(new.content,'')"
        "      WHEN 2 THEN COALESCE(new.content,'') || ' ' || COALESCE(new.tool_calls,'')"
        "      ELSE COALESCE(new.content,'')"
        "    END"
        "  );"
        "END;", NULL, NULL, NULL);

    /* Rebuild FTS index from existing entries */
    sqlite3_exec(db, "INSERT INTO entries_fts(entries_fts) VALUES('rebuild');", NULL, NULL, NULL);

    return 0;
}

/* V57: mmap + reduced cache for agent processes (not daemon) */
void db_set_agent_pragmas(sqlite3 *db) {
    sqlite3_exec(db, "PRAGMA mmap_size=67108864;", NULL, NULL, NULL);
    sqlite3_exec(db, "PRAGMA cache_size=-512;", NULL, NULL, NULL);
}

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

    /* T156: migrate old entries schema before creating tables */
    if (entries_needs_migration(db)) {
        if (migrate_entries_split_columns(db) != 0) {
            fprintf(stderr, "db_open: T156 migration failed\n");
            sqlite3_close(db);
            return NULL;
        }
    }

    /* T159: add original_parent_id if entries table exists without it */
    {
        sqlite3_stmt *chk;
        if (sqlite3_prepare_v2(db, "SELECT original_parent_id FROM entries LIMIT 0", -1, &chk, NULL) != SQLITE_OK) {
            /* Column missing — table exists but lacks column */
            sqlite3_stmt *tbl;
            if (sqlite3_prepare_v2(db, "SELECT 1 FROM entries LIMIT 0", -1, &tbl, NULL) == SQLITE_OK) {
                sqlite3_finalize(tbl);
                sqlite3_exec(db, "ALTER TABLE entries ADD COLUMN original_parent_id INTEGER;", NULL, NULL, NULL);
            }
        } else {
            sqlite3_finalize(chk);
        }
    }

    /* Create tables */
    rc = sqlite3_exec(db, SCHEMA_SQL, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_open schema: %s\n", err);
        sqlite3_free(err);
        sqlite3_close(db);
        return NULL;
    }

    /* T155: migrate soul/memory columns to memory_blocks, then drop columns */
    {
        sqlite3_stmt *chk;
        if (sqlite3_prepare_v2(db, "SELECT soul, memory FROM agents LIMIT 0", -1, &chk, NULL) == SQLITE_OK) {
            sqlite3_finalize(chk);
            /* Columns exist — migrate non-NULL content */
            sqlite3_stmt *scan;
            if (sqlite3_prepare_v2(db, "SELECT name, soul, memory FROM agents WHERE soul IS NOT NULL OR memory IS NOT NULL", -1, &scan, NULL) == SQLITE_OK) {
                while (sqlite3_step(scan) == SQLITE_ROW) {
                    const char *aname = (const char *)sqlite3_column_text(scan, 0);
                    const char *soul = (const char *)sqlite3_column_text(scan, 1);
                    const char *mem = (const char *)sqlite3_column_text(scan, 2);
                    if (aname && soul && soul[0]) {
                        MemoryBlock *existing = memory_block_get(db, aname, "persona");
                        if (!existing)
                            memory_block_create(db, aname, "persona", "Agent identity and tone", soul, 5000);
                        else
                            memory_block_free(existing);
                    }
                    if (aname && mem && mem[0]) {
                        MemoryBlock *existing = memory_block_get(db, aname, "human");
                        if (!existing)
                            memory_block_create(db, aname, "human", "Known facts about the user", mem, 5000);
                        else
                            memory_block_free(existing);
                    }
                }
                sqlite3_finalize(scan);
            }
            /* Drop columns */
            sqlite3_exec(db, "ALTER TABLE agents DROP COLUMN soul;", NULL, NULL, NULL);
            sqlite3_exec(db, "ALTER TABLE agents DROP COLUMN memory;", NULL, NULL, NULL);
        }
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

/* Serialize tool_calls array to provider-neutral JSON.
 * Format: [{"id":"...","name":"...","args":{...}},...] */
static char *serialize_tool_calls(const ToolCall *tcs, size_t count) {
    if (!tcs || count == 0) return NULL;
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        cJSON *tc = cJSON_CreateObject();
        cJSON_AddStringToObject(tc, "id", tcs[i].id ? tcs[i].id : "");
        cJSON_AddStringToObject(tc, "name", tcs[i].name ? tcs[i].name : "");
        if (tcs[i].arguments) {
            cJSON *args = cJSON_Parse(tcs[i].arguments);
            if (args)
                cJSON_AddItemToObject(tc, "args", args);
            else
                cJSON_AddRawToObject(tc, "args", "{}");
        } else {
            cJSON_AddRawToObject(tc, "args", "{}");
        }
        cJSON_AddItemToArray(arr, tc);
    }
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}

/* Deserialize tool_calls from provider-neutral JSON array.
 * Sets *out_count. Returns heap array or NULL. */
static ToolCall *deserialize_tool_calls(const char *json, size_t *out_count) {
    *out_count = 0;
    if (!json) return NULL;
    cJSON *arr = cJSON_Parse(json);
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(arr); return NULL; }
    int n = cJSON_GetArraySize(arr);
    if (n == 0) { cJSON_Delete(arr); return NULL; }
    ToolCall *tcs = malloc((size_t)n * sizeof(ToolCall));
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        cJSON *id = cJSON_GetObjectItem(item, "id");
        cJSON *name = cJSON_GetObjectItem(item, "name");
        cJSON *args = cJSON_GetObjectItem(item, "args");
        tcs[i].id = (id && id->valuestring) ? strdup(id->valuestring) : NULL;
        tcs[i].name = (name && name->valuestring) ? strdup(name->valuestring) : NULL;
        if (args) {
            char *s = cJSON_PrintUnformatted(args);
            tcs[i].arguments = s;
        } else {
            tcs[i].arguments = NULL;
        }
    }
    *out_count = (size_t)n;
    cJSON_Delete(arr);
    return tcs;
}

/* Populate Message from split columns read from DB row */
static void read_entry_from_columns(sqlite3_stmt *stmt, int col_role, int col_content,
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
            msg->tool_calls = deserialize_tool_calls(tc_json, &msg->tool_call_count);
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
     * entries may have higher ids than entries they precede in the path. */
    const char *branch_sql =
        "WITH RECURSIVE branch(id, parent_id, original_parent_id, session_id, created_at, role, content, tool_calls, tool_call_id, stop_reason, lvl) AS ("
        "  SELECT id, parent_id, original_parent_id, session_id, created_at, role, content, tool_calls, tool_call_id, stop_reason, 0"
        "    FROM entries WHERE id=? AND session_id=?"
        "  UNION ALL"
        "  SELECT e.id, e.parent_id, e.original_parent_id, e.session_id, e.created_at, e.role, e.content, e.tool_calls, e.tool_call_id, e.stop_reason, b.lvl+1"
        "    FROM entries e JOIN branch b ON e.id=b.parent_id"
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
        read_entry_from_columns(stmt, 5, 6, 7, 8, 9, &e->message);
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

/* V14: insert entry with given parent_id, update session leaf — split columns */
int64_t entry_append_at(sqlite3 *db, int64_t session_id, int64_t parent_id, const Message *msg) {
    const char *sql =
        "INSERT INTO entries (parent_id, session_id, role, content, tool_calls,"
        " tool_call_id, stop_reason, token_estimate, content_bytes, tool_call_count)"
        " VALUES (?,?,?,?,?,?,?,?,?,?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, parent_id);
    sqlite3_bind_int64(stmt, 2, session_id);
    sqlite3_bind_int(stmt, 3, role_to_int(msg->role));

    /* content */
    const char *content_val = NULL;
    if (msg->role == ROLE_TOOL && msg->tool_result)
        content_val = msg->tool_result->content;
    else
        content_val = msg->content;
    if (content_val) sqlite3_bind_text(stmt, 4, content_val, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 4);

    /* tool_calls */
    char *tc_json = NULL;
    int tc_count = 0;
    if (msg->role == ROLE_ASSISTANT && msg->tool_calls && msg->tool_call_count > 0) {
        tc_json = serialize_tool_calls(msg->tool_calls, msg->tool_call_count);
        tc_count = (int)msg->tool_call_count;
    }
    if (tc_json) sqlite3_bind_text(stmt, 5, tc_json, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 5);

    /* tool_call_id */
    if (msg->role == ROLE_TOOL && msg->tool_result && msg->tool_result->tool_call_id)
        sqlite3_bind_text(stmt, 6, msg->tool_result->tool_call_id, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, 6);

    /* stop_reason */
    sqlite3_bind_int(stmt, 7, stop_reason_to_int(msg->stop_reason));

    /* token_estimate + content_bytes */
    int content_len = content_val ? (int)strlen(content_val) : 0;
    int tc_len = tc_json ? (int)strlen(tc_json) : 0;
    int total_bytes = content_len + tc_len;
    sqlite3_bind_int(stmt, 8, (total_bytes / 4) + 4);
    sqlite3_bind_int(stmt, 9, total_bytes);
    sqlite3_bind_int(stmt, 10, tc_count);

    free(tc_json);

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
        "INSERT INTO entries (parent_id, session_id, role, content, stop_reason,"
        " token_estimate, content_bytes, tool_call_count)"
        " VALUES (?,?,4,?,0,?,?,0);";
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
            if (!tmp) break;
            entries = tmp;
        }
        Entry *e = &entries[*count];
        e->id = sqlite3_column_int64(stmt, 0);
        e->parent_id = sqlite3_column_int64(stmt, 1);
        e->original_parent_id = -1; /* not loaded in search results */
        e->session_id = sqlite3_column_int64(stmt, 2);
        e->created_at = (time_t)sqlite3_column_int64(stmt, 3);
        /* cols: 4=role, 5=content, 6=tool_calls, 7=tool_call_id, 8=stop_reason */
        read_entry_from_columns(stmt, 4, 5, 6, 7, 8, &e->message);
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
        "INSERT INTO entries (parent_id, session_id, turn_id, role, content, tool_calls,"
        " tool_call_id, stop_reason, token_estimate, content_bytes, tool_call_count)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?);";
    if (sqlite3_prepare_v2(db, ins_sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, parent_id);
    sqlite3_bind_int64(stmt, 2, session_id);
    sqlite3_bind_int64(stmt, 3, turn_id);
    sqlite3_bind_int(stmt, 4, role_to_int(msg->role));

    const char *content_val = NULL;
    if (msg->role == ROLE_TOOL && msg->tool_result)
        content_val = msg->tool_result->content;
    else
        content_val = msg->content;
    if (content_val) sqlite3_bind_text(stmt, 5, content_val, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 5);

    char *tc_json = NULL;
    int tc_count = 0;
    if (msg->role == ROLE_ASSISTANT && msg->tool_calls && msg->tool_call_count > 0) {
        tc_json = serialize_tool_calls(msg->tool_calls, msg->tool_call_count);
        tc_count = (int)msg->tool_call_count;
    }
    if (tc_json) sqlite3_bind_text(stmt, 6, tc_json, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 6);

    if (msg->role == ROLE_TOOL && msg->tool_result && msg->tool_result->tool_call_id)
        sqlite3_bind_text(stmt, 7, msg->tool_result->tool_call_id, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_null(stmt, 7);

    sqlite3_bind_int(stmt, 8, stop_reason_to_int(msg->stop_reason));

    int content_len = content_val ? (int)strlen(content_val) : 0;
    int tc_len = tc_json ? (int)strlen(tc_json) : 0;
    int total_bytes = content_len + tc_len;
    sqlite3_bind_int(stmt, 9, (total_bytes / 4) + 4);
    sqlite3_bind_int(stmt, 10, total_bytes);
    sqlite3_bind_int(stmt, 11, tc_count);
    free(tc_json);

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
    const char *sql = "SELECT id, name, config, system_prompt, heartbeat, "
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
            row->heartbeat = v ? strdup(v) : NULL;
            row->created_at = sqlite3_column_int64(stmt, 5);
            row->updated_at = sqlite3_column_int64(stmt, 6);
        }
    }
    sqlite3_finalize(stmt);
    return row;
}

int db_agent_upsert(sqlite3 *db, const char *name, const char *config,
                    const char *system_prompt, const char *heartbeat) {
    if (!db || !name) return -1;
    const char *sql =
        "INSERT INTO agents (name, config, system_prompt, heartbeat) "
        "VALUES (?, ?, ?, ?) "
        "ON CONFLICT(name) DO UPDATE SET "
        "config=excluded.config, system_prompt=excluded.system_prompt, "
        "heartbeat=excluded.heartbeat, "
        "updated_at=unixepoch()";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, config, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, system_prompt, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, heartbeat, -1, SQLITE_STATIC);
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

void memory_blocks_seed(sqlite3 *db, const char *agent_name, const char *agent_json_str) {
    if (!agent_json_str || !agent_name) return;
    cJSON *root = cJSON_Parse(agent_json_str);
    if (!root) return;
    cJSON *blocks = cJSON_GetObjectItemCaseSensitive(root, "memory_blocks");
    if (!cJSON_IsArray(blocks)) { cJSON_Delete(root); return; }
    cJSON *item;
    cJSON_ArrayForEach(item, blocks) {
        cJSON *lbl = cJSON_GetObjectItemCaseSensitive(item, "label");
        if (!cJSON_IsString(lbl)) continue;
        /* Only seed if not already in DB (DB authoritative) */
        MemoryBlock *existing = memory_block_get(db, agent_name, lbl->valuestring);
        if (existing) { memory_block_free(existing); continue; }
        cJSON *desc = cJSON_GetObjectItemCaseSensitive(item, "description");
        cJSON *val = cJSON_GetObjectItemCaseSensitive(item, "value");
        cJSON *limit = cJSON_GetObjectItemCaseSensitive(item, "char_limit");
        cJSON *ro = cJSON_GetObjectItemCaseSensitive(item, "read_only");
        int cl = cJSON_IsNumber(limit) ? (int)limit->valuedouble : 5000;
        int64_t id = memory_block_create(db, agent_name, lbl->valuestring,
                                         cJSON_IsString(desc) ? desc->valuestring : NULL,
                                         cJSON_IsString(val) ? val->valuestring : NULL, cl);
        if (id > 0 && cJSON_IsTrue(ro)) {
            /* Set read_only flag directly (not exposed via normal API) */
            const char *sql = "UPDATE memory_blocks SET read_only=1 WHERE id=?;";
            sqlite3_stmt *stmt;
            if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(stmt, 1, id);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
        }
    }
    cJSON_Delete(root);
}
