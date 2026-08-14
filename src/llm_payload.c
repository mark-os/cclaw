#define _POSIX_C_SOURCE 200809L
#include "llm_payload.h"
#include "agent_config.h"
#include "config_registry.h"
#include "context.h"
#include "config.h"
#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* A SQL query emits the LLM request JSON — on purpose. The entries table
 * already holds every message; json_object()/json_group_array() over it *is*
 * the serializer, returned zero-copy from the open statement. Rewriting this
 * as a C JSON builder would add allocation, escaping, and drift risk for no
 * gain. See AGENTS.md "Counterintuitive on purpose". */

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

/* A cron_result is a scheduled script's output posted into the session with no
 * LLM call behind it. It rides the assistant role (so ordinary delivery finds
 * it), which is exactly why it must be labelled here: unlabelled, the model
 * reads a machine's stdout back as its own words. */
#define SQL_CRON_RESULT_TEXT \
    "'[scheduled script ' || COALESCE(json_extract(e.data,'$.job'),'?') ||" \
    " CASE WHEN e.is_error THEN ' failed]' ELSE ' output]' END ||" \
    " char(10) || COALESCE(e.content,'')"

static const char SQL_OPENAI_MESSAGES[] =
    "SELECT json_group_array(json(msg) ORDER BY pos) FROM ("
    "  SELECT p.pos,"
    "    CASE e.type"
    "      WHEN 'system' THEN"
    "        json_object('role','system','content',COALESCE(e.content,''))"
    "      WHEN 'compaction' THEN"
    "        json_object('role','system','content',"
    "          '[Summary of earlier conversation]' || char(10) || COALESCE(e.content,''))"
    "      WHEN 'user_message' THEN"
    "        json_object('role','user','content',COALESCE(e.content,''))"
    "      WHEN 'assistant_message' THEN"
    "        CASE WHEN EXISTS("
    "          SELECT 1 FROM entries tc WHERE tc.session_id=e.session_id"
    "            AND tc.iteration_id=e.iteration_id AND tc.type='tool_call')"
    "        THEN json_patch("
    "          json_object('role','assistant','content',e.content),"
    "          json_object('tool_calls',"
    "            (SELECT json_group_array("
    "              json_object('id',tc2.tool_call_id,'type','function',"
    "                'function',json_object('name',tc2.tool_name,"
    "                  'arguments',COALESCE(tc2.content,'{}'))))"
    "             FROM entries tc2 WHERE tc2.session_id=e.session_id"
    "               AND tc2.iteration_id=e.iteration_id AND tc2.type='tool_call'"
    "             ORDER BY tc2.part_index)))"
    "        ELSE json_object('role','assistant','content',e.content)"
    "        END"
    "      WHEN 'tool_result' THEN"
    "        json_object('role','tool','tool_call_id',e.tool_call_id,"
    "                    'content'," SQL_WRAP_TOOL_CONTENT ")"
    "      WHEN 'cron_result' THEN"
    "        json_object('role','assistant','content'," SQL_CRON_RESULT_TEXT ")"
    "      ELSE NULL"
    "    END AS msg"
    "  FROM _plan p JOIN entries e ON e.id = p.entry_id AND e.session_id = ?1"
    "  WHERE e.type NOT IN ('tool_call','reasoning')"
    /* Session context (recall + live state) rides at the turn boundary:
     * right before the newest user_message. The user's actual message stays
     * the true tail on iteration 0 (recency weighting), tool-loop iterations
     * never split an assistant tool_calls message from its tool result
     * (strict providers 400 on non-adjacency), and the block's position —
     * like its content, frozen per turn in llm_proc.c — never moves
     * mid-turn, keeping the request prefix byte-stable for prompt caching.
     * A plan with no user_message anchors before its first entry instead,
     * which is equally adjacency-safe. */
    "  UNION ALL"
    "  SELECT COALESCE("
    "      (SELECT MAX(pu.pos) FROM _plan pu JOIN entries eu"
    "         ON eu.id = pu.entry_id AND eu.type = 'user_message'),"
    "      (SELECT MIN(pp.pos) FROM _plan pp)) - 0.5 AS pos,"
    "    json_object('role','user','content',?2) AS msg"
    "  WHERE ?2 IS NOT NULL AND EXISTS (SELECT 1 FROM _plan)"
    /* Ephemeral hook injects ride at the absolute tail, after even the
     * newest entry — orthogonal to session context, unaffected by it. */
    "  UNION ALL"
    "  SELECT 1000000+hd.id AS pos,"
    "    json_object('role', hd.role, 'content', hd.content) AS msg"
    "  FROM hook_directives hd WHERE hd.session_id = ?1 AND hd.kind = 'inject'"
    ") sub WHERE msg IS NOT NULL;";

