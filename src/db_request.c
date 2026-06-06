#define _POSIX_C_SOURCE 200809L
#include "db_request.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* T295: SQL request builder — produce complete LLM request JSON from entries table.
 *
 * Strategy: insert plan entry IDs into a temp table (preserving order),
 * then use a single SQL query with json_object()/json_group_array() to
 * produce the messages array. Wrap with model/tools/config in C. */

static int populate_plan_table(sqlite3 *db, const int64_t *ids, int count) {
    sqlite3_exec(db, "DROP TABLE IF EXISTS _plan;", NULL, NULL, NULL);
    sqlite3_exec(db,
        "CREATE TEMP TABLE _plan(pos INTEGER PRIMARY KEY, entry_id INTEGER NOT NULL);",
        NULL, NULL, NULL);

    sqlite3_stmt *ins;
    if (sqlite3_prepare_v2(db, "INSERT INTO _plan(pos, entry_id) VALUES(?,?);",
                           -1, &ins, NULL) != SQLITE_OK)
        return -1;

    for (int i = 0; i < count; i++) {
        sqlite3_bind_int(ins, 1, i);
        sqlite3_bind_int64(ins, 2, ids[i]);
        if (sqlite3_step(ins) != SQLITE_DONE) {
            sqlite3_finalize(ins);
            return -1;
        }
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    return 0;
}

/* OpenAI messages: json_group_array of json_objects in plan order.
 * tool_calls args stringified via format('%s',...) to strip JSON subtype. */
static const char SQL_OPENAI_MESSAGES[] =
    "SELECT json_group_array(json(msg) ORDER BY pos) FROM ("
    "  SELECT p.pos,"
    "    CASE"
    "      WHEN e.role = 3 THEN"
    "        json_object('role','tool','tool_call_id',e.tool_call_id,"
    "                    'content',COALESCE(e.content,''))"
    "      WHEN e.role = 2 AND e.tool_calls IS NOT NULL THEN"
    "        json_patch("
    "          json_object('role','assistant','content',e.content),"
    "          json_object('tool_calls',"
    "            (SELECT json_group_array("
    "              json_object('id',json_extract(tc.value,'$.id'),"
    "                'type','function',"
    "                'function',json_object("
    "                  'name',json_extract(tc.value,'$.name'),"
    "                  'arguments',format('%s',json_extract(tc.value,'$.args'))"
    "                ))"
    "            ) FROM json_each(e.tool_calls) tc)"
    "          ))"
    "      WHEN e.role = 2 THEN"
    "        json_object('role','assistant','content',e.content)"
    "      WHEN e.role = 0 THEN"
    "        json_object('role','system','content',COALESCE(e.content,''))"
    "      ELSE"
    "        json_object('role','user','content',COALESCE(e.content,''))"
    "    END AS msg"
    "  FROM _plan p JOIN entries e ON e.id = p.entry_id AND e.session_id = ?"
    ") sub;";

/* OpenAI with cache_control on system messages whose id > cache_break_after. */
static const char SQL_OPENAI_MESSAGES_CACHED[] =
    "SELECT json_group_array(json(msg) ORDER BY pos) FROM ("
    "  SELECT p.pos,"
    "    CASE"
    "      WHEN e.role = 3 THEN"
    "        json_object('role','tool','tool_call_id',e.tool_call_id,"
    "                    'content',COALESCE(e.content,''))"
    "      WHEN e.role = 2 AND e.tool_calls IS NOT NULL THEN"
    "        json_patch("
    "          json_object('role','assistant','content',e.content),"
    "          json_object('tool_calls',"
    "            (SELECT json_group_array("
    "              json_object('id',json_extract(tc.value,'$.id'),"
    "                'type','function',"
    "                'function',json_object("
    "                  'name',json_extract(tc.value,'$.name'),"
    "                  'arguments',format('%s',json_extract(tc.value,'$.args'))"
    "                ))"
    "            ) FROM json_each(e.tool_calls) tc)"
    "          ))"
    "      WHEN e.role = 2 THEN"
    "        json_object('role','assistant','content',e.content)"
    "      WHEN e.role = 0 AND e.id > ?1 THEN"
    "        json_object('role','system','content',"
    "          json_array(json_object('type','text',"
    "            'text',COALESCE(e.content,''),"
    "            'cache_control',json_object('type','ephemeral'))))"
    "      WHEN e.role = 0 THEN"
    "        json_object('role','system','content',COALESCE(e.content,''))"
    "      ELSE"
    "        json_object('role','user','content',COALESCE(e.content,''))"
    "    END AS msg"
    "  FROM _plan p JOIN entries e ON e.id = p.entry_id AND e.session_id = ?2"
    ") sub;";

/* Gemini contents[] (non-system entries only). */
static const char SQL_GEMINI_CONTENTS[] =
    "SELECT json_group_array(json(msg) ORDER BY pos) FROM ("
    "  SELECT p.pos,"
    "    CASE"
    "      WHEN e.role = 3 THEN"
    "        json_object('role','user','parts',"
    "          json_array(json_object('functionResponse',"
    "            json_object('name',e.tool_call_id,"
    "              'response',json_object('content',COALESCE(e.content,''))))))"
    "      WHEN e.role = 2 AND e.tool_calls IS NOT NULL AND e.content IS NOT NULL AND e.content != '' THEN"
    "        json_object('role','model','parts',"
    "          (SELECT json_group_array(json(part)) FROM ("
    "            SELECT 0 AS ord, json_object('text',e.content) AS part"
    "            UNION ALL"
    "            SELECT 1, json_object('functionCall',"
    "              json_object('name',json_extract(tc.value,'$.name'),"
    "                'args',json(json_extract(tc.value,'$.args'))))"
    "            FROM json_each(e.tool_calls) tc"
    "          ) ORDER BY ord))"
    "      WHEN e.role = 2 AND e.tool_calls IS NOT NULL THEN"
    "        json_object('role','model','parts',"
    "          (SELECT json_group_array(json("
    "            json_object('functionCall',"
    "              json_object('name',json_extract(tc.value,'$.name'),"
    "                'args',json(json_extract(tc.value,'$.args'))))))"
    "          FROM json_each(e.tool_calls) tc))"
    "      WHEN e.role = 2 THEN"
    "        json_object('role','model','parts',"
    "          json_array(json_object('text',COALESCE(e.content,''))))"
    "      ELSE"
    "        json_object('role','user','parts',"
    "          json_array(json_object('text',COALESCE(e.content,''))))"
    "    END AS msg"
    "  FROM _plan p JOIN entries e ON e.id = p.entry_id AND e.session_id = ?"
    "  WHERE e.role != 0"
    ") sub;";

/* Gemini systemInstruction: concatenate all system messages. */
static const char SQL_GEMINI_SYSTEM[] =
    "SELECT group_concat(e.content, char(10))"
    " FROM _plan p JOIN entries e ON e.id = p.entry_id AND e.session_id = ?"
    " WHERE e.role = 0 AND e.content IS NOT NULL AND e.content != ''"
    " ORDER BY p.pos;";

char *db_build_request(sqlite3 *db, int64_t session_id,
                       const int64_t *entry_ids, int entry_count,
                       const char *model, EndpointType endpoint_type,
                       const char *tools_json, int max_tokens,
                       int stream, int64_t cache_break_after) {
    if (!db || !entry_ids || entry_count <= 0 || !model) return NULL;

    if (populate_plan_table(db, entry_ids, entry_count) != 0)
        return NULL;

    char *result = NULL;

    if (endpoint_type == ENDPOINT_GEMINI) {
        /* 1. systemInstruction text */
        sqlite3_stmt *sys_stmt;
        if (sqlite3_prepare_v2(db, SQL_GEMINI_SYSTEM, -1, &sys_stmt, NULL) != SQLITE_OK)
            goto cleanup;
        sqlite3_bind_int64(sys_stmt, 1, session_id);
        const char *sys_text = NULL;
        char *sys_copy = NULL;
        if (sqlite3_step(sys_stmt) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(sys_stmt, 0);
            if (t) sys_copy = strdup(t);
        }
        sqlite3_finalize(sys_stmt);
        sys_text = sys_copy;

        /* 2. contents array */
        sqlite3_stmt *cont_stmt;
        if (sqlite3_prepare_v2(db, SQL_GEMINI_CONTENTS, -1, &cont_stmt, NULL) != SQLITE_OK) {
            free(sys_copy);
            goto cleanup;
        }
        sqlite3_bind_int64(cont_stmt, 1, session_id);
        char *contents_copy = NULL;
        if (sqlite3_step(cont_stmt) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(cont_stmt, 0);
            if (t) contents_copy = strdup(t);
        }
        sqlite3_finalize(cont_stmt);

        /* 3. Assemble */
        cJSON *root = cJSON_CreateObject();
        if (sys_text && sys_text[0]) {
            cJSON *si = cJSON_CreateObject();
            cJSON *parts = cJSON_CreateArray();
            cJSON *part = cJSON_CreateObject();
            cJSON_AddStringToObject(part, "text", sys_text);
            cJSON_AddItemToArray(parts, part);
            cJSON_AddItemToObject(si, "parts", parts);
            cJSON_AddItemToObject(root, "systemInstruction", si);
        }
        if (contents_copy) {
            cJSON *contents = cJSON_Parse(contents_copy);
            if (contents)
                cJSON_AddItemToObject(root, "contents", contents);
        }
        if (tools_json) {
            cJSON *tools = cJSON_Parse(tools_json);
            if (tools)
                cJSON_AddItemToObject(root, "tools", tools);
        }
        if (max_tokens > 0) {
            cJSON *gc = cJSON_CreateObject();
            cJSON_AddNumberToObject(gc, "maxOutputTokens", max_tokens);
            cJSON_AddItemToObject(root, "generationConfig", gc);
        }

        result = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        free(sys_copy);
        free(contents_copy);
    } else {
        /* OpenAI-compatible */
        const char *sql = (cache_break_after >= 0)
            ? SQL_OPENAI_MESSAGES_CACHED : SQL_OPENAI_MESSAGES;

        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
            goto cleanup;

        if (cache_break_after >= 0) {
            sqlite3_bind_int64(stmt, 1, cache_break_after);
            sqlite3_bind_int64(stmt, 2, session_id);
        } else {
            sqlite3_bind_int64(stmt, 1, session_id);
        }

        char *messages_copy = NULL;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(stmt, 0);
            if (t) messages_copy = strdup(t);
        }
        sqlite3_finalize(stmt);

        if (!messages_copy) goto cleanup;

        /* Assemble full request */
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "model", model);
        cJSON *messages = cJSON_Parse(messages_copy);
        if (messages)
            cJSON_AddItemToObject(root, "messages", messages);
        if (tools_json) {
            cJSON *tools = cJSON_Parse(tools_json);
            if (tools)
                cJSON_AddItemToObject(root, "tools", tools);
        }
        if (stream)
            cJSON_AddTrueToObject(root, "stream");
        if (max_tokens > 0)
            cJSON_AddNumberToObject(root, "max_tokens", max_tokens);

        result = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        free(messages_copy);
    }

