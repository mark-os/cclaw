#define _POSIX_C_SOURCE 200809L
#include "db_response.h"
#include "config.h"
#include "config_registry.h"
#include "db.h"
#include "llm.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* SQL response ingress.
 * Tool call arguments validated with json() — stored in entries.content as
 * normalized text (single source of truth). Invalid args get an immediate error
 * tool_result. tool_calls table holds only workflow state, no arguments. */

int db_tool_call_complete_with_result(sqlite3 *db, int64_t entry_id,
                                      const char *call_id, int64_t result_entry_id) {
    if (!db || !call_id) return -1;
    const char *sql =
        "UPDATE tool_calls SET status='done', result_entry_id=?"
        " WHERE entry_id=? AND call_id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, result_entry_id);
    sqlite3_bind_int64(stmt, 2, entry_id);
    sqlite3_bind_text(stmt, 3, call_id, -1, SQLITE_STATIC);
    int rc = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0) ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

/* Scalar columns, identical order for both providers:
 *   0 content   1 finish_reason   2 reasoning   3 prompt_tokens
 *   4 completion_tokens   5 total_tokens   6 cost(real)   7 array_length
 *   8 reasoning replay blob (JSON text, NULL when the response carries none)
 *   9 cached_tokens   10 cache_write_tokens   11 reasoning_tokens
 *
 * Column 8 is the verbatim wire artifact the provider requires back on the
 * next request (specs: OpenRouter reasoning_details — order and every field
 * matter, it is how Gemini's thoughtSignature round-trips through the
 * OpenAI-compat envelope; Gemini-native thoughtSignature per part). Stored
 * unmodified in entries.reasoning_meta.blob; see replay in llm_payload.c.
 * Columns 9-11 are NULL on providers that don't report them; nothing depends
 * on them (absent == pre-M4 behavior everywhere). cached_tokens is a SUBSET of
 * prompt_tokens on every provider supported today (cache-inclusive wire
 * semantics) — a cache-exclusive provider would have to be normalized here. */
static const char SCALAR_OPENAI[] =
    "SELECT"
    " json_extract(?1,'$.choices[0].message.content'),"
    " json_extract(?1,'$.choices[0].finish_reason'),"
    " COALESCE(json_extract(?1,'$.choices[0].message.reasoning'),"
    "          json_extract(?1,'$.choices[0].message.reasoning_content'),"
    "          json_extract(?1,'$.choices[0].message.reasoning_text')),"
    " json_extract(?1,'$.usage.prompt_tokens'),"
    " json_extract(?1,'$.usage.completion_tokens'),"
    " json_extract(?1,'$.usage.total_tokens'),"
    " COALESCE(json_extract(?1,'$.usage.cost'), json_extract(?1,'$.usage.total_cost')),"
    " json_array_length(?1,'$.choices'),"
    " CASE WHEN json_array_length(?1,'$.choices[0].message.reasoning_details') > 0"
    "   THEN json_extract(?1,'$.choices[0].message.reasoning_details') END,"
    /* DeepSeek-direct spells the cache-read count prompt_cache_hit_tokens;
     * OpenAI/OpenRouter nest it under prompt_tokens_details. */
    " COALESCE(json_extract(?1,'$.usage.prompt_tokens_details.cached_tokens'),"
    "          json_extract(?1,'$.usage.prompt_cache_hit_tokens')),"
    " json_extract(?1,'$.usage.prompt_tokens_details.cache_write_tokens'),"
    " json_extract(?1,'$.usage.completion_tokens_details.reasoning_tokens')";

/* A text part marked thought:true is a thought summary (asked for with
 * includeThoughts, llm_payload.c): it is the reasoning, not the answer. */
static const char SCALAR_GEMINI[] =
    "SELECT"
    " (SELECT group_concat(json_extract(value,'$.text'), char(10))"
    "    FROM json_each(?1,'$.candidates[0].content.parts')"
    "    WHERE json_extract(value,'$.text') IS NOT NULL"
    "      AND json_extract(value,'$.thought') IS NOT 1),"
    " CASE json_extract(?1,'$.candidates[0].finishReason')"
    "   WHEN 'STOP' THEN 'stop' WHEN 'MAX_TOKENS' THEN 'length'"
    "   WHEN 'SAFETY' THEN 'content_filter' WHEN 'RECITATION' THEN 'content_filter'"
    "   ELSE json_extract(?1,'$.candidates[0].finishReason') END,"
    " (SELECT group_concat(json_extract(value,'$.text'), char(10))"
    "    FROM json_each(?1,'$.candidates[0].content.parts')"
    "    WHERE json_extract(value,'$.thought') = 1"
    "      AND json_extract(value,'$.text') != ''),"
    " json_extract(?1,'$.usageMetadata.promptTokenCount'),"
    /* thoughtsTokenCount is a sibling of candidatesTokenCount, not a part of
     * it — summing them is what makes Gemini's completion count comparable to
     * the OpenAI-compat one (pre-M4 we stored only candidates and undercounted
     * every thinking response). */
    " COALESCE(json_extract(?1,'$.usageMetadata.candidatesTokenCount'),0)"
    "   + COALESCE(json_extract(?1,'$.usageMetadata.thoughtsTokenCount'),0),"
    " json_extract(?1,'$.usageMetadata.totalTokenCount'),"
    " NULL,"
    " json_array_length(?1,'$.candidates'),"
    /* One row per part that carried a thoughtSignature, tagged with the
     * functionCall name it rode on — that name is what replay matches, since
     * part_index in entries counts only the parts we kept. */
    " (SELECT NULLIF(json_group_array(json_object("
    "     'fn', json_extract(value,'$.functionCall.name'),"
    "     'sig', json_extract(value,'$.thoughtSignature'))), '[]')"
    "    FROM json_each(?1,'$.candidates[0].content.parts')"
    "    WHERE json_extract(value,'$.thoughtSignature') IS NOT NULL),"
    /* Gemini's JSON omits a zero-valued field (proto3), so a usage report
     * without cachedContentTokenCount is a miss — a reported zero. Checked
     * live: the field is absent on a request whose prefix the one before
     * had just read from cache. No usageMetadata stays not reported. */
    " CASE WHEN json_type(?1,'$.usageMetadata') = 'object' THEN"
    "   COALESCE(json_extract(?1,'$.usageMetadata.cachedContentTokenCount'),0) END,"
    " NULL,"
    " json_extract(?1,'$.usageMetadata.thoughtsTokenCount')";