/* launch_agent's description carries a live roster of delegable agents and the
 * live default worker toolset (?3), both recomputed every request — no refresh
 * plumbing when agents or config change. The toolset is spelled out rather
 * than named ("the worker_tools config"): a spawned worker cannot read config
 * from inside its sandbox, and a spawner that can't see the list has no way to
 * know its worker will be missing a tool. */
#define SQL_TOOL_DESCRIPTION \
    "CASE WHEN t.name='launch_agent' THEN" \
    "  t.description || COALESCE(' Available agents: ' ||" \
    "    (SELECT group_concat(a.name || ' — ' ||" \
    "       COALESCE(a.description,'(no description)'), '; ') FROM agents a), '')" \
    "  || COALESCE(' Default worker toolset (self-spawn with no ''tools''): '" \
    "       || ?3 || '.', '')" \
    " ELSE t.description END"

static const char SQL_OPENAI_TOOLS[] =
    "SELECT json_group_array("
    "  json_object('type','function','function',"
    "    json_object('name',t.name,'description'," SQL_TOOL_DESCRIPTION ","
    "      'parameters',json(t.parameters_json)))"
    ") FROM tools t"
    " WHERE t.enabled=1"
    "   AND (t.agent_name IS NULL OR t.agent_name=?1)"
    /* Shown ≡ granted: grants are the single authorization currency (D1) —
     * extension_install seeds the owner's tool grants, so no attachment
     * OR-branch is needed here. */
    "   AND (?2 IS NULL OR t.name IN (SELECT value FROM json_each(?2)));";

/* Both FULL templates wrap json_object in json_patch('{}', ...): RFC 7386
 * merge drops top-level NULL-valued keys, which json_object would otherwise
 * emit as JSON null ("max_tokens":null etc.) — strict providers 400 on those. */
static const char SQL_OPENAI_FULL[] =
    "SELECT json_patch('{}', json_object("
    "  'model', ?1,"
    "  'messages', (SELECT json_group_array(json(m)) FROM ("
    /* ?6 = this session's prompt suffix: location line + the pinned route's
     * system_prompt_suffix (NULL when neither applies) — assembled in SQL by
     * route_prompt_suffix(), appended here rather than glued together in C. */
    "    SELECT 0 AS ord, 0 AS sub, json_object('role','system','content',"
    "      COALESCE(?2,'') || COALESCE(char(10) || char(10) || ?6, '')) AS m"
    "      WHERE ?2 IS NOT NULL OR ?6 IS NOT NULL"
    "    UNION ALL"
    /* ?3 (SQL_OPENAI_MESSAGES) already carries session context in the right
     * spot — at the turn boundary, before the newest user_message — so it
     * needs no special-casing here. */
    "    SELECT 1, rowid, json(value) FROM json_each(?3)"
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
    "            WHERE e2.iteration_id=e.iteration_id"
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
    "      WHEN e.type = 'cron_result' THEN"
    "        json_object('role','model','parts',"
    "          json_array(json_object('text'," SQL_CRON_RESULT_TEXT ")))"
    "      ELSE NULL"
    "    END AS content_obj"
    "  FROM _plan p JOIN entries e ON e.id = p.entry_id AND e.session_id = ?1"
    "  WHERE e.type != 'system'"
    /* -iteration_id vs id: both are always positive, so negating iteration_id
     * puts the two group-key domains in disjoint ranges. Without this, a
     * user_message with id=N and an assistant/tool_call/reasoning entry with
     * iteration_id=N collide into one GROUP BY bucket — silently merging
     * unrelated messages and dropping one (hits fresh/short sessions, where
     * id is still small). */
    "  GROUP BY CASE WHEN e.type IN ('assistant_message','tool_call','reasoning')"
    "    THEN -e.iteration_id ELSE e.id END"
    /* Session context (recall + live state) at the turn boundary: right
     * before the newest user_message — see SQL_OPENAI_MESSAGES for
     * rationale (tail recency, functionCall/functionResponse adjacency,
     * position stability for prompt caching). */
    "  UNION ALL"
    "  SELECT COALESCE("
    "      (SELECT MAX(pu.pos) FROM _plan pu JOIN entries eu"
    "         ON eu.id = pu.entry_id AND eu.type = 'user_message'),"
    "      (SELECT MIN(pp.pos) FROM _plan pp)) - 0.5 AS min_pos,"
    "    json_object('role','user','parts',"
    "      json_array(json_object('text',?2))) AS content_obj"
    "  WHERE ?2 IS NOT NULL AND EXISTS (SELECT 1 FROM _plan)"
    /* Ephemeral hook injects: Gemini has no system role mid-contents, so a
     * system inject becomes a '[system] '-prefixed user part at absolute
     * tail, after even the newest entry. */
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
    /* ?5 = this session's prompt suffix — see SQL_OPENAI_FULL. */
    "  'systemInstruction', CASE WHEN COALESCE(?1,'') ||"
    "      COALESCE(char(10) || char(10) || ?5, '') != ''"
    "    THEN json_object('parts',json_array(json_object('text',"
    "      COALESCE(?1,'') || COALESCE(char(10) || char(10) || ?5, ''))))"
    "    ELSE NULL END,"
    /* ?2 (SQL_GEMINI_CONTENTS) already carries session context in the right
     * spot — at the turn boundary, before the newest user_message. */
    "  'contents', json(?2),"
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