cleanup:
    sqlite3_exec(db, "DROP TABLE IF EXISTS _plan;", NULL, NULL, NULL);
    return result;
}

/* ── Typed-entry egress: OpenAI chat/completions ──────────────────────────
 * Reads from flat typed entries, groups tool_call siblings with assistant_message.
 * Uses _plan temp table (same pattern as legacy). replay_reasoning controls
 * whether reasoning entries are included. */
static const char SQL_OPENAI_TYPED[] =
    "SELECT json_group_array(json(msg) ORDER BY pos) FROM ("
    /* Group by pos: each turn_id may produce one or two messages:
     * - assistant_message + tool_calls → single assistant message with tool_calls[]
     * - tool_result entries → individual tool role messages
     * Entries that don't need grouping go through directly. */
    "  SELECT p.pos, "
    "    CASE e.type"
    "      WHEN 'system' THEN"
    "        json_object('role','system','content',COALESCE(e.content,''))"
    "      WHEN 'user_message' THEN"
    "        json_object('role','user','content',COALESCE(e.content,''))"
    "      WHEN 'assistant_message' THEN"
    "        CASE WHEN EXISTS("
    "          SELECT 1 FROM entries tc WHERE tc.session_id=e.session_id"
    "            AND tc.turn_id=e.turn_id AND tc.type='tool_call')"
    "        THEN json_patch("
    "          json_object('role','assistant','content',e.content),"
    "          json_object('tool_calls',"
    "            (SELECT json_group_array("
    "              json_object('id',tc2.tool_call_id,'type','function',"
    "                'function',json_object('name',tc2.tool_name,"
    "                  'arguments',COALESCE(tc2.content,'{}'))))"
    "             FROM entries tc2 WHERE tc2.session_id=e.session_id"
    "               AND tc2.turn_id=e.turn_id AND tc2.type='tool_call'"
    "             ORDER BY tc2.part_index)))"
    "        ELSE json_object('role','assistant','content',e.content)"
    "        END"
    "      WHEN 'tool_result' THEN"
    "        json_object('role','tool','tool_call_id',e.tool_call_id,"
    "                    'content',COALESCE(e.content,''))"
    "      ELSE NULL"
    "    END AS msg"
    "  FROM _plan p JOIN entries e ON e.id = p.entry_id AND e.session_id = ?1"
    "  WHERE e.type NOT IN ('tool_call','reasoning')"
    ") sub WHERE msg IS NOT NULL;";

