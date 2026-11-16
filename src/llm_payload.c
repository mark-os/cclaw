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
    /* The index answers "is this entry on the plan?" for the transcript's
     * reasoning lookup without a scan of the plan per iteration. */
    sqlite3_exec(db,
        "DROP TABLE IF EXISTS _plan;"
        "CREATE TEMP TABLE _plan(pos INTEGER PRIMARY KEY, entry_id INTEGER NOT NULL);"
        "CREATE INDEX temp._plan_entry ON _plan(entry_id);",
        NULL, NULL, NULL);
    sqlite3_stmt *ins;
    if (sqlite3_prepare_v2(db, "INSERT INTO _plan(pos,entry_id) VALUES(?,?);",
                           -1, &ins, NULL) != SQLITE_OK)
        return -1;
    int pos = 0;
    for (int i = plan->cut; i < plan->count; i++) {
        sqlite3_bind_int(ins, 1, pos++);
        sqlite3_bind_int64(ins, 2, plan->entries[i].id);
        sqlite3_step(ins);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    return 0;
}

/* ── The transcript ────────────────────────────────────────────────
 *
 * Everything the three wire formats agree on is decided once, here: which
 * entries are sent, what role each speaks in, how its text is decorated,
 * which rows belong to the same iteration, and where the user context block
 * sits. The provider queries below are projections of this table into a wire
 * shape; the only other thing they read is the reasoning blob a row points
 * at, so a row that is not on the plan cannot reach any provider.
 *
 * One row per part:
 *   pos    2 × plan position. The odd slots are free; the user context block
 *          takes the one before its anchor.
 *   grp    rows that make one message where a format groups: an iteration's
 *          assistant text and tool calls share a key, that iteration's tool
 *          results share another, every other row has its own. Three disjoint
 *          ranges — negative odd, negative even, entry ids (positive; 0 is
 *          the user context block) — so a user_message with id N can never
 *          land in the bucket of iteration N.
 *   role   'user' | 'assistant' | 'tool' — neutral; each format spells it.
 *   kind   'text' | 'tool_call' | 'tool_result'.
 *   text   decorated and ready to send (NULL on a tool call, and on an
 *          assistant message that had none).
 *   reasoning_id   on an iteration's assistant rows: the reasoning entry of
 *          that iteration, if it is on the plan. Reasoning is a property of
 *          the assistant message, never a row of its own, and an iteration
 *          whose reasoning entry was taken off the branch replays nothing.
 *
 * A temp table rather than a view or a shared CTE: a view cannot take the
 * bound context text, and a CTE pasted into three queries would be
 * materialized by SQLite anyway wherever it is read twice — the same copy,
 * made out of sight. The request body is still returned zero-copy: the
 * statement that stays open (SQL_*_FULL) reads neither temp table. */

static const char SQL_TRANSCRIPT_CREATE[] =
    "DROP TABLE IF EXISTS _transcript;"
    "CREATE TEMP TABLE _transcript("
    "  pos INTEGER PRIMARY KEY, grp INTEGER NOT NULL,"
    "  role TEXT NOT NULL, kind TEXT NOT NULL, text TEXT,"
    "  call_id TEXT, tool_name TEXT, args TEXT, is_error INTEGER NOT NULL,"
    "  entry_id INTEGER, iteration_id INTEGER, turn_id INTEGER,"
    "  reasoning_id INTEGER);";

/* Query-time untrusted-content wrap: a tool_result whose entry carries a
 * network_hosts tag came (at least partly) from the network. Both tags carry
 * entries.fence_id, a random id minted when the entry was tagged — after the
 * content was already written, so nothing inside it can name the id and close
 * the block early. That is the whole defence: content is not scrubbed for
 * tag lookalikes. Stored, not generated here, so the request prefix stays
 * byte-stable for prompt caching. */
#define SQL_WRAP_TOOL_CONTENT \
    "CASE WHEN e.network_hosts IS NOT NULL THEN" \
    "  '<UNTRUSTED_EXTERNAL_CONTENT id=\"' || COALESCE(e.fence_id,'') || '\">' || char(10) ||" \
    "  'Contacted hosts: ' || e.network_hosts || char(10) || '---' || char(10) ||" \
    "  COALESCE(e.content,'') || char(10) ||" \
    "  '</UNTRUSTED_EXTERNAL_CONTENT id=\"' || COALESCE(e.fence_id,'') || '\">'" \
    " ELSE COALESCE(e.content,'') END"

/* A cron_result is a scheduled script's output posted into the session with no
 * LLM call behind it. It rides the assistant role (so ordinary delivery finds
 * it), which is exactly why it must be labelled here: unlabelled, the model
 * reads a machine's stdout back as its own words. */
#define SQL_CRON_RESULT_TEXT \
    "'[scheduled script ' || COALESCE(json_extract(e.data,'$.job'),'?') ||" \
    " CASE WHEN e.is_error THEN ' failed]' ELSE ' output]' END ||" \
    " char(10) || COALESCE(e.content,'')"