static const char SCALAR_ANTHROPIC[] =
    "SELECT"
    /* A refusal is a 200 whose content may be empty; say so in the entry
     * rather than leave a silent error turn. */
    " COALESCE("
    "   (SELECT group_concat(json_extract(value,'$.text'), char(10))"
    "      FROM json_each(?1,'$.content')"
    "      WHERE json_extract(value,'$.type') = 'text'),"
    "   CASE WHEN json_extract(?1,'$.stop_reason') = 'refusal' THEN"
    "     '[the model declined this request' ||"
    "     COALESCE(': ' || json_extract(?1,'$.stop_details.category'), '') || ']' END),"
    " CASE json_extract(?1,'$.stop_reason')"
    "   WHEN 'end_turn' THEN 'stop' WHEN 'stop_sequence' THEN 'stop'"
    "   WHEN 'pause_turn' THEN 'stop' WHEN 'max_tokens' THEN 'length'"
    "   WHEN 'model_context_window_exceeded' THEN 'length'"
    "   WHEN 'tool_use' THEN 'tool_calls' WHEN 'refusal' THEN 'content_filter'"
    "   ELSE json_extract(?1,'$.stop_reason') END,"
    /* Display text only — the summary thinking.display "summarized" asks
     * for, empty under "omitted"; replay reads column 8, never this. */
    " (SELECT group_concat(json_extract(value,'$.thinking'), char(10))"
    "    FROM json_each(?1,'$.content')"
    "    WHERE json_extract(value,'$.type') = 'thinking'"
    "      AND json_extract(value,'$.thinking') != ''),"
    /* Anthropic's input_tokens EXCLUDES cache reads and writes; every other
     * provider's prompt count includes them, and so do the limiter and the
     * context accounting that read this column. Normalized here. */
    " json_extract(?1,'$.usage.input_tokens')"
    "   + COALESCE(json_extract(?1,'$.usage.cache_read_input_tokens'),0)"
    "   + COALESCE(json_extract(?1,'$.usage.cache_creation_input_tokens'),0),"
    " json_extract(?1,'$.usage.output_tokens'),"
    " NULL,"
    " NULL,"
    /* Shape check: a message object with a content array. The array may
     * legitimately be empty (refusal, or max_tokens spent on thinking), so
     * report 1 rather than its length. */
    " CASE WHEN json_extract(?1,'$.type') = 'message'"
    "       AND json_type(?1,'$.content') = 'array' THEN 1 END,"
    /* The whole content array, verbatim, whenever it carries thinking: the
     * signature covers the block's position among its siblings, so replay
     * needs the original array, not the thinking blocks alone. */
    " CASE WHEN EXISTS (SELECT 1 FROM json_each(?1,'$.content')"
    "     WHERE json_extract(value,'$.type') IN ('thinking','redacted_thinking'))"
    "   THEN json_extract(?1,'$.content') END,"
    " json_extract(?1,'$.usage.cache_read_input_tokens'),"
    " json_extract(?1,'$.usage.cache_creation_input_tokens'),"
    " json_extract(?1,'$.usage.output_tokens_details.thinking_tokens')";

/* Tool-call rows, columns: 0 id (NULL for Gemini), 1 name, 2 arguments. */
static const char TC_OPENAI[] =
    "SELECT json_extract(value,'$.id'),"
    " json_extract(value,'$.function.name'),"
    " json_extract(value,'$.function.arguments')"
    " FROM json_each(?1,'$.choices[0].message.tool_calls')";

static const char TC_GEMINI[] =
    "SELECT NULL,"
    " json_extract(value,'$.functionCall.name'),"
    " json_extract(value,'$.functionCall.args')"
    " FROM json_each(?1,'$.candidates[0].content.parts')"
    " WHERE json_extract(value,'$.functionCall') IS NOT NULL";

static const char TC_ANTHROPIC[] =
    "SELECT json_extract(value,'$.id'),"
    " json_extract(value,'$.name'),"
    " json_extract(value,'$.input')"
    " FROM json_each(?1,'$.content')"
    " WHERE json_extract(value,'$.type') = 'tool_use'";

/* Tag a freshly inserted type='reasoning' entry with everything replay needs:
 * who produced it (provider+model — a model switch strips the replay, pi's
 * rule) and the verbatim wire artifact under $.blob. The blob is always where
 * replay reads from, never entries.content: the content half is display-only
 * and the routing entry's reasoning_display 'none' leaves it out, while the
 * wire requirement holds regardless of what an operator wants to *see*.
 *   gemini_parts      blob = [{fn, sig}, …]  (native thoughtSignature)
 *   reasoning_details blob = the provider's array, byte-for-byte
 *   reasoning_content blob = the bare string (DeepSeek-style)
 *   anthropic_content blob = the response's whole content array
 * Best-effort: the entry is already durable, a failure here just means no
 * replay for that iteration. */