/* Same but includes reasoning as reasoning_content on assistant messages */
static const char SQL_OPENAI_TYPED_WITH_REASONING[] =
    "SELECT json_group_array(json(msg) ORDER BY pos) FROM ("
    "  SELECT p.pos, "
    "    CASE e.type"
    "      WHEN 'system' THEN"
    "        json_object('role','system','content',COALESCE(e.content,''))"
    "      WHEN 'user_message' THEN"
    "        json_object('role','user','content',COALESCE(e.content,''))"
    "      WHEN 'assistant_message' THEN"
    "        CASE WHEN EXISTS("
    "          SELECT 1 FROM entries tc WHERE tc.session_id=e.session_id"
    "            AND tc.turn_id=e.turn_id AND tc.type='tool_call')"
    "        THEN json_patch("
    "          json_patch("
    "            json_object('role','assistant','content',e.content),"
    "            CASE WHEN EXISTS(SELECT 1 FROM entries r WHERE r.session_id=e.session_id"
    "              AND r.turn_id=e.turn_id AND r.type='reasoning')"
    "            THEN json_object('reasoning_content',"
    "              (SELECT r2.content FROM entries r2 WHERE r2.session_id=e.session_id"
    "                AND r2.turn_id=e.turn_id AND r2.type='reasoning' LIMIT 1))"
    "            ELSE '{}' END),"
    "          json_object('tool_calls',"
    "            (SELECT json_group_array("
    "              json_object('id',tc2.tool_call_id,'type','function',"
    "                'function',json_object('name',tc2.tool_name,"
    "                  'arguments',COALESCE(tc2.content,'{}'))))"
    "             FROM entries tc2 WHERE tc2.session_id=e.session_id"
    "               AND tc2.turn_id=e.turn_id AND tc2.type='tool_call'"
    "             ORDER BY tc2.part_index)))"
    "        ELSE json_patch("
    "          json_object('role','assistant','content',e.content),"
    "          CASE WHEN EXISTS(SELECT 1 FROM entries r WHERE r.session_id=e.session_id"
    "            AND r.turn_id=e.turn_id AND r.type='reasoning')"
    "          THEN json_object('reasoning_content',"
    "            (SELECT r2.content FROM entries r2 WHERE r2.session_id=e.session_id"
    "              AND r2.turn_id=e.turn_id AND r2.type='reasoning' LIMIT 1))"
    "          ELSE '{}' END)"
    "        END"
    "      WHEN 'tool_result' THEN"
    "        json_object('role','tool','tool_call_id',e.tool_call_id,"
    "                    'content',COALESCE(e.content,''))"
    "      ELSE NULL"
    "    END AS msg"
    "  FROM _plan p JOIN entries e ON e.id = p.entry_id AND e.session_id = ?1"
    "  WHERE e.type NOT IN ('tool_call','reasoning')"
    ") sub WHERE msg IS NOT NULL;";