/* ?1 = session id. Entry types not named in the WHERE are not sent. */
static const char SQL_TRANSCRIPT_PARTS[] =
    "INSERT INTO _transcript"
    " SELECT 2*p.pos,"
    /* An entry with no iteration_id stands alone rather than pooling with
     * every other such entry. */
    "  COALESCE(CASE"
    "    WHEN e.type IN ('assistant_message','tool_call') THEN -2*e.iteration_id - 1"
    "    WHEN e.type = 'tool_result' THEN -2*e.iteration_id - 2 END, e.id),"
    "  CASE WHEN e.type IN ('assistant_message','tool_call','cron_result') THEN 'assistant'"
    "       WHEN e.type = 'tool_result' THEN 'tool' ELSE 'user' END,"
    "  CASE WHEN e.type IN ('tool_call','tool_result') THEN e.type ELSE 'text' END,"
    "  CASE e.type"
    /* No format carries a non-leading system role: no provider documents
     * mid-array system messages (OpenRouter rewrites them per-upstream,
     * Anthropic/Gemini structurally can't carry them), and a tail-append is
     * cache-safe whatever role it wears. Mid-conversation system entries
     * therefore ride as '[system] '-prefixed user text everywhere; only the
     * leading system prompt (SQL_*_FULL) keeps the system role. */
    "    WHEN 'system' THEN '[system] ' || COALESCE(e.content,'')"
    "    WHEN 'compaction' THEN"
    "      '[Summary of earlier conversation]' || char(10) || COALESCE(e.content,'')"
    "    WHEN 'user_message' THEN COALESCE(e.content,'')"
    "    WHEN 'assistant_message' THEN e.content"
    "    WHEN 'cron_result' THEN " SQL_CRON_RESULT_TEXT
    "    WHEN 'tool_result' THEN " SQL_WRAP_TOOL_CONTENT
    "  END,"
    "  CASE WHEN e.type IN ('tool_call','tool_result') THEN e.tool_call_id END,"
    "  CASE WHEN e.type IN ('tool_call','tool_result') THEN e.tool_name END,"
    "  CASE WHEN e.type = 'tool_call' THEN COALESCE(e.content,'{}') END,"
    "  e.is_error, e.id, e.iteration_id, e.turn_id,"
    "  CASE WHEN e.type IN ('assistant_message','tool_call') THEN"
    "    (SELECT r.id FROM entries r"
    "      WHERE r.session_id=e.session_id AND r.iteration_id=e.iteration_id"
    "        AND r.type='reasoning' AND r.id IN (SELECT entry_id FROM _plan)"
    "      LIMIT 1) END"
    " FROM _plan p JOIN entries e ON e.id = p.entry_id AND e.session_id = ?1"
    " WHERE e.type IN ('system','compaction','user_message','assistant_message',"
    "                  'cron_result','tool_call','tool_result');";

/* The user context block (clock, recall, live state) rides at the turn
 * boundary: right before the first entry of the newest turn on the plan. A
 * turn can open with several queued user messages, and the block goes before
 * all of them, so the messages being answered stay the true tail on iteration
 * 1 (recency weighting). Tool-loop iterations never split an assistant's tool
 * calls from their results (strict providers 400 on non-adjacency), and the
 * block's position — like its content, frozen per turn in llm_proc.c — never
 * moves mid-turn, keeping the request prefix byte-stable for prompt caching.
 * A compaction summary inherits the turn of the entry it follows, and a
 * cron_result the turn it lands in, so neither opens a turn or moves the
 * block. When no entry carries a turn_id the block goes before the first
 * entry, which is equally adjacency-safe. ?1 = the block's text, or NULL. */
static const char SQL_TRANSCRIPT_CONTEXT[] =
    "INSERT INTO _transcript(pos, grp, role, kind, text, is_error)"
    " SELECT COALESCE("
    "    (SELECT MIN(pos) FROM _transcript"
    "      WHERE turn_id = (SELECT MAX(turn_id) FROM _transcript)),"
    "    (SELECT MIN(pos) FROM _transcript)) - 1,"
    "  0, 'user', 'text', ?1, 0"
    " WHERE ?1 IS NOT NULL AND EXISTS (SELECT 1 FROM _transcript);";

/* Run one statement with ?1 bound to text when given, else to id. */
static int run_bound(sqlite3 *db, const char *sql, int64_t id, const char *text) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    if (text) sqlite3_bind_text(s, 1, text, -1, SQLITE_STATIC);
    else sqlite3_bind_int64(s, 1, id);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* user_context: the user context block's text, or NULL/"" for none. */
static int build_transcript(sqlite3 *db, int64_t session_id, const ContextPlan *plan,
                            const char *user_context) {
    if (populate_plan(db, plan) != 0) return -1;
    if (sqlite3_exec(db, SQL_TRANSCRIPT_CREATE, NULL, NULL, NULL) != SQLITE_OK) return -1;
    if (run_bound(db, SQL_TRANSCRIPT_PARTS, session_id, NULL) != 0) return -1;
    if (!user_context || !user_context[0]) return 0;
    return run_bound(db, SQL_TRANSCRIPT_CONTEXT, 0, user_context);
}

/* ── Wire projections ──────────────────────────────────────────────
 *
 * Each *_MESSAGES / *_CONTENTS query reads _transcript and takes one
 * parameter, ?1 = the requesting model.
 *
 * Reasoning replay is model-bound (pi's rule): the join below finds a row
 * only when the iteration's reasoning entry is on the plan and was produced
 * by the model being called now, so switching models strips the replay for
 * free. Entries with no captured blob (pre-v48, or a provider that sent none)
 * likewise yield nothing: never invent a reasoning field. */
#define SQL_REPLAY_JOIN \
    " LEFT JOIN entries r ON r.id = t.reasoning_id" \
    "   AND json_extract(r.reasoning_meta,'$.model') = ?1"

/* An iteration's assistant rows are one message; everything else, tool
 * results included, is a message per row. Within a group `role` is the same
 * on every row, and a group of one is the only place a bare text/call_id
 * column is read. */
