#define _POSIX_C_SOURCE 200809L
#include "tool_session_condense.h"
#include "db.h"
#include "llm_proc.h"
#include "tool_args.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* One branch row, root→leaf. Only what the range arithmetic needs. */
typedef struct {
    int64_t id;
    int64_t turn;
} BranchRow;

/* The active branch (leaf→root by parent_id, returned root→leaf) — the same
 * walk context assembly does, so what we can condense is exactly what the
 * model can see. */
static BranchRow *branch_load(sqlite3 *db, int64_t session_id, int *out_n) {
    *out_n = 0;
    int64_t leaf = db_scalar_i64(db, "SELECT leaf_id FROM sessions WHERE id=?;",
                                 session_id, -1);
    if (leaf < 0) return NULL;

    static const char *SQL =
        "WITH RECURSIVE branch(id, parent_id, turn_id, lvl) AS ("
        "  SELECT id, parent_id, turn_id, 0 FROM entries"
        "   WHERE id=?1 AND session_id=?2"
        "  UNION ALL"
        "  SELECT e.id, e.parent_id, e.turn_id, b.lvl+1"
        "    FROM entries e JOIN branch b ON e.id=b.parent_id"
        "   WHERE b.lvl < 100000"
        ") SELECT id, COALESCE(turn_id,0) FROM branch ORDER BY lvl DESC;";

    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, SQL, -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int64(st, 1, leaf);
    sqlite3_bind_int64(st, 2, session_id);

    int cap = 64, n = 0;
    BranchRow *rows = malloc((size_t)cap * sizeof(*rows));
    while (rows && sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) {
            cap *= 2;
            BranchRow *tmp = realloc(rows, (size_t)cap * sizeof(*rows));
            if (!tmp) { free(rows); rows = NULL; break; }
            rows = tmp;
        }
        rows[n].id = sqlite3_column_int64(st, 0);
        rows[n].turn = sqlite3_column_int64(st, 1);
        n++;
    }
    sqlite3_finalize(st);
    if (!rows) return NULL;
    *out_n = n;
    return rows;
}

/* Insert + reparent as one unit: a summary entry whose successor never got
 * reparented would be an invisible duplicate of the range it replaces. */
static int64_t condense_commit(sqlite3 *db, int64_t session_id,
                               int64_t last_kept_id, int64_t first_after_id,
                               const char *content) {
    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK)
        return -1;
    int64_t cid = entry_compact(db, session_id, last_kept_id, first_after_id,
                                content);
    if (cid <= 0) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        return -1;
    }
    if (sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        return -1;
    }
    return cid;
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

    int n = 0;
    BranchRow *rows = branch_load(ctx->db, ctx->session_id, &n);
    if (!rows || n == 0) {
        free(rows); free(summary);
        return tool_fail(is_error, "error: session #%lld has no context to "
                                   "condense", (long long)ctx->session_id);
    }

    int first = -1, last = -1;
    for (int i = 0; i < n; i++) {
        if (rows[i].turn == from_turn && first < 0) first = i;
        if (rows[i].turn == to_turn) last = i;
    }
    int64_t live_turn = rows[n - 1].turn;

    char *err = NULL;
    if (first < 0 || last < 0)
        err = tool_fail(is_error, "error: turn %lld is not in this session's "
                                  "context (turns present: 1..%lld)",
                        (long long)(first < 0 ? from_turn : to_turn),
                        (long long)live_turn);
    else if (to_turn >= live_turn)
        err = tool_fail(is_error, "error: turn %lld is the live turn — the "
                                  "turn you are in cannot be condensed; "
                                  "to_turn must be %lld or earlier",
                        (long long)live_turn, (long long)(live_turn - 1));
    else if (first == 0)
        err = tool_fail(is_error, "error: turn %lld is the oldest turn in "
                                  "context — there is no earlier entry to "
                                  "attach the summary to",
                        (long long)from_turn);
    if (err) { free(rows); free(summary); return err; }

    /* Whole turns only: everything between the endpoints must belong to the
     * range, and the range must not straddle its own edges. Turn ids ascend
     * along the branch, so this holds for any well-formed range — it is the
     * backstop for a branch the reparenting of an earlier condense left in a
     * shape we did not anticipate. */
    for (int i = first; i <= last; i++) {
        if (rows[i].turn >= from_turn && rows[i].turn <= to_turn) continue;
        free(rows); free(summary);
        return tool_fail(is_error, "error: turns %lld-%lld are not a "
                                   "contiguous range in this session",
                         (long long)from_turn, (long long)to_turn);
    }

    int64_t last_kept_id = rows[first - 1].id;
    int64_t first_after_id = rows[last + 1].id;
    int condensed = last - first + 1;
    free(rows);

    /* Same coda as overflow compaction (E2): live commitments — open
     * approvals, running sub-agents, scheduled one-shots — are carried
     * mechanically rather than trusted to the summary text. Here the author
     * is the agent itself, which makes the guarantee more valuable, not less:
     * it cannot drop state by summarizing carelessly. */
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

    int64_t cid = condense_commit(ctx->db, ctx->session_id, last_kept_id,
                                  first_after_id, content);
    if (content != summary) free(content);
    free(summary);

    if (cid <= 0)
        return tool_fail(is_error, "error: could not write the compaction "
                                   "entry (DB error)");

    char *out = malloc(192);
    if (!out) return tool_fail(is_error, "error: OOM");
    snprintf(out, 192,
             "condensed turns %lld-%lld (%d entries) into compaction entry "
             "#%lld; those turns are no longer in context",
             (long long)from_turn, (long long)to_turn, condensed,
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
                     (ToolRecipe){EXEC_THREAD, SBX_NONE, condense_thread_run});
    return 0;
}
