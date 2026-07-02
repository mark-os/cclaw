#define _POSIX_C_SOURCE 200809L
#include "llm_payload.h"
#include "agent_config.h"
#include "context.h"
#include "config.h"
#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Populate temp plan table with entry IDs in order ──────────── */

static int populate_plan(sqlite3 *db, const ContextPlan *plan) {
    sqlite3_exec(db, "DROP TABLE IF EXISTS _plan;", NULL, NULL, NULL);
    sqlite3_exec(db,
        "CREATE TEMP TABLE _plan(pos INTEGER PRIMARY KEY, entry_id INTEGER NOT NULL);",
        NULL, NULL, NULL);
    sqlite3_stmt *ins;
    if (sqlite3_prepare_v2(db, "INSERT INTO _plan(pos,entry_id) VALUES(?,?);",
                           -1, &ins, NULL) != SQLITE_OK)
        return -1;
    int pos = 0;
    for (int i = 0; i < plan->count; i++) {
        if (!plan->entries[i].include) continue;  /* cut-excluded, not pinned */
        sqlite3_bind_int(ins, 1, pos++);
        sqlite3_bind_int64(ins, 2, plan->entries[i].id);
        sqlite3_step(ins);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    return 0;
}

/* ── SQL templates ─────────────────────────────────────────────── */

/* Query-time untrusted-content wrap: a tool_result whose entry carries a
 * network_hosts tag came (at least partly) from the network. Content was
 * sanitized at storage time (invisible Unicode stripped, marker lookalikes
 * neutralized), so static boundary markers here cannot be forged from inside. */
#define SQL_WRAP_TOOL_CONTENT \
    "CASE WHEN e.network_hosts IS NOT NULL THEN" \
    "  '<<<UNTRUSTED_EXTERNAL_CONTENT>>>' || char(10) ||" \
    "  'Contacted hosts: ' || e.network_hosts || char(10) || '---' || char(10) ||" \
    "  COALESCE(e.content,'') || char(10) ||" \
    "  '<<<END_UNTRUSTED_EXTERNAL_CONTENT>>>'" \
    " ELSE COALESCE(e.content,'') END"

static const char SQL_OPENAI_MESSAGES[] =
    "SELECT json_group_array(json(msg) ORDER BY pos) FROM ("
    "  SELECT p.pos,"
    "    CASE e.type"
    "      WHEN 'system' THEN"
    "        json_object('role','system','content',COALESCE(e.content,''))"
    "      WHEN 'compaction' THEN"
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
    "                    'content'," SQL_WRAP_TOOL_CONTENT ")"
    "      ELSE NULL"
    "    END AS msg"
    "  FROM _plan p JOIN entries e ON e.id = p.entry_id AND e.session_id = ?1"
    "  WHERE e.type NOT IN ('tool_call','reasoning')"
    /* Ephemeral hook injects ride at the tail of history (before recall, which
     * SQL_OPENAI_FULL appends after) — preserves prompt-prefix caching. */
    "  UNION ALL"
    "  SELECT 1000000+hd.id AS pos,"
    "    json_object('role', hd.role, 'content', hd.content) AS msg"
    "  FROM hook_directives hd WHERE hd.session_id = ?1 AND hd.kind = 'inject'"
    ") sub WHERE msg IS NOT NULL;";

/* launch_agent's description carries a live roster of delegable agents,
 * recomputed every turn — no refresh plumbing when agents come and go. */
#define SQL_TOOL_DESCRIPTION \
    "CASE WHEN t.name='launch_agent' THEN" \
    "  t.description || COALESCE(' Available agents: ' ||" \
    "    (SELECT group_concat(a.name || ' — ' ||" \
    "       COALESCE(a.description,'(no description)'), '; ') FROM agents a), '')" \
    " ELSE t.description END"

static const char SQL_OPENAI_TOOLS[] =
    "SELECT json_group_array("
    "  json_object('type','function','function',"
    "    json_object('name',t.name,'description'," SQL_TOOL_DESCRIPTION ","
    "      'parameters',json(t.parameters_json)))"
    ") FROM tools t"
    " WHERE t.enabled=1"
    "   AND (t.agent_name IS NULL OR t.agent_name=?1)"
    "   AND ((?2 IS NULL OR t.name IN (SELECT value FROM json_each(?2)))"
    "        OR t.extension_name IN (SELECT ae.extension_name FROM agent_extensions ae"
    "             JOIN extensions e ON e.name=ae.extension_name"
    "             WHERE ae.agent_name=?1 AND ae.enabled=1"
    "               AND (e.published=1 OR e.owner_agent=?1)));";

/* Both FULL templates wrap json_object in json_patch('{}', ...): RFC 7386
 * merge drops top-level NULL-valued keys, which json_object would otherwise
 * emit as JSON null ("max_tokens":null etc.) — strict providers 400 on those. */
static const char SQL_OPENAI_FULL[] =
    "SELECT json_patch('{}', json_object("
    "  'model', ?1,"
    "  'messages', (SELECT json_group_array(json(m)) FROM ("
    "    SELECT 0 AS ord, 0 AS sub, json_object('role','system','content',?2) AS m"
    "      WHERE ?2 IS NOT NULL"
    "    UNION ALL"
    "    SELECT 1, rowid, json(value) FROM json_each(?3)"
    "    UNION ALL"
    /* Auto-recall rides at the tail as a user message: keeps the prompt
     * prefix byte-stable across turns so provider prompt caching works. */
    "    SELECT 2, 0, json_object('role','user','content',?6)"
    "      WHERE ?6 IS NOT NULL"
    "    ORDER BY ord, sub)),"
    "  'max_tokens', CASE WHEN ?4 > 0 THEN ?4 ELSE NULL END,"
    "  'tools', CASE WHEN json_array_length(?5) > 0 THEN json(?5) ELSE NULL END"
    "));";

static const char SQL_GEMINI_CONTENTS[] =
    "SELECT json_group_array(json(content_obj) ORDER BY min_pos) FROM ("
    "  SELECT MIN(p.pos) AS min_pos,"
    "    CASE"
    "      WHEN e.type IN ('assistant_message','tool_call','reasoning') THEN"
    "        json_object('role','model','parts',"
    "          (SELECT json_group_array(json(part)) FROM ("
    "            SELECT e2.part_index AS ord,"
    "              CASE e2.type"
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
    "              AND e2.type IN ('assistant_message','tool_call')"
    "            ORDER BY e2.part_index"
    "          ) WHERE part IS NOT NULL))"
    "      WHEN e.type = 'tool_result' THEN"
    "        json_object('role','user','parts',"
    "          json_array(json_object('functionResponse',"
    "            json_object('name',COALESCE(e.tool_name,e.tool_call_id),"
    "              'response',json_object('content'," SQL_WRAP_TOOL_CONTENT ")))))"
    "      WHEN e.type = 'user_message' THEN"
    "        json_object('role','user','parts',"
    "          json_array(json_object('text',COALESCE(e.content,''))))"
    "      WHEN e.type = 'compaction' THEN"
    "        json_object('role','user','parts',"
    "          json_array(json_object('text',"
    "            '[Summary of earlier conversation]' || char(10) || COALESCE(e.content,''))))"
    "      ELSE NULL"
    "    END AS content_obj"
    "  FROM _plan p JOIN entries e ON e.id = p.entry_id AND e.session_id = ?1"
    "  WHERE e.type != 'system'"
    "  GROUP BY CASE WHEN e.type IN ('assistant_message','tool_call','reasoning')"
    "    THEN e.turn_id ELSE e.id END"
    /* Ephemeral hook injects: Gemini has no system role mid-contents, so a
     * system inject becomes a '[system] '-prefixed user part at history tail. */
    "  UNION ALL"
    "  SELECT 1000000+hd.id AS min_pos,"
    "    json_object('role','user','parts',json_array(json_object('text',"
    "      CASE WHEN hd.role='system' THEN '[system] ' || hd.content"
    "           ELSE hd.content END))) AS content_obj"
    "  FROM hook_directives hd WHERE hd.session_id = ?1 AND hd.kind = 'inject'"
    ") sub WHERE content_obj IS NOT NULL;";

static const char SQL_GEMINI_TOOLS[] =
    "SELECT json_array(json_object('functionDeclarations',"
    "  (SELECT json_group_array("
    "    json_object('name',t.name,'description'," SQL_TOOL_DESCRIPTION ","
    "      'parameters',json(t.parameters_json))"
    "  ) FROM tools t"
    "   WHERE t.enabled=1"
    "     AND (t.agent_name IS NULL OR t.agent_name=?1)"
    "     AND ((?2 IS NULL OR t.name IN (SELECT value FROM json_each(?2)))"
    "          OR t.extension_name IN (SELECT ae.extension_name FROM agent_extensions ae"
    "               JOIN extensions e ON e.name=ae.extension_name"
    "               WHERE ae.agent_name=?1 AND ae.enabled=1"
    "                 AND (e.published=1 OR e.owner_agent=?1))))"
    "));";

static const char SQL_GEMINI_FULL[] =
    "SELECT json_patch('{}', json_object("
    "  'systemInstruction', CASE WHEN ?1 IS NOT NULL AND ?1 != ''"
    "    THEN json_object('parts',json_array(json_object('text',?1)))"
    "    ELSE NULL END,"
    /* Auto-recall (?5) appends as a trailing user content — never into
     * systemInstruction, which would break prefix caching every turn. */
    "  'contents', CASE WHEN ?5 IS NOT NULL"
    "    THEN json_insert(json(?2),'$[#]',"
    "      json_object('role','user','parts',"
    "        json_array(json_object('text',?5))))"
    "    ELSE json(?2) END,"
    "  'tools', CASE WHEN json_array_length(json_extract(?3,'$[0].functionDeclarations')) > 0"
    "    THEN json(?3) ELSE NULL END,"
    "  'generationConfig', CASE WHEN ?4 > 0"
    "    THEN json_object('maxOutputTokens',?4) ELSE NULL END"
    "));";

/* ── Helpers ───────────────────────────────────────────────────── */

/* Session's spawn-frozen tool filter, or NULL when unrestricted. */
static char *session_tool_filter(sqlite3 *db, int64_t session_id) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, "SELECT tool_filter FROM sessions WHERE id=?",
                           -1, &s, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(s, 1, session_id);
    char *r = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(s, 0);
        if (t) r = strdup(t);
    }
    sqlite3_finalize(s);
    return r;
}