#define SQL_GROUP_RESULTS_APART \
    " GROUP BY t.grp, CASE WHEN t.kind = 'tool_result' THEN t.pos END"

/* Reasoning replay (OpenAI-compat). The provider wants its own reasoning
 * artifact back on the assistant message that carried the tool calls —
 * OpenRouter's reasoning_details array verbatim (order and every field matter:
 * it is how Gemini's thoughtSignature survives the OpenAI-shaped envelope),
 * or DeepSeek's bare reasoning_content string (documented 400 / a silently
 * invalidated cache prefix when it goes missing after a tool-call turn). */
#define SQL_OPENAI_REASONING \
    "CASE json_extract(r.reasoning_meta,'$.format')" \
    "  WHEN 'reasoning_details' THEN" \
    "    json_object('reasoning_details'," \
    "      json(json_extract(r.reasoning_meta,'$.blob')))" \
    "  WHEN 'reasoning_content' THEN" \
    "    json_object('reasoning_content'," \
    "      json_extract(r.reasoning_meta,'$.blob')) END"

static const char SQL_OPENAI_MESSAGES[] =
    "SELECT json_group_array(json(msg) ORDER BY pos) FROM ("
    "  SELECT MIN(t.pos) AS pos,"
    "    CASE t.role"
    "      WHEN 'user' THEN json_object('role','user','content',t.text)"
    "      WHEN 'tool' THEN json_object('role','tool','tool_call_id',t.call_id,"
    "                                   'content',t.text)"
    "      ELSE json_patch(json_patch("
    "        json_object('role','assistant',"
    "          'content',MAX(t.text) FILTER (WHERE t.kind = 'text')),"
    "        CASE WHEN COUNT(*) FILTER (WHERE t.kind = 'tool_call') > 0 THEN"
    "          json_object('tool_calls', json_group_array("
    "            json_object('id',t.call_id,'type','function',"
    "              'function',json_object('name',t.tool_name,'arguments',t.args))"
    "            ORDER BY t.pos) FILTER (WHERE t.kind = 'tool_call'))"
    "        ELSE '{}' END),"
    "        COALESCE(MAX(" SQL_OPENAI_REASONING "),'{}'))"
    "    END AS msg"
    "  FROM _transcript t" SQL_REPLAY_JOIN
       SQL_GROUP_RESULTS_APART
    ");";

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

/* Provider escape hatch: providers.request_extra (JSON) is merged into the
 * finished body as the very last step, so it can override anything cclaw
 * built — that's the point (decision 6). NULL column / unknown provider =
 * merge of '{}', which RFC 7386 defines as leaving the target untouched, so
 * the default payload stays byte-identical. Bound at ?7 (OpenAI) / ?6
 * (Gemini) as the provider name; the lookup itself is a subquery. */
#define SQL_REQUEST_EXTRA(param) \
    "COALESCE((SELECT request_extra FROM providers WHERE name=" param \
    "          AND json_valid(request_extra)), '{}')"

/* Both FULL templates wrap json_object in json_patch('{}', ...): RFC 7386
 * merge drops top-level NULL-valued keys, which json_object would otherwise
 * emit as JSON null ("max_tokens":null etc.) — strict providers 400 on those. */
static const char SQL_OPENAI_FULL[] =
    "SELECT json_patch(json_patch(json_patch('{}', json_object("
    "  'model', ?1,"
    "  'messages', (SELECT json_group_array(json(m)) FROM ("
    /* ?6 = this session's chat suffix: location line, reply line and the
     * chat's guidance (NULL for a session with no chat) — assembled in SQL by
     * chat_prompt_suffix(), appended here rather than glued together in C. */
    "    SELECT 0 AS ord, 0 AS sub, json_object('role','system','content',"
    "      COALESCE(?2,'') || COALESCE(char(10) || char(10) || ?6, '')) AS m"
    "      WHERE ?2 IS NOT NULL OR ?6 IS NOT NULL"
    "    UNION ALL"
    /* ?3 (SQL_OPENAI_MESSAGES) already carries the user context block in the
     * right spot — the transcript put it at the turn boundary — so it needs
     * no special-casing here. */
    "    SELECT 1, rowid, json(value) FROM json_each(?3)"
    "    ORDER BY ord, sub)),"
    "  'max_tokens', CASE WHEN ?4 > 0 THEN ?4 ELSE NULL END,"
    "  'tools', CASE WHEN json_array_length(?5) > 0 THEN json(?5) ELSE NULL END"
    /* ?8 = the reasoning-effort fragment ('{}' when nothing to say), merged
     * BEFORE request_extra so the provider escape hatch still wins. */
    ")), json(?8)), " SQL_REQUEST_EXTRA("?7") ");";