static void reasoning_meta_set(sqlite3 *db, int64_t entry_id, EndpointType ep,
                               const char *model, const char *blob_json,
                               const char *text) {
    const char *fmt = ep == ENDPOINT_GEMINI    ? "gemini_parts"
                    : ep == ENDPOINT_ANTHROPIC ? "anthropic_content"
                    : (blob_json ? "reasoning_details" : "reasoning_content");
    /* Only the OpenAI-compat path can replay bare text. */
    if (ep != ENDPOINT_OPENAI && !blob_json) return;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "UPDATE entries SET reasoning_meta = json_object("
            "  'provider', ?2, 'model', ?3, 'format', ?4,"
            "  'blob', CASE WHEN ?5 IS NOT NULL THEN json(?5) ELSE ?6 END)"
            " WHERE id = ?1", -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(s, 1, entry_id);
    sqlite3_bind_text(s, 2, endpoint_type_name(ep), -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, model ? model : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 4, fmt, -1, SQLITE_STATIC);
    if (blob_json) sqlite3_bind_text(s, 5, blob_json, -1, SQLITE_STATIC);
    else sqlite3_bind_null(s, 5);
    if (text) sqlite3_bind_text(s, 6, text, -1, SQLITE_STATIC);
    else sqlite3_bind_null(s, 6);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

/* Archive one raw response for forensics. When is_jsonb, body/blen is the
 * parsed JSONB blob (and we can pull the provider's $.id out of it); otherwise
 * body is NUL-terminated text that wasn't valid JSON. Best-effort: errors here
 * never affect ingest. usage (may be NULL) carries the extracted usage block —
 * only the success path has one; failures archive with NULL usage columns. */
typedef struct {
    int prompt_tokens, completion_tokens;
    int cached_tokens, cache_write_tokens, reasoning_tokens;  /* <0 = absent */
    double cost; int has_cost;
} RespUsage;

static void archive_store(sqlite3 *db, int64_t session_id, int64_t iteration_id,
                          const char *model, const char *status,
                          const void *body, int blen, int is_jsonb,
                          const char *request_body, const RespUsage *usage) {
    /* Retention cap (config 'llm_response_archive_max'):
     *   > 0  keep the most recent N 'ok' rows and the most recent N failures
     *   == 0 archiving off — write nothing
     *   < 0  keep everything (no pruning) */
    int cap = config_default_int("llm_response_archive_max");
    sqlite3_stmt *c;
    if (sqlite3_prepare_v2(db,
            "SELECT CAST(COALESCE(value, default_value) AS INTEGER) FROM config"
            " WHERE key='llm_response_archive_max'",
            -1, &c, NULL) == SQLITE_OK) {
        if (sqlite3_step(c) == SQLITE_ROW && sqlite3_column_type(c, 0) != SQLITE_NULL)
            cap = sqlite3_column_int(c, 0);
        sqlite3_finalize(c);
    }
    if (cap == 0) return;   /* archiving disabled — skip the insert entirely */

    const char *sql =
        "INSERT INTO llm_responses(session_id,iteration_id,model,status,provider_id,"
        "  upstream_provider,body,request_body,"
        "  cached_tokens,cache_write_tokens,reasoning_tokens,cost)"
        " VALUES(?1,?2,?3,?4,"
        "  CASE WHEN ?5 THEN COALESCE(json_extract(?6,'$.id'),"
        "                             json_extract(?6,'$.responseId')) END,"
        /* $.provider is the upstream backend that actually served the request
         * (OpenRouter), a different field from $.id — see provider_id above.
         * Recorded on every response, not just failures: it is the audit trail
         * that turns "exclude this backend" into an evidence-backed call. */
        "  CASE WHEN ?5 THEN json_extract(?6,'$.provider') END, ?6,"
        "  CASE WHEN ?7 IS NOT NULL THEN jsonb(?7) END,?8,?9,?10,?11);";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, session_id);
        sqlite3_bind_int64(s, 2, iteration_id);
        sqlite3_bind_text(s, 3, model ? model : "", -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 4, status, -1, SQLITE_STATIC);
        sqlite3_bind_int(s, 5, is_jsonb);
        if (is_jsonb) sqlite3_bind_blob(s, 6, body, blen, SQLITE_STATIC);
        else sqlite3_bind_text(s, 6, (const char *)body, -1, SQLITE_STATIC);
        if (request_body) sqlite3_bind_text(s, 7, request_body, -1, SQLITE_STATIC);
        else sqlite3_bind_null(s, 7);
        int u[3] = { usage ? usage->cached_tokens : -1,
                     usage ? usage->cache_write_tokens : -1,
                     usage ? usage->reasoning_tokens : -1 };
        for (int i = 0; i < 3; i++) {
            if (u[i] >= 0) sqlite3_bind_int(s, 8 + i, u[i]);
            else sqlite3_bind_null(s, 8 + i);
        }
        if (usage && usage->has_cost) sqlite3_bind_double(s, 11, usage->cost);
        else sqlite3_bind_null(s, 11);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }

    if (cap > 0) {
        /* Prune successes and failures independently: failure rows are what
         * error entries cite ("[resp #N]") and are rare — a busy session's
         * steady stream of 'ok' rows must not push them out before the
         * operator gets to look. */
        char prune[512];
        snprintf(prune, sizeof(prune),
            "DELETE FROM llm_responses WHERE status='ok' AND id NOT IN"
            " (SELECT id FROM llm_responses WHERE status='ok' ORDER BY id DESC LIMIT %d);"
            "DELETE FROM llm_responses WHERE status!='ok' AND id NOT IN"
            " (SELECT id FROM llm_responses WHERE status!='ok' ORDER BY id DESC LIMIT %d);",
            cap, cap);
        sqlite3_exec(db, prune, NULL, NULL, NULL);
    }
}

/* Public entry: archive a raw response body (any HTTP outcome) given as text.
 * Parses to JSONB when valid, stores raw text otherwise. Used for non-2xx /
 * network errors that never reach db_ingest_response. */
void db_archive_response(sqlite3 *db, int64_t session_id, int64_t iteration_id,
                         const char *model, const char *status, const char *body,
                         const char *request_body) {
    if (!db || !status) return;
    if (!body) body = "";
    sqlite3_stmt *j;
    if (sqlite3_prepare_v2(db, "SELECT jsonb(?1)", -1, &j, NULL) == SQLITE_OK) {
        sqlite3_bind_text(j, 1, body, -1, SQLITE_STATIC);
        if (sqlite3_step(j) == SQLITE_ROW) {
            archive_store(db, session_id, iteration_id, model, status,
                          sqlite3_column_blob(j, 0), sqlite3_column_bytes(j, 0), 1,
                          request_body, NULL);
            sqlite3_finalize(j);
            return;
        }
        sqlite3_finalize(j);
    }
    archive_store(db, session_id, iteration_id, model, status, body, -1, 0, request_body, NULL);
}