/* Everything appended to the system prompt for this session: the built-in
 * location line (where the bot is chatting) followed by the pinned route's
 * system_prompt_suffix, or NULL when neither applies. Assembled in SQL, like
 * the rest of this file. Read every turn (not frozen at session creation like
 * tool_filter): it is prompt text, not authority, so a renamed chat or an
 * edited route shows up on the next turn. The location line needs both
 * channel_name and chat_id — NULL in either propagates through the ||
 * concatenation and COALESCE drops the whole line. */
static char *route_prompt_suffix(sqlite3 *db, int64_t session_id) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT COALESCE(loc || COALESCE(char(10) || char(10) || rt, ''), rt)"
            " FROM (SELECT"
            "   'You are chatting in '"
            "     || COALESCE(se.chat_title, 'chat ' || se.chat_id)"
            "     || ' (channel ' || se.channel_name"
            "     || ', chat id ' || se.chat_id || ').' AS loc,"
            "   (SELECT r.system_prompt_suffix FROM channel_routes r"
            "     WHERE r.session_id=se.id AND r.system_prompt_suffix IS NOT NULL"
            "     LIMIT 1) AS rt"
            "  FROM sessions se WHERE se.id=?)",
            -1, &s, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(s, 1, session_id);
    char *r = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(s, 0);
        if (t && t[0]) r = strdup(t);
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

/* context_text: session context (recall + live state), or NULL. Used by the
 * messages/contents builders, which bind it at ?2 alongside session_id (?1). */
static char *query_text(sqlite3 *db, const char *sql, int64_t session_id,
                        const char *context_text) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int64(s, 1, session_id);
    if (context_text && context_text[0])
        sqlite3_bind_text(s, 2, context_text, -1, SQLITE_STATIC);
    else sqlite3_bind_null(s, 2);
    char *r = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(s, 0);
        if (t) r = strdup(t);
    }
    sqlite3_finalize(s);
    return r;
}

/* The worker_tools config as a human-readable list, for launch_agent's
 * description. Config-sourced, so byte-stable across a turn's iterations.
 * A malformed value (operator typo) makes json_each error at step time and
 * we return NULL — the sentence is dropped, the payload still builds. */
static char *worker_tools_list(sqlite3 *db) {
    char *raw = config_get(db, "worker_tools");
    if (!raw || !raw[0]) { free(raw); return NULL; }
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, "SELECT group_concat(value, ', ') FROM json_each(?1)",
                           -1, &s, NULL) != SQLITE_OK) { free(raw); return NULL; }
    sqlite3_bind_text(s, 1, raw, -1, SQLITE_STATIC);
    char *r = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(s, 0);
        if (t && t[0]) r = strdup(t);
    }
    sqlite3_finalize(s);
    free(raw);
    return r;
}