static const char SQL_GEMINI_CONTENTS[] =
    "SELECT json_group_array(json(content_obj) ORDER BY pos) FROM ("
    "  SELECT MIN(t.pos) AS pos,"
    "    CASE t.role"
    "      WHEN 'assistant' THEN"
    "        json_object('role','model','parts', json_group_array(json("
    "          CASE t.kind WHEN 'tool_call' THEN"
    /* Google requires the thoughtSignature it issued back on the replayed
     * functionCall part; without it a thinking model 400s the request. The
     * signature is matched by tool name (part_index counts only the parts we
     * kept, so it can't index back into the wire parts array), and only when
     * the producing model is the one we're calling now. No captured
     * signature → the part goes out bare. */
    "            json_patch("
    "              json_object('functionCall',json_object('name',t.tool_name,"
    "                'args',CASE WHEN json_valid(t.args) THEN json(t.args)"
    "                         ELSE json('{}') END)),"
    "              COALESCE((SELECT json_object('thoughtSignature',"
    "                          json_extract(sg.value,'$.sig'))"
    "                 FROM json_each(json_extract(r.reasoning_meta,'$.blob')) sg"
    "                 WHERE json_extract(r.reasoning_meta,'$.format')='gemini_parts'"
    "                   AND json_extract(sg.value,'$.fn')=t.tool_name"
    "                 LIMIT 1),'{}'))"
    "          ELSE json_object('text',t.text) END)"
    "          ORDER BY t.pos)"
    "          FILTER (WHERE t.kind = 'tool_call' OR COALESCE(t.text,'') != ''))"
    "      WHEN 'tool' THEN"
    "        json_object('role','user','parts',"
    "          json_array(json_object('functionResponse',"
    "            json_object('name',COALESCE(t.tool_name,t.call_id),"
    "              'response',json_object('content',t.text)))))"
    "      ELSE json_object('role','user','parts',"
    "             json_array(json_object('text',t.text)))"
    "    END AS content_obj"
    "  FROM _transcript t" SQL_REPLAY_JOIN
       SQL_GROUP_RESULTS_APART
    ");";

static const char SQL_GEMINI_TOOLS[] =
    "SELECT json_array(json_object('functionDeclarations',"
    "  (SELECT json_group_array("
    "    json_object('name',t.name,'description'," SQL_TOOL_DESCRIPTION ","
    "      'parameters',json(t.parameters_json))"
    "  ) FROM tools t"
    "   WHERE t.enabled=1"
    "     AND (t.agent_name IS NULL OR t.agent_name=?1)"
    /* Same predicate as SQL_OPENAI_TOOLS — shown ≡ granted. (This query kept
     * an "or the extension is attached" branch after the OpenAI one dropped
     * it, so on Gemini an agent was shown tools it could not call.) */
    "     AND (?2 IS NULL OR t.name IN (SELECT value FROM json_each(?2))))"
    "));";

static const char SQL_GEMINI_FULL[] =
    "SELECT json_patch(json_patch(json_patch('{}', json_object("
    /* ?5 = this session's prompt suffix — see SQL_OPENAI_FULL. */
    "  'systemInstruction', CASE WHEN COALESCE(?1,'') ||"
    "      COALESCE(char(10) || char(10) || ?5, '') != ''"
    "    THEN json_object('parts',json_array(json_object('text',"
    "      COALESCE(?1,'') || COALESCE(char(10) || char(10) || ?5, ''))))"
    "    ELSE NULL END,"
    /* ?2 (SQL_GEMINI_CONTENTS) already carries the user context block in the
     * right spot — see SQL_OPENAI_FULL. */
    "  'contents', json(?2),"
    "  'tools', CASE WHEN json_array_length(json_extract(?3,'$[0].functionDeclarations')) > 0"
    "    THEN json(?3) ELSE NULL END,"
    "  'generationConfig', CASE WHEN ?4 > 0"
    "    THEN json_object('maxOutputTokens',?4) ELSE NULL END"
    /* ?7 = the reasoning-effort fragment — its own generationConfig subtree,
     * which RFC 7386 merges into the one above rather than replacing it. */
    ")), json(?7)), " SQL_REQUEST_EXTRA("?6") ");";

/* ── Anthropic Messages ────────────────────────────────────────────
 *
 * Content is always an array of typed blocks; an iteration's assistant text
 * and tool calls are one assistant message, and all of that iteration's tool
 * results are one user message (the API reads split results as "don't call
 * tools in parallel") — the transcript's grp, as is. Adjacent same-role
 * messages are legal — the API merges them — so the user context block and
 * '[system] ' notes ride as their own user messages exactly as in the other
 * two formats. Empty text blocks are a 400, hence the guards: an assistant
 * message with nothing to say has no blocks and is dropped. */

#define SQL_CACHE_MARK "json('{\"type\":\"ephemeral\"}')"