/* Tool-call markup leaking into `reasoning` as plain text.
 *
 * DeepSeek tokenizes its tool calls as DSML markup
 * (`<｜DSML｜tool_calls><｜DSML｜invoke name="js_eval">...`); some OpenRouter
 * upstreams fail to parse it back into a structured tool_calls array and hand
 * the raw text back inside `reasoning` with `content: null` and
 * `finish_reason: stop`. Wire-legal, so nothing else fires — the turn used to
 * end in silence (prod resps #336 and #644, both StreamLake).
 *
 * Conservative on purpose: only an opening marker (ASCII `<|` or its fullwidth
 * U+FF5C twin) immediately followed by tool-call markup counts. A model merely
 * *discussing* tool calls in prose has no marker, and the caller only consults
 * this when there is no content and no tool_calls at all. */
static int reasoning_has_tool_markup(const char *r) {
    static const char *kWords[] = { "DSML", "tool_call", "tool\xe2\x96\x81" "call",  /* U+2581 ▁ variant */
                                    "invoke name", "function_call", NULL };
    static const char kFullwidthBar[] = "\xef\xbd\x9c";   /* U+FF5C */
    if (!r) return 0;
    for (const char *p = r; (p = strchr(p, '<')) != NULL; p++) {
        const char *after = NULL;
        if (p[1] == '|') after = p + 2;
        else if (strncmp(p + 1, kFullwidthBar, 3) == 0) after = p + 4;
        if (!after) continue;
        for (int i = 0; kWords[i]; i++)
            if (strncmp(after, kWords[i], strlen(kWords[i])) == 0) return 1;
    }
    return 0;
}

/* ── Attributing the provider's counts to entries ───────────────────
 *
 * Every response reports two numbers: usage_in, the whole request that
 * produced it, and usage_out, what the model wrote. Measured across three
 * wire formats (2026-11), the next request's usage_in is this one's, plus
 * this usage_out, plus whatever was appended in between — to within a few
 * tokens of framing — with one variable: reasoning costs its full reported
 * count while it is replayed and nothing once it is not.
 *
 * So each entry's size is written once, here, when the report that measures
 * it arrives (entries.tokens), and everything that sizes a context reads the
 * stored number (context.c):
 *
 *   1. this response's own entries share usage_out: the reasoning entry gets
 *      the reasoning tokens the provider reports (0 when there is nothing to
 *      replay), the message and its tool calls share the rest by length;
 *   2. the entries appended since the previous report — tool results, user
 *      messages, notes — share the growth in usage_in that the previous
 *      response's entries do not account for, by length. At a turn boundary
 *      the user context block was rebuilt, so the new block is one of the
 *      sharers and the old block's count (sessions.turn_context_tokens) is
 *      given back first.
 *
 * Reasoning is the one thing that leaves a request without an edit. Inside a
 * turn the previous response's reasoning is replayed; across a boundary the
 * Anthropic format drops the whole previous turn's (llm_payload.c), and some
 * providers stop counting what they are sent. The expected case is tried
 * first and the alternatives after it; reasoning found dropped is set to 0.
 *
 * A difference is only trusted when the two requests can be compared — same
 * model, no compaction and no turn edit between them — and when what is left
 * for the appended entries is within sight of their estimate (a moved cut or
 * a changed system prompt shows up there). Otherwise nothing is written and
 * those entries keep their estimate: measured once, or not at all. */
typedef struct {
    int64_t id, turn_id, iteration_id;
    int role, stop, usage_in, tokens, est;      /* tokens < 0: not measured */
    char kind;                                  /* r)easoning a)ssistant_message o)ther */
    char same_model, anthropic, skip;
} AttrRow;

#define ATTR_MAX_ROWS 1024

/* Run one statement of up to three integer parameters (binding one the
 * statement does not have is a no-op). */
static void attr_exec(sqlite3 *db, const char *sql, int64_t a, int64_t b, int64_t c) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN_("usage attribution: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_int64(st, 1, a);
    sqlite3_bind_int64(st, 2, b);
    sqlite3_bind_int64(st, 3, c);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* Step 1: usage_out over the response's own entries. reasoning < 0 = the
 * provider did not say: the reasoning entry stays unmeasured. */
static void attr_own(sqlite3 *db, int64_t session_id, int64_t iteration_id,
                     int out, int reasoning) {
    if (out <= 0) return;
    if (reasoning > out) reasoning = out;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "UPDATE entries SET tokens = CASE type WHEN 'reasoning' THEN"
            "    CASE WHEN ?3 < 0 THEN NULL"
            "         WHEN json_extract(reasoning_meta,'$.blob') IS NULL THEN 0 ELSE ?3 END"
            "  ELSE ?4 * token_estimate / (SELECT SUM(v.token_estimate) FROM entries v"
            "    WHERE v.session_id=?1 AND v.iteration_id=?2"
            "      AND v.type IN ('assistant_message','tool_call')) END"
            " WHERE session_id=?1 AND iteration_id=?2"
            "   AND type IN ('reasoning','assistant_message','tool_call');",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN_("usage attribution: %s", sqlite3_errmsg(db));
        return;
    }
    int visible = out - (reasoning > 0 ? reasoning : 0);
    sqlite3_bind_int64(st, 1, session_id);
    sqlite3_bind_int64(st, 2, iteration_id);
    sqlite3_bind_int(st, 3, reasoning);
    sqlite3_bind_int(st, 4, visible);
    sqlite3_step(st);
    sqlite3_finalize(st);
    /* Integer division leaves a few tokens over: the message takes them. */
    attr_exec(db,
        "UPDATE entries SET tokens = tokens + ?3 - (SELECT SUM(v.tokens) FROM entries v"
        "   WHERE v.session_id=?1 AND v.iteration_id=?2"
        "     AND v.type IN ('assistant_message','tool_call'))"
        " WHERE id=(SELECT id FROM entries WHERE session_id=?1 AND iteration_id=?2"
        "            AND type='assistant_message' ORDER BY id DESC LIMIT 1);",
        session_id, iteration_id, visible);
}

/* The branch from the response upward, newest first, until the previous
 * report and the reasoning entry that opens its response. Returns the row
 * count and sets *prev to the previous report's index, -1 when there is none
 * to compare with (first report, a compaction in between, too far back). */