static char *query_tools(sqlite3 *db, const char *sql, const char *agent_name,
                         const char *allowed_tools, const char *worker_tools) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return NULL;
    if (agent_name) sqlite3_bind_text(s, 1, agent_name, -1, SQLITE_STATIC);
    else sqlite3_bind_null(s, 1);
    if (allowed_tools) sqlite3_bind_text(s, 2, allowed_tools, -1, SQLITE_STATIC);
    else sqlite3_bind_null(s, 2);
    if (worker_tools) sqlite3_bind_text(s, 3, worker_tools, -1, SQLITE_STATIC);
    else sqlite3_bind_null(s, 3);
    char *r = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(s, 0);
        if (t) r = strdup(t);
    }
    sqlite3_finalize(s);
    return r;
}

/* The whole <RELEVANT_CONTEXT> block in one query: the wall clock (?3, built
 * in C — SQLite's strftime has no zone-name/offset format), recall (?2,
 * computed in C by context_auto_recall — the only piece not natively SQL)
 * plus three live-state sections queried directly. Each optional section is
 * COALESCE'd away when empty — never an empty tag. The block itself is
 * unconditional: the clock always has something to say. */
static const char SQL_SESSION_CONTEXT[] =
    "WITH mb AS ("
    "  SELECT group_concat("
    "    char(10) || '### ' || COALESCE(b.label,'') || char(10) ||"
    "    'description: ' || COALESCE(b.description,'') || char(10) ||"
    "    COALESCE("
    "      (SELECT group_concat(e.pos || '. ' || e.text, char(10) ORDER BY e.pos) || char(10)"
    "         FROM memory_entries e WHERE e.agent_name=b.agent_name AND e.block_label=b.label),"
    "      '(no entries yet)' || char(10)"
    "    ), '' ORDER BY b.id) AS txt"
    "  FROM memory_blocks b"
    "  WHERE b.agent_name=(SELECT agent_name FROM sessions WHERE id=?1) AND b.placement='context'"
    "), sub AS ("
    "  SELECT group_concat("
    "    'session #' || s.id || ' (' || COALESCE(s.agent_name,'?') || ') — ' || s.state,"
    "    char(10)) AS txt"
    "  FROM sessions s WHERE s.parent_session_id=?1 AND s.state != 'idle'"
    "), appr AS ("
    /* Both open states: 'pending' (waiting on a human) and 'approved' (granted
     * but never consumed — the ticket is only spent when the tool call is
     * re-issued). Consumed/rejected/expired rows are closed and absent. */
    "  SELECT group_concat("
    "    '#' || a.id || ' ' || COALESCE(a.tool_name, a.action, '?') || ' — ' ||"
    "    CASE a.state"
    "      WHEN 'approved' THEN 'approved, unused: re-issue the tool call to consume'"
    "      ELSE 'pending: waiting on a human decision' END, char(10)) AS txt"
    "  FROM approvals a WHERE a.session_id=?1 AND a.state IN ('pending','approved')"
    ")"
    "SELECT '<RELEVANT_CONTEXT>' || char(10) ||"
    "    '<current_time>' || ?3 || '</current_time>' || char(10) ||"
    "    COALESCE('<recall>' || char(10) || ?2 || char(10) || '</recall>' || char(10), '') ||"
    "    COALESCE('<memory_blocks>' || mb.txt || '</memory_blocks>' || char(10), '') ||"
    "    COALESCE('<running_sub_agents>' || char(10) || sub.txt || char(10) ||"
    "      '</running_sub_agents>' || char(10), '') ||"
    "    COALESCE('<open_approvals>' || char(10) || appr.txt || char(10) ||"
    "      '</open_approvals>' || char(10), '') ||"
    "    '</RELEVANT_CONTEXT>'"
    " FROM mb, sub, appr;";

/* ── Public API ────────────────────────────────────────────────── */

/* Wall clock for the context block, stated with its zone — the model can't
 * turn "9am" into a cron_expr (which evaluates local, see cron.c) unless it
 * knows what local means here. Volatile by nature, so it lives in the
 * turn-boundary block, never in the cache-sensitive system prompt (F22). */