static const char SQL_ANTHROPIC_MESSAGES[] =
    "WITH grouped(pos, role, content) AS MATERIALIZED ("
    "  SELECT MIN(t.pos), t.role,"
    "    CASE t.role"
    "      WHEN 'assistant' THEN COALESCE("
    /* Thinking replay. The API binds a thinking block's signature to
     * everything that preceded it, and wants the block back in its original
     * position among the text and tool_use blocks — so what is replayed is
     * the response's whole content array, verbatim, not a reconstruction.
     * Only within the turn that produced it: the user context block moves at
     * every turn boundary, which is an edit to the prefix those signatures
     * cover, while inside a turn the prefix is byte-stable (the mid-turn
     * invariant). Dropping the blocks of earlier turns is a leading run,
     * which the API allows. A model switch falls through to the
     * reconstruction below, as does any iteration that carried no thinking. */
    "        MAX(CASE WHEN json_extract(r.reasoning_meta,'$.format')='anthropic_content'"
    "                  AND r.turn_id = (SELECT MAX(turn_id) FROM _transcript)"
    "                 THEN json_extract(r.reasoning_meta,'$.blob') END),"
    "        json_group_array(json("
    "          CASE t.kind WHEN 'tool_call' THEN"
    "            json_object('type','tool_use','id',t.call_id,'name',t.tool_name,"
    "              'input',CASE WHEN json_valid(t.args) THEN json(t.args)"
    "                        ELSE json('{}') END)"
    "          ELSE json_object('type','text','text',t.text) END)"
    "          ORDER BY t.pos)"
    "          FILTER (WHERE t.kind = 'tool_call' OR COALESCE(t.text,'') != ''))"
    "      WHEN 'tool' THEN"
    "        json_group_array(json(json_patch("
    "          json_object('type','tool_result','tool_use_id',t.call_id,"
    "                      'content',t.text),"
    "          CASE WHEN t.is_error THEN json_object('is_error',json('true'))"
    "               ELSE '{}' END)) ORDER BY t.pos)"
    "      ELSE json_array(json_object('type','text',"
    "             'text',COALESCE(NULLIF(t.text,''),'(empty message)')))"
    "    END"
    "  FROM _transcript t" SQL_REPLAY_JOIN
    "  GROUP BY t.grp"
    "), live(pos, msg) AS ("
    "  SELECT pos, json_object('role',CASE role WHEN 'assistant' THEN 'assistant'"
    "                                 ELSE 'user' END, 'content',json(content))"
    "  FROM grouped WHERE json_array_length(content) > 0),"
    /* Prompt-cache breakpoints. Anthropic caching is opt-in and keyed on the
     * exact prefix up to a marked block, and the user context block's move at
     * each turn boundary changes everything after its old position. So the
     * stable prefix ends at the last message before the newest turn: marking
     * it writes the entry the NEXT turn reads, and marking the same spot one
     * turn back reads the entry the PREVIOUS turn wrote (the API only looks
     * ~20 blocks behind a marker on its own — a long tool loop is more than
     * that). Within a turn the request-level marker in SQL_ANTHROPIC_FULL
     * follows the growing tail. That is 4 markers with the system prompt's,
     * the API's limit. The block sits at start - 1, hence the bound. */
    " bp(pos) AS (SELECT (SELECT MAX(l.pos) FROM live l WHERE l.pos < s.start - 1)"
    "   FROM (SELECT MIN(pos) AS start FROM _transcript WHERE turn_id IS NOT NULL"
    "          GROUP BY turn_id ORDER BY turn_id DESC LIMIT 2) s)"
    "SELECT json_group_array(json(CASE WHEN pos IN (SELECT pos FROM bp)"
    "    THEN json_set(msg,'$.content[#-1].cache_control'," SQL_CACHE_MARK ")"
    "    ELSE msg END) ORDER BY pos)"
    " FROM live;";

static const char SQL_ANTHROPIC_TOOLS[] =
    "SELECT json_group_array("
    /* input_schema must say type:object; a parameterless tool's '{}' doesn't. */
    "  json_object('name',t.name,'description'," SQL_TOOL_DESCRIPTION ","
    "    'input_schema',json_patch('{\"type\":\"object\"}',"
    "                              COALESCE(t.parameters_json,'{}')))"
    ") FROM tools t"
    " WHERE t.enabled=1"
    "   AND (t.agent_name IS NULL OR t.agent_name=?1)"
    "   AND (?2 IS NULL OR t.name IN (SELECT value FROM json_each(?2)));";

/* Same parameters as SQL_OPENAI_FULL, plus ?9 = the routed models.id.
 *
 * max_tokens is mandatory here and also pays for thinking, which current
 * models cannot turn off — the global 4096 default would regularly be spent
 * before any visible text. The model row's max_output_tokens wins when set.
 *
 * thinking.block_binding: a thinking block replayed after its prefix changed
 * is a 400 on accounts where the check is enforced. Replay is scoped so that
 * should not happen (see SQL_ANTHROPIC_MESSAGES), but a mid-turn context cut
 * can still edit the prefix, and losing one block's
 * reasoning beats losing the turn. Every drop is reported in the response's
 * input_transformations, archived in llm_responses. A model that predates
 * adaptive thinking needs request_extra {"thinking":null}. */
static const char SQL_ANTHROPIC_FULL[] =
    "SELECT json_patch(json_patch(json_patch('{}', json_object("
    "  'model', ?1,"
    "  'max_tokens', COALESCE((SELECT max_output_tokens FROM models"
    "                           WHERE id=?9 AND max_output_tokens > 0),"
    "                         MAX(?4, 16000)),"
    "  'system', CASE WHEN ?2 IS NOT NULL OR ?6 IS NOT NULL THEN"
    "    json_array(json_object('type','text',"
    "      'text', COALESCE(?2,'') || COALESCE(char(10) || char(10) || ?6, ''),"
    "      'cache_control'," SQL_CACHE_MARK ")) END,"
    "  'messages', json(?3),"
    "  'tools', CASE WHEN json_array_length(?5) > 0 THEN json(?5) ELSE NULL END,"
    "  'thinking', json('{\"type\":\"adaptive\",\"block_binding\":"
    "                     {\"prefix_mismatch_behavior\":\"drop_block\"}}'),"
    "  'cache_control'," SQL_CACHE_MARK
    ")), json(?8)), " SQL_REQUEST_EXTRA("?7") ");";

/* ── Reasoning effort (M5) ─────────────────────────────────────────
 *
 * The knob is a level name on the routing entry (agent_models.reasoning_effort);
 * the wire spelling is a per-MODEL property (models.effort_map), because two
 * models behind one provider disagree — Cerebras gpt-oss takes low/medium/high
 * while its qwen3 takes neither, Gemini 2.5 wants a budget where 3.x wants a
 * level. The map is {"format": kind, "levels": {level: wire value}}; a missing
 * or null level means unsupported.
 *
 * Clamp-to-nearest-supported is pi's rule: an unsupported request walks the
 * ladder UP first (more thinking beats silently less), then down. Expressed as
 * the ORDER BY below — supported levels at or above the request sort first, and
 * within each direction the nearest wins. A map with no supported level at all
 * yields no row, hence no fragment, hence today's payload byte for byte. */