/* Intersection of two JSON string arrays, as a JSON array ("[]" if none). */
static char *json_intersect(sqlite3 *db, const char *a, const char *b) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT json_group_array(value) FROM json_each(?1)"
            " WHERE value IN (SELECT value FROM json_each(?2))",
            -1, &s, NULL) != SQLITE_OK)
        return strdup("[]");
    sqlite3_bind_text(s, 1, a, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, b, -1, SQLITE_STATIC);
    char *r = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(s, 0);
        if (t) r = strdup(t);
    }
    sqlite3_finalize(s);
    return r ? r : strdup("[]");
}

static char *query_text(sqlite3 *db, const char *sql, int64_t session_id) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int64(s, 1, session_id);
    char *r = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(s, 0);
        if (t) r = strdup(t);
    }
    sqlite3_finalize(s);
    return r;
}

static char *query_tools(sqlite3 *db, const char *sql,
                         const char *agent_name, const char *allowed_tools) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return NULL;
    if (agent_name) sqlite3_bind_text(s, 1, agent_name, -1, SQLITE_STATIC);
    else sqlite3_bind_null(s, 1);
    if (allowed_tools) sqlite3_bind_text(s, 2, allowed_tools, -1, SQLITE_STATIC);
    else sqlite3_bind_null(s, 2);
    char *r = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(s, 0);
        if (t) r = strdup(t);
    }
    sqlite3_finalize(s);
    return r;
}