static int attr_walk(sqlite3 *db, int64_t asst_id, int64_t iteration_id,
                     const char *model, AttrRow *rows, int *prev) {
    *prev = -1;
    sqlite3_stmt *st;
    /* No ORDER BY: a recursive CTE yields rows as it generates them, so
     * stepping stops at the previous report without reading the branch. */
    if (sqlite3_prepare_v2(db,
            "WITH RECURSIVE up(id, parent_id, turn_id, iteration_id, role, type, stop,"
            "                  usage_in, tokens, est, same_model, anthropic) AS ("
            "  SELECT id, parent_id, turn_id, iteration_id, role, type, stop_reason,"
            "         usage_in, tokens, token_estimate, model IS ?2,"
            "         json_extract(reasoning_meta,'$.format') IS 'anthropic_content'"
            "    FROM entries WHERE id=?1"
            "  UNION ALL"
            "  SELECT e.id, e.parent_id, e.turn_id, e.iteration_id, e.role, e.type,"
            "         e.stop_reason, e.usage_in, e.tokens, e.token_estimate, e.model IS ?2,"
            "         json_extract(e.reasoning_meta,'$.format') IS 'anthropic_content'"
            "    FROM entries e JOIN up ON e.id=up.parent_id"
            ") SELECT id, turn_id, iteration_id, role, type, stop, usage_in, tokens, est,"
            "         same_model, anthropic FROM up;",
            -1, &st, NULL) != SQLITE_OK) {
        LOG_WARN_("usage attribution: %s", sqlite3_errmsg(db));
        return 0;
    }
    sqlite3_bind_int64(st, 1, asst_id);
    sqlite3_bind_text(st, 2, model, -1, SQLITE_STATIC);
    int n = 0;
    while (n < ATTR_MAX_ROWS && sqlite3_step(st) == SQLITE_ROW) {
        AttrRow *r = &rows[n];
        const char *type = (const char *)sqlite3_column_text(st, 4);
        r->id = sqlite3_column_int64(st, 0);
        r->turn_id = sqlite3_column_int64(st, 1);
        r->iteration_id = sqlite3_column_int64(st, 2);
        r->role = sqlite3_column_int(st, 3);
        r->kind = !type ? 'o' : !strcmp(type, "reasoning") ? 'r'
                : !strcmp(type, "assistant_message") ? 'a' : 'o';
        r->stop = sqlite3_column_int(st, 5);
        r->usage_in = sqlite3_column_int(st, 6);
        r->tokens = sqlite3_column_type(st, 7) == SQLITE_NULL ? -1 : sqlite3_column_int(st, 7);
        r->est = sqlite3_column_int(st, 8);
        if (r->est <= 0) r->est = 4;
        r->same_model = (char)sqlite3_column_int(st, 9);
        r->anthropic = (char)sqlite3_column_int(st, 10);
        r->skip = 0;
        if (*prev >= 0) {
            /* One row past the report: its reasoning entry, or not ours. */
            if (r->kind == 'r' && r->iteration_id == rows[*prev].iteration_id) n++;
            break;
        }
        n++;
        if (r->iteration_id == iteration_id && r->role == 2) continue;  /* this response */
        if (r->role == 4) break;                                        /* compaction */
        if (r->kind == 'a' && r->usage_in > 0 && r->stop != 4 && r->stop != 5)
            *prev = n - 1;
    }
    sqlite3_finalize(st);
    return n;
}

/* Is what is left for the appended entries plausibly their size? */
static int attr_fits(int rest, int est, int n) {
    return rest >= est / 3 - 16 && rest <= 3 * est + 48 * n + 16;
}