#define EFFORT_LADDER \
    "ladder(name, ord) AS (VALUES ('off',0),('minimal',1),('low',2)," \
    "                             ('medium',3),('high',4))"

static const char SQL_EFFORT_FRAGMENT[] =
    /* ?1 = models.id, ?2 = requested level, ?3 = endpoint default map. */
    "WITH " EFFORT_LADDER ","
    " map(m) AS (SELECT COALESCE((SELECT effort_map FROM models"
    "                              WHERE id=?1 AND json_valid(effort_map)), ?3)),"
    " req(ord) AS (SELECT ord FROM ladder WHERE name=?2),"
    " pick(lvl, val) AS ("
    "   SELECT l.name, json_extract((SELECT m FROM map), '$.levels.' || l.name)"
    "     FROM ladder l"
    "    WHERE json_extract((SELECT m FROM map), '$.levels.' || l.name) IS NOT NULL"
    "    ORDER BY (l.ord < (SELECT ord FROM req)),"
    "             abs(l.ord - (SELECT ord FROM req))"
    "    LIMIT 1)"
    "SELECT CASE json_extract((SELECT m FROM map), '$.format')"
    "  WHEN 'openai' THEN json_object('reasoning_effort', val)"
    /* DeepSeek has one bit, not a ladder: any level that survives the clamp
     * other than 'off' means thinking on. Its map's values only mark which
     * levels exist; the type string comes from the level name. */
    "  WHEN 'deepseek' THEN json_object('thinking', json_object('type',"
    "      CASE WHEN lvl='off' THEN 'disabled' ELSE 'enabled' END))"
    "  WHEN 'gemini-level' THEN json_object('generationConfig',"
    "      json_object('thinkingConfig', json_object('thinkingLevel', val)))"
    "  WHEN 'gemini-budget' THEN json_object('generationConfig',"
    "      json_object('thinkingConfig', json_object('thinkingBudget', val)))"
    "  WHEN 'anthropic' THEN json_object('output_config', json_object('effort', val))"
    /* 'openrouter' and anything unrecognized: the unified nested object.
     * OpenRouter spells "don't think" as enabled:false, not effort:'off'. */
    "  ELSE CASE WHEN lvl='off' THEN json_object('reasoning', json_object('enabled', json('false')))"
    "            ELSE json_object('reasoning', json_object('effort', val)) END"
    " END FROM pick;";

/* No effort_map on the model: assume the endpoint's own vocabulary, mapping
 * each level to itself. OpenAI-compat traffic is overwhelmingly OpenRouter,
 * whose unified reasoning.effort it converts to whatever the backend wants;
 * the Gemini endpoint takes an uppercase thinkingLevel. */
static const char EFFORT_DEFAULT_OPENAI[] =
    "{\"format\":\"openrouter\",\"levels\":{\"off\":\"off\",\"minimal\":\"minimal\","
    "\"low\":\"low\",\"medium\":\"medium\",\"high\":\"high\"}}";
static const char EFFORT_DEFAULT_GEMINI[] =
    "{\"format\":\"gemini-level\",\"levels\":{\"off\":\"OFF\",\"minimal\":\"MINIMAL\","
    "\"low\":\"LOW\",\"medium\":\"MEDIUM\",\"high\":\"HIGH\"}}";
/* Anthropic's effort ladder starts at 'low' and has no 'off' (current models
 * always think), so 'off' clamps up to 'minimal'. Its xhigh/max sit above our
 * ladder — a model row's own effort_map can point 'high' at one of them. */
static const char EFFORT_DEFAULT_ANTHROPIC[] =
    "{\"format\":\"anthropic\",\"levels\":{\"minimal\":\"low\","
    "\"low\":\"low\",\"medium\":\"medium\",\"high\":\"high\"}}";

/* The JSON object to merge into the request body for this route's effort
 * level, or NULL when nothing should be sent (no level set, or the model
 * supports no level at all). Caller frees. */
char *llm_effort_fragment(sqlite3 *db, const ProviderConfig *prov) {
    if (!prov->reasoning_effort || !prov->reasoning_effort[0]) return NULL;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, SQL_EFFORT_FRAGMENT, -1, &s, NULL) != SQLITE_OK)
        return NULL;
    if (prov->model_id) sqlite3_bind_text(s, 1, prov->model_id, -1, SQLITE_STATIC);
    else sqlite3_bind_null(s, 1);
    sqlite3_bind_text(s, 2, prov->reasoning_effort, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3,
                      prov->endpoint_type == ENDPOINT_GEMINI    ? EFFORT_DEFAULT_GEMINI
                    : prov->endpoint_type == ENDPOINT_ANTHROPIC ? EFFORT_DEFAULT_ANTHROPIC
                    : EFFORT_DEFAULT_OPENAI,
                      -1, SQLITE_STATIC);
    char *r = NULL;
    if (sqlite3_step(s) == SQLITE_ROW && sqlite3_column_type(s, 0) != SQLITE_NULL) {
        const char *t = (const char *)sqlite3_column_text(s, 0);
        if (t) r = strdup(t);
    }
    sqlite3_finalize(s);
    return r;
}

/* ── Helpers ───────────────────────────────────────────────────── */

/* Everything appended to the system prompt of a session that belongs to a
 * chat, in three parts: where it is chatting, how its replies reach that chat
 * (from the chat's delivery_mode — the same value delivery reads, so the
 * model is told what will actually happen), then the chat's guidance. NULL
 * for a session with no chat. Assembled in SQL, like the rest of this file,
 * and read for every request: the text changes only when the chat's row
 * does (a rename, a mode flip, edited guidance), so it stays byte-stable for
 * prompt caching otherwise. */