/* ── Public API ────────────────────────────────────────────────── */

int llm_build_payload(sqlite3 *db, int64_t session_id, const Config *cfg,
                      const ContextPlan *plan, const char *recall_text,
                      const char *system_prompt, LlmPayload *out) {
    if (!db || !cfg || !plan || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->db = db;

    int gemini = (cfg->provider.endpoint_type == ENDPOINT_GEMINI);
    if (populate_plan(db, plan) != 0) return -1;

    /* Effective tool scope = grants ∩ session tool_filter. An agent with an
     * empty grant set keeps "[]" (sees zero tools) — only a NULL agent is
     * unrestricted. The filter (frozen at spawn for workers) can only narrow;
     * dispatch_tool re-checks both, so this is advisory for the model. */
    char *agent_name = session_get_agent_name(db, session_id);
    char *allowed_tools = agent_name ? grants_json(db, agent_name, "tool") : NULL;
    char *tool_filter = session_tool_filter(db, session_id);
    if (tool_filter) {
        if (allowed_tools) {
            char *isect = json_intersect(db, allowed_tools, tool_filter);
            free(allowed_tools);
            allowed_tools = isect;
            free(tool_filter);
        } else {
            allowed_tools = tool_filter;
        }
        tool_filter = NULL;
    }

    if (gemini) {
        char *contents = query_text(db, SQL_GEMINI_CONTENTS, session_id);
        char *tools_json = query_tools(db, SQL_GEMINI_TOOLS, agent_name, allowed_tools);

        sqlite3_stmt *full;
        if (sqlite3_prepare_v2(db, SQL_GEMINI_FULL, -1, &full, NULL) != SQLITE_OK) {
            free(contents); free(tools_json);
            free(agent_name); free(allowed_tools);
            return -1;
        }
        if (system_prompt && system_prompt[0])
            sqlite3_bind_text(full, 1, system_prompt, -1, SQLITE_STATIC);
        else sqlite3_bind_null(full, 1);
        sqlite3_bind_text(full, 2, contents ? contents : "[]", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(full, 3, tools_json ? tools_json : "[]", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(full, 4, cfg->provider.max_tokens);
        if (recall_text && recall_text[0])
            sqlite3_bind_text(full, 5, recall_text, -1, SQLITE_STATIC);
        else sqlite3_bind_null(full, 5);

        free(contents); free(tools_json);

        if (sqlite3_step(full) == SQLITE_ROW) {
            out->stmt = full;
            out->body = (const char *)sqlite3_column_text(full, 0);
        } else {
            sqlite3_finalize(full);
            free(agent_name); free(allowed_tools);
            return -1;
        }
    } else {
        char *messages = query_text(db, SQL_OPENAI_MESSAGES, session_id);
        if (!messages) messages = strdup("[]");
        char *tools_json = query_tools(db, SQL_OPENAI_TOOLS, agent_name, allowed_tools);

        sqlite3_stmt *full;
        if (sqlite3_prepare_v2(db, SQL_OPENAI_FULL, -1, &full, NULL) != SQLITE_OK) {
            free(messages); free(tools_json);
            free(agent_name); free(allowed_tools);
            return -1;
        }
        sqlite3_bind_text(full, 1, cfg->provider.model ? cfg->provider.model : "unknown",
                          -1, SQLITE_STATIC);
        if (system_prompt && system_prompt[0])
            sqlite3_bind_text(full, 2, system_prompt, -1, SQLITE_STATIC);
        else sqlite3_bind_null(full, 2);
        sqlite3_bind_text(full, 3, messages, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(full, 4, cfg->provider.max_tokens);
        sqlite3_bind_text(full, 5, tools_json ? tools_json : "[]", -1, SQLITE_TRANSIENT);
        if (recall_text && recall_text[0])
            sqlite3_bind_text(full, 6, recall_text, -1, SQLITE_STATIC);
        else sqlite3_bind_null(full, 6);

        free(messages); free(tools_json);

        if (sqlite3_step(full) == SQLITE_ROW) {
            out->stmt = full;
            out->body = (const char *)sqlite3_column_text(full, 0);
        } else {
            sqlite3_finalize(full);
            free(agent_name); free(allowed_tools);
            return -1;
        }
    }

    free(agent_name); free(allowed_tools);
    return 0;
}

void llm_payload_release(LlmPayload *p) {
    if (!p) return;
    if (p->stmt) { sqlite3_finalize(p->stmt); p->stmt = NULL; }
    if (p->db) { sqlite3_exec(p->db, "DROP TABLE IF EXISTS _plan;", NULL, NULL, NULL); }
    p->body = NULL;
}