static void usage_attribute(sqlite3 *db, int64_t session_id, int64_t iteration_id,
                            int64_t asst_id, const char *model,
                            int in, int out, int reasoning) {
    attr_own(db, session_id, iteration_id, out, reasoning);
    if (in <= 0) return;

    AttrRow *rows = malloc(ATTR_MAX_ROWS * sizeof(AttrRow));
    if (!rows) return;
    int prev, n = attr_walk(db, asst_id, iteration_id, model ? model : "", rows, &prev);

    /* The context block this request carried, by estimate: what the session
     * holds whenever the measurement below does not replace it. */
    int ctx_est = (int)db_scalar_i64(db,
        "SELECT COALESCE(length(CAST(turn_context AS BLOB)),0)/3 FROM sessions WHERE id=?1;",
        session_id, 0);
    int ctx_old = (int)db_scalar_i64(db,
        "SELECT COALESCE(turn_context_tokens,-1) FROM sessions WHERE id=?1;", session_id, -1);
    int boundary = prev < 0 || rows[prev].turn_id != rows[0].turn_id;
    int ctx_new = boundary ? ctx_est : ctx_old;    /* inside a turn the block is frozen */
    const char *why = NULL;

    if (prev < 0) { why = "no earlier report to compare with"; goto done; }
    const AttrRow *p = &rows[prev];
    if (!p->same_model) {
        /* Another model's reasoning is stripped from every request to this
         * one (SQL_REPLAY_JOIN), and its counts are in another tokenizer. */
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db,
                "UPDATE entries SET tokens=0 WHERE session_id=?1 AND type='reasoning'"
                " AND tokens > 0 AND json_extract(reasoning_meta,'$.model') IS NOT ?2;",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, session_id);
            sqlite3_bind_text(st, 2, model ? model : "", -1, SQLITE_STATIC);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
        why = "model changed";
        goto done;
    }
    /* An edit since the previous report — a script's, the trim's, a
     * summary, at a turn's end or between two iterations, to any turn —
     * changed what that request was sent with, so the difference between
     * the two reports measures nothing. Every edit leaves its record on an
     * entry at least as new as the newest report whose ground it changed
     * (turn_edit.c: the leaf after the edit, or the note or summary it
     * wrote), so one look by id finds them all. Ids are creation order. */
    {
        sqlite3_stmt *st;
        int edited = 0;
        if (sqlite3_prepare_v2(db,
                "SELECT EXISTS(SELECT 1 FROM entries WHERE session_id=?1 AND id >= ?2"
                "  AND (role=4 OR (json_valid(data)"
                "                  AND json_type(data,'$.turn_edits')='array')));",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, session_id);
            sqlite3_bind_int64(st, 2, p->id);
            if (sqlite3_step(st) == SQLITE_ROW) edited = sqlite3_column_int(st, 0);
            sqlite3_finalize(st);
        }
        if (edited) { why = "the session was edited since the previous report"; goto done; }
    }

    /* Rows the request did not carry: a response that stopped on an error
     * and the results under it (context.c filters the same way). */
    for (int i = prev - 1; i >= 1; i--) {
        if (rows[i].kind != 'a' || (rows[i].stop != 4 && rows[i].stop != 5)) continue;
        rows[i].skip = 1;
        for (int j = i - 1; j >= 1 && rows[j].role == 3; j--) rows[j].skip = 1;
    }

    /* Between the two reports: what is already counted, what is not. */
    int known = 0, est = 0, unknown = 0;
    for (int i = prev; i >= 1; i--) {
        const AttrRow *r = &rows[i];
        if (r->skip || (r->iteration_id == iteration_id && r->role == 2)) continue;
        if (r->tokens >= 0) known += r->tokens;
        else { est += r->est; unknown++; }
    }
    /* The previous response's reasoning, and (across a boundary) the rest of
     * its turn's, which that request carried and this one may not. */
    const AttrRow *pr = prev + 1 < n ? &rows[prev + 1] : NULL;
    int own = pr && pr->tokens > 0 ? pr->tokens : 0, earlier = 0, anthropic = pr && pr->anthropic;
    if (boundary) {
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db,
                "SELECT COALESCE(SUM(tokens),0),"
                "       COALESCE(MAX(json_extract(reasoning_meta,'$.format')"
                "                    IS 'anthropic_content'),0)"
                "  FROM entries WHERE session_id=?1 AND turn_id=?2 AND type='reasoning'"
                "   AND tokens > 0 AND iteration_id IS NOT ?3;", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, session_id);
            sqlite3_bind_int64(st, 2, p->turn_id);
            sqlite3_bind_int64(st, 3, p->iteration_id);
            if (sqlite3_step(st) == SQLITE_ROW) {
                earlier = sqlite3_column_int(st, 0);
                anthropic |= sqlite3_column_int(st, 1);
            }
            sqlite3_finalize(st);
        }
    }
    int share_ctx = boundary && ctx_old >= 0;
    if (share_ctx) { est += ctx_est; unknown++; }

    /* What is left for the appended entries, under each account of the
     * reasoning: kept (the expected one wherever we replay it), the whole
     * previous turn's dropped, the previous response's alone not counted. */
    int grew = in - p->usage_in - known + (share_ctx ? ctx_old : 0);
    enum { KEEP, DROP_TURN, DROP_OWN, NONE } how = NONE;
    int rest = 0;
    struct { int how, rest, try; } h[3] = {
        { KEEP,      grew - own,     !(boundary && anthropic) },
        { DROP_TURN, grew + earlier, boundary && (own || earlier) },
        { DROP_OWN,  grew,           own > 0 && !(boundary && anthropic) },
    };
    for (int i = 0; i < 3 && how == NONE; i++)
        if (h[i].try && attr_fits(h[i].rest, est, unknown)) { how = h[i].how; rest = h[i].rest; }
    if (how == NONE) { why = "the growth in usage_in does not fit what was appended"; goto done; }
    if (rest < 0) rest = 0;

    if (how == DROP_TURN)
        attr_exec(db, "UPDATE entries SET tokens=0 WHERE session_id=?1 AND turn_id=?2"
                      " AND type='reasoning' AND tokens > 0;",
                  session_id, p->turn_id, 0);
    else if (how == DROP_OWN)
        attr_exec(db, "UPDATE entries SET tokens=0 WHERE id=?1;", pr->id, 0, 0);

    /* Shares by length; the last sharer takes what integer division left. */
    int left = rest, seen = 0;
    if (share_ctx) {
        ctx_new = ++seen == unknown ? left : (int)((int64_t)rest * ctx_est / est);
        left -= ctx_new;
    }
    for (int i = prev; i >= 1; i--) {
        const AttrRow *r = &rows[i];
        if (r->skip || r->tokens >= 0 || (r->iteration_id == iteration_id && r->role == 2))
            continue;
        int share = ++seen == unknown ? left : (int)((int64_t)rest * r->est / est);
        left -= share;
        attr_exec(db, "UPDATE entries SET tokens=?2 WHERE id=?1;",
                  r->id, share, 0);
    }
    LOG_DEBUG_("usage attribution session=%lld iteration=%lld: %d tokens over %d appended"
               " entries (estimate %d)%s", (long long)session_id, (long long)iteration_id,
               rest, unknown, est, how == KEEP ? "" : ", reasoning no longer counted");

done:
    if (why)
        LOG_DEBUG_("usage attribution session=%lld iteration=%lld: appended entries keep"
                   " their estimate (%s)", (long long)session_id, (long long)iteration_id, why);
    if (ctx_new != ctx_old)
        attr_exec(db, "UPDATE sessions SET turn_context_tokens=?2 WHERE id=?1;",
                  session_id, ctx_new, 0);
    free(rows);
}