static char *chat_prompt_suffix(sqlite3 *db, int64_t session_id,
                                const char *allowed_tools) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT 'You are chatting in ' || COALESCE(c.title, 'chat ' || c.chat_id)"
            "     || ' (channel ' || c.channel_name || ', chat id ' || c.chat_id || ').'"
            "     || char(10) || char(10) || CASE c.delivery_mode"
            /* ?2 = this request's tool list (NULL = unrestricted): an
             * agent without channel_send is told it cannot speak here,
             * rather than led to believe a reply goes somewhere. */
            "       WHEN 'explicit' THEN CASE WHEN ?2 IS NULL OR EXISTS(SELECT 1"
            "             FROM json_each(?2) WHERE value='channel_send')"
            "         THEN 'Nothing you write is posted to this chat. To say something'"
            "              || ' here, call channel_send.'"
            "         ELSE 'Nothing you write is posted to this chat, and you have no'"
            "              || ' channel_send tool, so you cannot reply here.' END"
            "       WHEN 'iteration' THEN 'Each message you write is posted to this chat'"
            "                            || ' as you write it.'"
            "       WHEN 'digest' THEN 'Everything you write during a turn is posted to'"
            "                            || ' this chat when the turn ends.'"
            "       ELSE 'Your final message of the turn is posted to this chat.' END"
            "     || COALESCE(char(10) || char(10) || NULLIF(c.guidance, ''), '')"
            " FROM sessions s JOIN channel_chats c"
            "   ON c.channel_name = s.channel_name AND c.chat_id = s.chat_id"
            " WHERE s.id=?1",
            -1, &s, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(s, 1, session_id);
    if (allowed_tools) sqlite3_bind_text(s, 2, allowed_tools, -1, SQLITE_STATIC);
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

/* One of the wire projections of _transcript. model (?1) is the requesting
 * model, which reasoning replay matches on. */
static char *query_text(sqlite3 *db, const char *sql, const char *model) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return NULL;
    if (model && model[0]) sqlite3_bind_text(s, 1, model, -1, SQLITE_STATIC);
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
static const char SQL_USER_CONTEXT[] =
    "WITH mb AS ("
    "  SELECT group_concat(" MEMORY_BLOCK_RENDER_SQL ", '' ORDER BY b.id) AS txt"
    "  FROM agent_memory_blocks a JOIN memory_blocks b ON b.id=a.block_id"
    "  WHERE a.agent_name=(SELECT agent_name FROM sessions WHERE id=?1) AND b.placement='context'"
    "), sub AS ("
    "  SELECT group_concat("
    "    'session #' || s.id || ' (' || COALESCE(s.agent_name,'?') || ') — ' || s.state,"
    "    char(10)) AS txt"
    "  FROM sessions s WHERE s.parent_session_id=?1 AND s.state != 'idle'"
    "), jobs AS ("
    /* Background jobs (status='background'): id + command snippet + age +
     * log path. Frozen with the rest of the user context block — mid-turn
     * freshness is pull-based (read the log / check_session), and the
     * completion arrives as its own user entry, so block and entries never
     * disagree at turn start. */
    "  SELECT group_concat("
    "    'job ' || tc.id || ' (' || tc.name || ') — ' ||"
    "    substr(replace(COALESCE(json_extract(e.content,'$.command'),"
    "                            e.content, ''), char(10), ' '), 1, 80) ||"
    "    ' — running ' || (unixepoch() - e.created_at) || 's — log"
    " .tool_results/' || tc.session_id || '/' || tc.call_id || '.log',"
    "    char(10)) AS txt"
    "  FROM tool_calls tc JOIN entries e ON e.id=tc.entry_id"
    "  WHERE tc.session_id=?1 AND tc.status='background'"
    "), appr AS ("
    /* Pending only — "waiting on a human" is the one approval state the model
     * can act on (work around it). Decided rows are history: apply effects
     * land at decision time, rerun tickets are consumed by the wake/rerun and
     * ticket-transfer machinery, and advertising them here sends the model
     * chasing "unused" grants it already has. */
    "  SELECT group_concat("
    "    '#' || a.id || ' ' || COALESCE(a.tool_name, '?') ||"
    "    ' — pending: waiting on a human decision', char(10)) AS txt"
    "  FROM approvals a WHERE a.session_id=?1 AND a.state='pending'"
    ")"
    "SELECT '<RELEVANT_CONTEXT>' || char(10) ||"
    "    '<current_time>' || ?3 || '</current_time>' || char(10) ||"
    "    COALESCE('<recall>' || char(10) || ?2 || char(10) || '</recall>' || char(10), '') ||"
    "    COALESCE('<memory_blocks>' || mb.txt || '</memory_blocks>' || char(10), '') ||"
    "    COALESCE('<running_sub_agents>' || char(10) || sub.txt || char(10) ||"
    "      '</running_sub_agents>' || char(10), '') ||"
    "    COALESCE('<background_jobs>' || char(10) || jobs.txt || char(10) ||"
    "      '(tail the log for live output — ps cannot see jobs, each runs in"
    " its own PID namespace; check_session {job_id} for status; cancel"
    " {job_id} to stop. Results arrive as messages on completion.)'"
    "      || char(10) || '</background_jobs>' || char(10), '') ||"
    "    COALESCE('<open_approvals>' || char(10) || appr.txt || char(10) ||"
    "      '</open_approvals>' || char(10), '') ||"
    "    '</RELEVANT_CONTEXT>'"
    " FROM mb, sub, jobs, appr;";

