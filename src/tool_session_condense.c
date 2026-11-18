#define _POSIX_C_SOURCE 200809L
#include "tool_session_condense.h"
#include "db.h"
#include "llm_proc.h"
#include "tool_args.h"
#include "turn_edit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Who wrote the summary, in its data.source and in the edit's record. */
#define CONDENSE_SOURCE "tool:session_condense"

static const char *CONDENSE_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"from_turn\":{\"type\":\"integer\","
    "\"description\":\"First turn to condense (a turn_id from entries)\"},"
    "\"to_turn\":{\"type\":\"integer\","
    "\"description\":\"Last turn to condense, inclusive\"},"
    "\"summary\":{\"type\":\"string\","
    "\"description\":\"The replacement text: what those turns established —"
    " goal, decisions, state, open threads. Everything you leave out is gone"
    " from your context\"}"
    "},\"required\":[\"from_turn\",\"to_turn\",\"summary\"]}";

/* The edit, as a hook would send it: one summarise of whole turns. Built by
 * SQLite so the summary's text is escaped by the JSON writer, not by hand. */
static char *condense_reply(sqlite3 *db, int64_t from_turn, int64_t to_turn,
                            const char *text) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT json_object('edits', json_array(json_object('op','summarise',"
            "  'from_turn',?1,'to_turn',?2,'text',?3)));", -1, &st, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(st, 1, from_turn);
    sqlite3_bind_int64(st, 2, to_turn);
    sqlite3_bind_text(st, 3, text, -1, SQLITE_STATIC);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) out = strdup(v);
    }
    sqlite3_finalize(st);
    return out;
}

char *tool_session_condense_handler(const char *arguments, void *user_data,
                                    int *is_error) {
    ToolCondenseCtx *ctx = (ToolCondenseCtx *)user_data;
    if (!ctx || !ctx->db) return tool_fail(is_error, "error: no db handle");

    char *summary = tool_args_str(ctx->db, arguments, "summary");
    int64_t from_turn = tool_args_int(ctx->db, arguments, "from_turn", -1);
    int64_t to_turn   = tool_args_int(ctx->db, arguments, "to_turn", -1);

    if (!summary || !summary[0]) {
        free(summary);
        return tool_fail(is_error, "error: 'summary' is required and must be "
                                   "non-empty — it replaces the turns you are "
                                   "condensing");
    }
    if (from_turn < 1 || to_turn < 1) {
        free(summary);
        return tool_fail(is_error, "error: 'from_turn' and 'to_turn' are "
                                   "required turn_id values (>= 1)");
    }
    if (from_turn > to_turn) {
        free(summary);
        return tool_fail(is_error, "error: from_turn %lld is after to_turn "
                                   "%lld — the range runs oldest to newest",
                         (long long)from_turn, (long long)to_turn);
    }

    /* Ownership: the dispatcher hands us the advancing session, but the agent
     * whose grant authorized the call must be the one that owns it. */
    char *owner = session_get_agent_name(ctx->db, ctx->session_id);
    if (!owner || strcmp(owner, ctx->agent_name) != 0) {
        char *msg = tool_fail(is_error,
                              "error: session #%lld belongs to agent '%s' — "
                              "you can only condense your own session",
                              (long long)ctx->session_id,
                              owner ? owner : "?");
        free(owner); free(summary);
        return msg;
    }
    free(owner);

    /* Live commitments — open approvals, running sub-agents, scheduled
     * one-shots — are carried mechanically rather than trusted to the
     * summary text. The author is the agent itself, which makes the
     * guarantee more valuable, not less: it cannot drop state by summarizing
     * carelessly. */
    char *coda = compaction_state_coda(ctx->db, ctx->session_id);
    char *content = summary;
    if (coda) {
        size_t sz = strlen(summary) + strlen(coda) + 8;
        char *joined = malloc(sz);
        if (joined) {
            snprintf(joined, sz, "%s\n\n%s", summary, coda);
            content = joined;
        }
        free(coda);
    }

    /* The same edit a hook's summarise is — checks, record, write path
     * (turn_edit.c) — on whole turns before the one this call is part of. */
    char *reply = condense_reply(ctx->db, from_turn, to_turn, content);
    if (content != summary) free(content);
    free(summary);
    if (!reply) return tool_fail(is_error, "error: could not build the edit (DB error)");
    char *err = NULL;
    TurnEditResult rc = turn_edit_apply(ctx->db, ctx->session_id, reply,
                                        CONDENSE_SOURCE, &err);
    free(reply);
    if (rc != TURN_EDIT_APPLIED) {
        /* The edit's reason names the op ("summarise: …"); the agent called
         * session_condense. */
        const char *why = err ? err : "nothing to condense";
        if (strncmp(why, "summarise: ", 11) == 0) why += 11;
        char *msg = tool_fail(is_error, "error: %s", why);
        free(err);
        return msg;
    }
    int64_t cid = db_scalar_i64(ctx->db,
        "SELECT MAX(id) FROM entries WHERE session_id=? AND type='compaction'"
        "   AND json_extract(data,'$.source')='" CONDENSE_SOURCE "';", ctx->session_id, -1);
    char *out = malloc(192);
    if (!out) return tool_fail(is_error, "error: OOM");
    snprintf(out, 192,
             "condensed turns %lld-%lld into compaction entry #%lld; those turns "
             "are no longer in context", (long long)from_turn, (long long)to_turn,
             (long long)cid);
    return out;
}

/* EXEC_THREAD shim: rebuild the ctx around the thread's own db handle. */
static char *condense_thread_run(sqlite3 *db, const char *agent_name,
                                 int64_t session_id, const char *args,
                                 int *is_error) {
    ToolCondenseCtx c = {.db = db, .session_id = session_id};
    snprintf(c.agent_name, sizeof(c.agent_name), "%s",
             agent_name ? agent_name : "");
    return tool_session_condense_handler(args, &c, is_error);
}

int tool_session_condense_register(ToolRegistry *reg, ToolCondenseCtx *ctx) {
    if (tools_register(reg, "session_condense",
                       "Replace a range of your own completed turns with a "
                       "summary you write, curating your history before the "
                       "context window forces automatic compaction on you. "
                       "from_turn/to_turn are entries.turn_id values (query "
                       "them with db_query on entries); the range must cover "
                       "whole turns, run oldest to newest, and end before the "
                       "turn you are in — the live turn cannot be condensed. "
                       "The condensed turns leave your context permanently: "
                       "the transcript keeps them, but you will only ever see "
                       "your summary. Condensing also invalidates the prompt "
                       "cache for everything after the cut, so the next "
                       "request pays a full re-read — condense in one "
                       "worthwhile pass, not a turn at a time.",
                       CONDENSE_PARAMS, tool_session_condense_handler,
                       ctx) != 0)
        return -1;
    tools_set_recipe(reg, "session_condense",
                     (ToolRecipe){.vehicle = EXEC_THREAD, .thread_run = condense_thread_run});
    return 0;
}