char *db_build_request_typed(sqlite3 *db, int64_t session_id,
                             const int64_t *entry_ids, int entry_count,
                             EndpointType endpoint_type,
                             int replay_reasoning) {
    if (!db || !entry_ids || entry_count <= 0) return NULL;
    if (populate_plan_table(db, entry_ids, entry_count) != 0) return NULL;

    char *messages_json = NULL;

    if (endpoint_type == ENDPOINT_OPENAI) {
        const char *sql = replay_reasoning ? SQL_OPENAI_TYPED_WITH_REASONING : SQL_OPENAI_TYPED;
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
            goto done;
        sqlite3_bind_int64(stmt, 1, session_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(stmt, 0);
            if (t) messages_json = strdup(t);
        }
        sqlite3_finalize(stmt);
    } else if (endpoint_type == ENDPOINT_RESPONSES) {
        /* OpenRouter responses API: near-1:1 mapping from typed entries.
         * Each entry becomes an input item. */
        const char *sql =
            "SELECT json_group_array(json(item) ORDER BY pos) FROM ("
            "  SELECT p.pos,"
            "    CASE e.type"
            "      WHEN 'system' THEN"
            "        json_object('type','message','role','developer',"
            "                    'content',COALESCE(e.content,''))"
            "      WHEN 'user_message' THEN"
            "        json_object('type','message','role','user',"
            "                    'content',COALESCE(e.content,''))"
            "      WHEN 'assistant_message' THEN"
            "        json_object('type','message','role','assistant',"
            "                    'content',COALESCE(e.content,''))"
            "      WHEN 'tool_call' THEN"
            "        json_object('type','function_call','call_id',e.tool_call_id,"
            "                    'name',e.tool_name,'arguments',COALESCE(e.content,'{}'))"
            "      WHEN 'tool_result' THEN"
            "        json_object('type','function_call_output','call_id',e.tool_call_id,"
            "                    'output',COALESCE(e.content,''))"
            "      WHEN 'reasoning' THEN"
            "        CASE WHEN ?2 THEN"
            "          json_object('type','reasoning','content',"
            "            json_array(json_object('type','reasoning_content',"
            "              'content',COALESCE(e.content,''))))"
            "        ELSE NULL END"
            "      ELSE NULL"
            "    END AS item"
            "  FROM _plan p JOIN entries e ON e.id = p.entry_id AND e.session_id = ?1"
            ") sub WHERE item IS NOT NULL;";
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
            goto done;
        sqlite3_bind_int64(stmt, 1, session_id);
        sqlite3_bind_int(stmt, 2, replay_reasoning);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(stmt, 0);
            if (t) messages_json = strdup(t);
        }
        sqlite3_finalize(stmt);
    } else if (endpoint_type == ENDPOINT_GEMINI) {
        /* Gemini generateContent: group by turn_id into contents[] with parts[].
         * System entries returned separately (caller wraps as systemInstruction).
         * tool_call args are JSON objects (not strings). */
        const char *sql =
            "SELECT json_group_array(json(content_obj) ORDER BY min_pos) FROM ("
            "  SELECT MIN(p.pos) AS min_pos,"
            "    CASE"
            "      WHEN e.type IN ('assistant_message','tool_call','reasoning') THEN"
            "        json_object('role','model','parts',"
            "          (SELECT json_group_array(json(part)) FROM ("
            "            SELECT e2.part_index AS ord,"
            "              CASE e2.type"
            "                WHEN 'reasoning' THEN"
            "                  CASE WHEN ?2 THEN json_object('thought',json('true'),'text',e2.content)"
            "                  ELSE NULL END"
            "                WHEN 'assistant_message' THEN"
            "                  CASE WHEN e2.content IS NOT NULL AND e2.content != ''"
            "                  THEN json_object('text',e2.content) ELSE NULL END"
            "                WHEN 'tool_call' THEN"
            "                  json_object('functionCall',json_object('name',e2.tool_name,"
            "                    'args',CASE WHEN json_valid(e2.content) THEN json(e2.content)"
            "                             ELSE json('{}') END))"
            "                ELSE NULL"
            "              END AS part"
            "            FROM _plan p2 JOIN entries e2 ON e2.id=p2.entry_id AND e2.session_id=?1"
            "            WHERE e2.turn_id=e.turn_id"
            "              AND e2.type IN ('assistant_message','tool_call','reasoning')"
            "            ORDER BY e2.part_index"
            "          ) WHERE part IS NOT NULL))"
            "      WHEN e.type = 'tool_result' THEN"
            "        json_object('role','user','parts',"
            "          json_array(json_object('functionResponse',"
            "            json_object('name',COALESCE(e.tool_name,e.tool_call_id),"
            "              'response',json_object('content',COALESCE(e.content,''))))))"
            "      WHEN e.type = 'user_message' THEN"
            "        json_object('role','user','parts',"
            "          json_array(json_object('text',COALESCE(e.content,''))))"
            "      ELSE NULL"
            "    END AS content_obj"
            "  FROM _plan p JOIN entries e ON e.id = p.entry_id AND e.session_id = ?1"
            "  WHERE e.type != 'system'"
            "  GROUP BY CASE WHEN e.type IN ('assistant_message','tool_call','reasoning')"
            "    THEN e.turn_id ELSE e.id END"
            ") sub WHERE content_obj IS NOT NULL;";
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
            goto done;
        sqlite3_bind_int64(stmt, 1, session_id);
        sqlite3_bind_int(stmt, 2, replay_reasoning);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(stmt, 0);
            if (t) messages_json = strdup(t);
        }
        sqlite3_finalize(stmt);
    } else if (endpoint_type == ENDPOINT_GEMINI_INTERACTIONS) {
        /* Gemini interactions API: flat steps, similar to responses API.
         * Only include entries with id > last_synced_entry_id (delta mode).
         * If last_synced_entry_id is NULL/0, send all (full history). */
        int64_t last_synced = 0;
        sqlite3_stmt *ss;
        if (sqlite3_prepare_v2(db,
                "SELECT COALESCE(last_synced_entry_id,0) FROM sessions WHERE id=?;",
                -1, &ss, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(ss, 1, session_id);
            if (sqlite3_step(ss) == SQLITE_ROW)
                last_synced = sqlite3_column_int64(ss, 0);
            sqlite3_finalize(ss);
        }

        const char *sql =
            "SELECT json_group_array(json(step) ORDER BY pos) FROM ("
            "  SELECT p.pos,"
            "    CASE e.type"
            "      WHEN 'user_message' THEN"
            "        json_object('type','user_input','content',COALESCE(e.content,''))"
            "      WHEN 'assistant_message' THEN"
            "        json_object('type','model_output','content',COALESCE(e.content,''))"
            "      WHEN 'tool_call' THEN"
            "        json_object('type','function_call','name',e.tool_name,"
            "                    'call_id',e.tool_call_id,"
            "                    'args',CASE WHEN json_valid(e.content) THEN json(e.content)"
            "                             ELSE json('{}') END)"
            "      WHEN 'tool_result' THEN"
            "        json_object('type','function_result','name',COALESCE(e.tool_name,''),"
            "                    'call_id',e.tool_call_id,"
            "                    'output',COALESCE(e.content,''))"
            "      WHEN 'reasoning' THEN"
            "        CASE WHEN ?2 THEN json_object('type','thought','content',e.content)"
            "        ELSE NULL END"
            "      ELSE NULL"
            "    END AS step"
            "  FROM _plan p JOIN entries e ON e.id = p.entry_id AND e.session_id = ?1"
            "  WHERE e.type != 'system' AND e.id > ?3"
            ") sub WHERE step IS NOT NULL;";
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
            goto done;
        sqlite3_bind_int64(stmt, 1, session_id);
        sqlite3_bind_int(stmt, 2, replay_reasoning);
        sqlite3_bind_int64(stmt, 3, last_synced);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(stmt, 0);
            if (t) messages_json = strdup(t);
        }
        sqlite3_finalize(stmt);
    }

done:
    sqlite3_exec(db, "DROP TABLE IF EXISTS _plan;", NULL, NULL, NULL);
    return messages_json;
}

int session_set_cache_break(sqlite3 *db, int64_t session_id, int64_t entry_id) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
            "UPDATE sessions SET cache_break_after=? WHERE id=?;",
            -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, entry_id);
    sqlite3_bind_int64(stmt, 2, session_id);
    int rc = (sqlite3_step(stmt) == SQLITE_DONE) ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

int64_t session_get_cache_break(sqlite3 *db, int64_t session_id) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
            "SELECT cache_break_after FROM sessions WHERE id=?;",
            -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, session_id);
    int64_t val = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        val = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return val;
}