/* ── Public API ────────────────────────────────────────────────── */

/* Wall clock for the user context block, stated with its zone — the model can't
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

char *user_context_text(sqlite3 *db, int64_t session_id, const char *recall_text) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, SQL_USER_CONTEXT, -1, &st, NULL) != SQLITE_OK)
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
                      const ContextPlan *plan, const char *user_context,
                      const char *system_prompt, LlmPayload *out) {
    if (!db || !cfg || !plan || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->db = db;

    int gemini = (cfg->provider.endpoint_type == ENDPOINT_GEMINI);
    if (build_transcript(db, session_id, plan, user_context) != 0) return -1;

    /* Effective tool scope = grants ∩ session tool_filter ∩ the pinned chat's
     * tool_filter as it is now. An agent with an empty grant set keeps "[]"
     * (sees zero tools) — only a NULL agent is unrestricted. The filters (the
     * session's frozen at spawn for workers, the chat's read live) can only
     * narrow; dispatch_tool re-checks all three, so this is advisory for the
     * model. */
    char *agent_name = session_get_agent_name(db, session_id);
    char *allowed_tools = agent_name ? grants_json(db, agent_name, "tool") : NULL;
    char *tool_filter = session_effective_tool_filter(db, session_id);
    char *worker_tools = worker_tools_list(db);
    char *effort = llm_effort_fragment(db, &cfg->provider);
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
    char *suffix = chat_prompt_suffix(db, session_id, allowed_tools);

    /* Reasoning replay matches on the model that produced it. Ingest stamps
     * the routed model's id (`model@provider`), so that is what must be
     * compared — the bare wire name never equals it, and the signature a
     * thinking model requires back was silently dropped (Gemini: HTTP 400
     * "missing a thought_signature"). The bare name is the fallback for the
     * unrouted synthetic provider, where ingest stamps it too. */
    const char *replay_model = cfg->provider.model_id ? cfg->provider.model_id
                                                      : cfg->provider.model;

    if (gemini) {
        char *contents = query_text(db, SQL_GEMINI_CONTENTS, replay_model);
        char *tools_json = query_tools(db, SQL_GEMINI_TOOLS, agent_name,
                                       allowed_tools, worker_tools);

        sqlite3_stmt *full;
        if (sqlite3_prepare_v2(db, SQL_GEMINI_FULL, -1, &full, NULL) != SQLITE_OK) {
            free(contents); free(tools_json);
            free(agent_name); free(allowed_tools); free(suffix); free(worker_tools); free(effort);
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
        if (cfg->provider.name) sqlite3_bind_text(full, 6, cfg->provider.name, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(full, 6);
        sqlite3_bind_text(full, 7, effort ? effort : "{}", -1, SQLITE_TRANSIENT);

        free(contents); free(tools_json);

        if (sqlite3_step(full) == SQLITE_ROW) {
            out->stmt = full;
            out->body = (const char *)sqlite3_column_text(full, 0);
        } else {
            sqlite3_finalize(full);
            free(agent_name); free(allowed_tools); free(suffix); free(worker_tools); free(effort);
            return -1;
        }
    } else {
        /* OpenAI and Anthropic share one parameter list; only the SQL differs. */
        int anthropic = (cfg->provider.endpoint_type == ENDPOINT_ANTHROPIC);
        char *messages = query_text(db, anthropic ? SQL_ANTHROPIC_MESSAGES
                                                  : SQL_OPENAI_MESSAGES,
                                    replay_model);
        if (!messages) messages = strdup("[]");
        char *tools_json = query_tools(db, anthropic ? SQL_ANTHROPIC_TOOLS
                                                     : SQL_OPENAI_TOOLS,
                                       agent_name, allowed_tools, worker_tools);

        sqlite3_stmt *full;
        if (sqlite3_prepare_v2(db, anthropic ? SQL_ANTHROPIC_FULL : SQL_OPENAI_FULL,
                               -1, &full, NULL) != SQLITE_OK) {
            free(messages); free(tools_json);
            free(agent_name); free(allowed_tools); free(suffix); free(worker_tools); free(effort);
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
        if (cfg->provider.name) sqlite3_bind_text(full, 7, cfg->provider.name, -1, SQLITE_TRANSIENT);
        else sqlite3_bind_null(full, 7);
        sqlite3_bind_text(full, 8, effort ? effort : "{}", -1, SQLITE_TRANSIENT);
        if (anthropic && cfg->provider.model_id)
            sqlite3_bind_text(full, 9, cfg->provider.model_id, -1, SQLITE_STATIC);

        free(messages); free(tools_json);

        if (sqlite3_step(full) == SQLITE_ROW) {
            out->stmt = full;
            out->body = (const char *)sqlite3_column_text(full, 0);
        } else {
            sqlite3_finalize(full);
            free(agent_name); free(allowed_tools); free(suffix); free(worker_tools); free(effort);
            return -1;
        }
    }

    free(agent_name); free(allowed_tools); free(suffix); free(worker_tools); free(effort);
    return 0;
}

void llm_payload_release(LlmPayload *p) {
    if (!p) return;
    if (p->stmt) { sqlite3_finalize(p->stmt); p->stmt = NULL; }
    if (p->db)
        sqlite3_exec(p->db, "DROP TABLE IF EXISTS _transcript; DROP TABLE IF EXISTS _plan;",
                     NULL, NULL, NULL);
    p->body = NULL;
}