LlmRespStatus db_ingest_response(sqlite3 *db, int64_t session_id, int64_t iteration_id,
                                 const char *model, EndpointType ep,
                                 const char *body, const char *request_body,
                                 int keep_reasoning_text, TypedIngestResult *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!db || !body) return LLM_RESP_MALFORMED;

    const char *scalar_sql = ep == ENDPOINT_GEMINI    ? SCALAR_GEMINI
                           : ep == ENDPOINT_ANTHROPIC ? SCALAR_ANTHROPIC : SCALAR_OPENAI;
    const char *tc_sql = ep == ENDPOINT_GEMINI    ? TC_GEMINI
                       : ep == ENDPOINT_ANTHROPIC ? TC_ANTHROPIC : TC_OPENAI;

    /* ── Parse the body once into a JSONB blob. Every read below binds the
     * blob (no re-parse), and the same blob is archived. Keep this statement
     * open: the blob lives in its result memory until it is finalized. ── */
    sqlite3_stmt *j;
    if (sqlite3_prepare_v2(db, "SELECT jsonb(?1)", -1, &j, NULL) != SQLITE_OK) {
        /* Our-side failure (e.g. SQLITE_BUSY), not a bad response — archive the
         * raw body so a valid reply lost to DB contention is still recoverable. */
        archive_store(db, session_id, iteration_id, model, "ingest_error", body, -1, 0, request_body, NULL);
        return LLM_RESP_DBERR;
    }
    sqlite3_bind_text(j, 1, body, -1, SQLITE_STATIC);
    if (sqlite3_step(j) != SQLITE_ROW) {
        /* Not valid JSON at all — archive the raw text for forensics. */
        sqlite3_finalize(j);
        archive_store(db, session_id, iteration_id, model, "malformed", body, -1, 0, request_body, NULL);
        return LLM_RESP_MALFORMED;
    }
    const void *blob = sqlite3_column_blob(j, 0);
    int blen = sqlite3_column_bytes(j, 0);

    /* ── Scalar fields: bind the blob, hold the statement open so the
     * extracted column pointers stay valid through the entry inserts. ── */
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, scalar_sql, -1, &s, NULL) != SQLITE_OK) {
        /* Our-side failure — archive the (valid) body for forensics + recovery. */
        archive_store(db, session_id, iteration_id, model, "ingest_error", blob, blen, 1, request_body, NULL);
        sqlite3_finalize(j);
        return LLM_RESP_DBERR;
    }
    sqlite3_bind_blob(s, 1, blob, blen, SQLITE_STATIC);
    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        archive_store(db, session_id, iteration_id, model, "malformed", blob, blen, 1, request_body, NULL);
        sqlite3_finalize(j);
        return LLM_RESP_MALFORMED;
    }

    /* Shape check: choices/candidates array present and non-empty. */
    if (sqlite3_column_type(s, 7) == SQLITE_NULL || sqlite3_column_int(s, 7) == 0) {
        sqlite3_finalize(s);
        archive_store(db, session_id, iteration_id, model, "malformed", blob, blen, 1, request_body, NULL);
        sqlite3_finalize(j);
        return LLM_RESP_MALFORMED;
    }

    const char *content   = (const char *)sqlite3_column_text(s, 0);
    const char *finish    = (const char *)sqlite3_column_text(s, 1);
    const char *reasoning = (const char *)sqlite3_column_text(s, 2);
    int prompt_tokens     = (sqlite3_column_type(s, 3) == SQLITE_INTEGER) ? sqlite3_column_int(s, 3) : 0;
    int completion_tokens = (sqlite3_column_type(s, 4) == SQLITE_INTEGER) ? sqlite3_column_int(s, 4) : 0;
    int total_tokens      = (sqlite3_column_type(s, 5) == SQLITE_INTEGER)
                                ? sqlite3_column_int(s, 5)
                                : prompt_tokens + completion_tokens;
    int64_t cost_nano = 0;
    RespUsage usage = { prompt_tokens, completion_tokens, -1, -1, -1, 0.0, 0 };
    if (sqlite3_column_type(s, 6) != SQLITE_NULL) {
        usage.cost = sqlite3_column_double(s, 6);
        usage.has_cost = 1;
        cost_nano = (int64_t)(usage.cost * 1e9 + 0.5);
    }
    if (sqlite3_column_type(s, 9) == SQLITE_INTEGER)  usage.cached_tokens = sqlite3_column_int(s, 9);
    if (sqlite3_column_type(s, 10) == SQLITE_INTEGER) usage.cache_write_tokens = sqlite3_column_int(s, 10);
    if (sqlite3_column_type(s, 11) == SQLITE_INTEGER) usage.reasoning_tokens = sqlite3_column_int(s, 11);

    /* ── Count tool calls first (need the count to set the assistant stop
     * reason before inserting it, and to keep a tool-call response off the
     * empty-stop path), then rewind for the insert pass. ── */
    sqlite3_stmt *tc = NULL;
    int tc_count = 0;
    if (sqlite3_prepare_v2(db, tc_sql, -1, &tc, NULL) == SQLITE_OK) {
        sqlite3_bind_blob(tc, 1, blob, blen, SQLITE_STATIC);
        while (sqlite3_step(tc) == SQLITE_ROW) tc_count++;
        sqlite3_reset(tc);
    }

    /* Tool call that arrived as unparsed markup in `reasoning` (E7): nothing
     * to flatten into entries, and the answer is a retry — not a silent turn. */
    if (tc_count == 0 && (!content || !content[0]) &&
        reasoning_has_tool_markup(reasoning)) {
        sqlite3_finalize(s);
        if (tc) sqlite3_finalize(tc);
        archive_store(db, session_id, iteration_id, model, "tool_markup", blob, blen, 1, request_body, NULL);
        sqlite3_finalize(j);
        return LLM_RESP_TOOL_MARKUP;
    }

    /* Zero-usage empty stop (provider glitch): no entries, retry next model. */
    if (tc_count == 0 && total_tokens == 0 && (!content || !content[0]) &&
        finish && strcmp(finish, "stop") == 0) {
        sqlite3_finalize(s);
        if (tc) sqlite3_finalize(tc);
        archive_store(db, session_id, iteration_id, model, "empty", blob, blen, 1, request_body, NULL);
        sqlite3_finalize(j);
        return LLM_RESP_EMPTY;
    }

    /* Well-formed — archive before flattening into entries. */
    archive_store(db, session_id, iteration_id, model, "ok", blob, blen, 1, request_body, &usage);

    StopReason stop = map_stop_reason(finish);
    if (tc_count > 0 && stop != STOP_REASON_TOOL_USE)
        stop = STOP_REASON_TOOL_USE;

    int part = 0;

    /* Reasoning entry (scalar pointers still valid — s untouched). Two things
     * live here: the display text — the provider's summary, or on an
     * OpenAI-compatible provider whatever reasoning text it returned — kept
     * unless the route's reasoning_display is 'none', and the replay blob,
     * which is captured unconditionally — the provider requires it back
     * verbatim on the next request, so dropping it because an operator
     * turned off reasoning *display* would break the wire (a Gemini 400 / a
     * silently invalidated prompt-cache prefix). The entry is created
     * whenever either half has something to say.
     *
     * The text is for reading, not a measure of the entry: what a reasoning
     * entry costs is its replay, which the provider's reasoning count
     * measures (usage_attribute). So its estimate stays the one an entry
     * without text has, whatever the summary's length — keeping the text
     * changes no accounting. */
    const char *replay_blob = (sqlite3_column_type(s, 8) != SQLITE_NULL)
                                  ? (const char *)sqlite3_column_text(s, 8) : NULL;
    const char *replay_text = (reasoning && reasoning[0]) ? reasoning : NULL;
    if (replay_blob || replay_text) {
        int64_t r_id = entry_append_typed(db, session_id, iteration_id, "reasoning", part++,
                                          keep_reasoning_text ? replay_text : NULL,
                                          NULL, NULL, 0,
                                          STOP_REASON_NONE, model, 0, 0, 0);
        if (r_id > 0) {
            reasoning_meta_set(db, r_id, ep, model, replay_blob, replay_text);
            attr_exec(db, "UPDATE entries SET token_estimate=?2 WHERE id=?1;",
                      r_id, TOKEN_ESTIMATE(0), 0);
        }
    }

    int64_t asst_id = entry_append_typed(db, session_id, iteration_id, "assistant_message", part++,
                                         content, NULL, NULL, 0, stop, model,
                                         prompt_tokens, completion_tokens, cost_nano);
    sqlite3_finalize(s);   /* done with content/reasoning pointers */

    /* Cache-read count lands next to usage_in on the assistant entry — that is
     * the row rate_limit_check aggregates, and the archive row it also appears
     * on may be pruned or disabled entirely. Written whenever the provider
     * reported a count, zero included (-1 = not reported), so NULL means a
     * silent provider: the limiter falls back to full weight and the cache
     * view can tell "not reported" from a miss. */
    if (asst_id > 0 && usage.cached_tokens >= 0) {
        sqlite3_stmt *cu;
        if (sqlite3_prepare_v2(db, "UPDATE entries SET cached_tokens=?1 WHERE id=?2",
                               -1, &cu, NULL) == SQLITE_OK) {
            sqlite3_bind_int(cu, 1, usage.cached_tokens);
            sqlite3_bind_int64(cu, 2, asst_id);
            sqlite3_step(cu);
            sqlite3_finalize(cu);
        }
    }
    if (asst_id < 0) {
        if (tc) sqlite3_finalize(tc);
        sqlite3_finalize(j);
        return LLM_RESP_DBERR;
    }

    /* ── Tool call entries + tool_calls workflow rows ──
     * Arguments are validated with json() and stored in entries.content (single
     * source of truth). tool_calls holds only workflow state — no arguments.
     * Invalid JSON → immediate error tool_result so the model sees feedback. */
    if (tc && tc_count > 0) {
        const char *tc_ins_sql =
            "INSERT INTO tool_calls(session_id, entry_id, call_id, name)"
            " VALUES(?1,?2,?3,?4);";
        sqlite3_stmt *ins = NULL;
        sqlite3_prepare_v2(db, tc_ins_sql, -1, &ins, NULL);

        /* Validator: json() normalizes valid JSON, returns NULL on invalid. */
        sqlite3_stmt *jval = NULL;
        sqlite3_prepare_v2(db, "SELECT json(?1)", -1, &jval, NULL);

        int idx = 0;
        while (sqlite3_step(tc) == SQLITE_ROW) {
            const char *id   = (const char *)sqlite3_column_text(tc, 0);
            const char *name = (const char *)sqlite3_column_text(tc, 1);
            const char *args = (const char *)sqlite3_column_text(tc, 2);
            /* No id on the wire (Gemini native): mint one that is unique in
             * the session. Approvals, job bookkeeping and result matching all
             * key on (session, call id); "call_gemini_<idx>" restarted at 0
             * on every response, so a later call inherited whatever an
             * earlier call with the same index had left behind — including
             * an approval. */
            char id_buf[48];
            if (!id || !id[0]) {
                snprintf(id_buf, sizeof(id_buf), "call_%lld_%d",
                         (long long)iteration_id, idx);
                id = id_buf;
            }
            const char *args_val = (args && args[0]) ? args : "{}";

            /* Validate + normalize arguments JSON */
            const char *normalized = NULL;
            if (jval) {
                sqlite3_bind_text(jval, 1, args_val, -1, SQLITE_STATIC);
                if (sqlite3_step(jval) == SQLITE_ROW)
                    normalized = (const char *)sqlite3_column_text(jval, 0);
            }

            if (!normalized) {
                /* Invalid JSON from model — emit error tool_result directly */
                entry_append_typed(db, session_id, iteration_id, "tool_call", part++,
                                   "{}", id, name,
                                   0, STOP_REASON_NONE, NULL, 0, 0, 0);
                entry_append_typed(db, session_id, iteration_id, "tool_result", part++,
                                   "error: invalid JSON in tool call arguments",
                                   id, name, 1, STOP_REASON_NONE, NULL, 0, 0, 0);
                if (jval) sqlite3_reset(jval);
                idx++;
                continue;
            }

            int64_t tc_entry = entry_append_typed(db, session_id, iteration_id, "tool_call", part++,
                                                  normalized, id, name,
                                                  0, STOP_REASON_NONE, NULL, 0, 0, 0);
            if (jval) sqlite3_reset(jval);

            if (tc_entry > 0 && ins) {
                sqlite3_bind_int64(ins, 1, session_id);
                sqlite3_bind_int64(ins, 2, tc_entry);
                sqlite3_bind_text(ins, 3, id, -1, SQLITE_STATIC);
                sqlite3_bind_text(ins, 4, name ? name : "", -1, SQLITE_STATIC);
                sqlite3_step(ins);
                sqlite3_reset(ins);
            }
            idx++;
        }
        if (jval) sqlite3_finalize(jval);
        if (ins) sqlite3_finalize(ins);
    }
    if (tc) sqlite3_finalize(tc);
    sqlite3_finalize(j);   /* blob no longer needed */

    usage_attribute(db, session_id, iteration_id, asst_id, model, prompt_tokens,
                    completion_tokens, usage.reasoning_tokens);

    if (out) {
        out->assistant_entry_id = asst_id;
        out->prompt_tokens = prompt_tokens;
        out->completion_tokens = completion_tokens;
        out->cost_nano = cost_nano;
    }
    return LLM_RESP_OK;
}