static void local_now_text(char *buf, size_t n) {
    time_t now = time(NULL);
    struct tm tm;
    if (!localtime_r(&now, &tm) ||
        strftime(buf, n, "%Y-%m-%d %H:%M:%S %Z (UTC%z)", &tm) == 0)
        snprintf(buf, n, "%lld (unix epoch)", (long long)now);
}

char *session_context_text(sqlite3 *db, int64_t session_id, const char *recall_text) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, SQL_SESSION_CONTEXT, -1, &st, NULL) != SQLITE_OK)
        return NULL;
    char now_buf[96];
    local_now_text(now_buf, sizeof(now_buf));
    sqlite3_bind_int64(st, 1, session_id);
    if (recall_text && recall_text[0])
        sqlite3_bind_text(st, 2, recall_text, -1, SQLITE_STATIC);
    else sqlite3_bind_null(st, 2);
    sqlite3_bind_text(st, 3, now_buf, -1, SQLITE_STATIC);
    char *r = NULL;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL) {
        const char *t = (const char *)sqlite3_column_text(st, 0);
        if (t) r = strdup(t);
    }
    sqlite3_finalize(st);
    return r;
}

int llm_build_payload(sqlite3 *db, int64_t session_id, const Config *cfg,
                      const ContextPlan *plan, const char *context_text,
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
    char *suffix = route_prompt_suffix(db, session_id);
    char *worker_tools = worker_tools_list(db);
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
        char *contents = query_text(db, SQL_GEMINI_CONTENTS, session_id, context_text);
        char *tools_json = query_tools(db, SQL_GEMINI_TOOLS, agent_name,
                                       allowed_tools, worker_tools);

        sqlite3_stmt *full;
        if (sqlite3_prepare_v2(db, SQL_GEMINI_FULL, -1, &full, NULL) != SQLITE_OK) {
            free(contents); free(tools_json);
            free(agent_name); free(allowed_tools); free(suffix); free(worker_tools);
            return -1;
        }
        if (system_prompt && system_prompt[0])
            sqlite3_bind_text(full, 1, system_prompt, -1, SQLITE_STATIC);
        else sqlite3_bind_null(full, 1);
        sqlite3_bind_text(full, 2, contents ? contents : "[]", -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(full, 3, tools_json ? tools_json : "[]", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(full, 4, cfg->provider.max_tokens);
        if (suffix) sqlite3_bind_text(full, 5, suffix, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(full, 5);

        free(contents); free(tools_json);

        if (sqlite3_step(full) == SQLITE_ROW) {
            out->stmt = full;
            out->body = (const char *)sqlite3_column_text(full, 0);
        } else {
            sqlite3_finalize(full);
            free(agent_name); free(allowed_tools); free(suffix); free(worker_tools);
            return -1;
        }
    } else {
        char *messages = query_text(db, SQL_OPENAI_MESSAGES, session_id, context_text);
        if (!messages) messages = strdup("[]");
        char *tools_json = query_tools(db, SQL_OPENAI_TOOLS, agent_name,
                                       allowed_tools, worker_tools);

        sqlite3_stmt *full;
        if (sqlite3_prepare_v2(db, SQL_OPENAI_FULL, -1, &full, NULL) != SQLITE_OK) {
            free(messages); free(tools_json);
            free(agent_name); free(allowed_tools); free(suffix); free(worker_tools);
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
        if (suffix) sqlite3_bind_text(full, 6, suffix, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(full, 6);

        free(messages); free(tools_json);

        if (sqlite3_step(full) == SQLITE_ROW) {
            out->stmt = full;
            out->body = (const char *)sqlite3_column_text(full, 0);
        } else {
            sqlite3_finalize(full);
            free(agent_name); free(allowed_tools); free(suffix); free(worker_tools);
            return -1;
        }
    }

    free(agent_name); free(allowed_tools); free(suffix); free(worker_tools);
    return 0;
}

void llm_payload_release(LlmPayload *p) {
    if (!p) return;
    if (p->stmt) { sqlite3_finalize(p->stmt); p->stmt = NULL; }
    if (p->db) { sqlite3_exec(p->db, "DROP TABLE IF EXISTS _plan;", NULL, NULL, NULL); }
    p->body = NULL;
}
