#define _POSIX_C_SOURCE 200809L
#include "advance.h"
#include "config_registry.h"
#include "log.h"
#include "wake.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static AdvanceOutput make_output(AdvanceResult action, int64_t sid,
                                 const char *agent, int iter) {
    AdvanceOutput out = { .action = action, .session_id = sid,
                          .iteration = iter, .tc_count = 0, .calls = NULL };
    if (agent)
        snprintf(out.agent_name, sizeof(out.agent_name), "%s", agent);
    return out;
}

/* Build a richer max-iterations error message by concatenating the last few
 * substantive assistant content blocks (tool_use responses with real text)
 * before appending the error notice. Returns heap-allocated string. */
static char *rich_max_iter_message(sqlite3 *db, int64_t session_id) {
    /* Walk recent assistant entries with content, stop_reason=tool_use (3) */
    const char *sql =
        "WITH RECURSIVE branch(id, parent_id, role, stop_reason, content, lvl) AS ("
        "  SELECT id, parent_id, role, stop_reason, content, 0"
        "    FROM entries WHERE id=(SELECT leaf_id FROM sessions WHERE id=?1) AND session_id=?1"
        "  UNION ALL"
        "  SELECT e.id, e.parent_id, e.role, e.stop_reason, e.content, b.lvl+1"
        "    FROM entries e JOIN branch b ON e.id=b.parent_id WHERE b.lvl < 50"
        ") SELECT content FROM branch"
        "  WHERE role=2 AND stop_reason=3 AND content IS NOT NULL AND content != ''"
        "  ORDER BY lvl ASC LIMIT 3;";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK)
        return strdup("error: max iterations reached");
    sqlite3_bind_int64(s, 1, session_id);

    /* Collect up to 3 chunks */
    char *chunks[3] = {NULL, NULL, NULL};
    int n = 0;
    while (n < 3 && sqlite3_step(s) == SQLITE_ROW) {
        const char *c = (const char *)sqlite3_column_text(s, 0);
        if (c && c[0]) chunks[n++] = strdup(c);
    }
    sqlite3_finalize(s);

    if (n == 0) return strdup("error: max iterations reached");

    /* Concatenate oldest→newest (the branch walk collects leaf-first, so
     * iterate chunks in reverse): chunk3\n\n---\n\nchunk2\n\n---\n\n...error */
    const char *sep = "\n\n---\n\n";
    const char *tail = "\n\n---\n\nerror: max iterations reached";
    size_t len = strlen(tail) + 1;
    for (int i = 0; i < n; i++)
        len += strlen(chunks[i]) + (i > 0 ? strlen(sep) : 0);

    char *buf = malloc(len);
    if (!buf) {
        for (int i = 0; i < n; i++) free(chunks[i]);
        return strdup("error: max iterations reached");
    }
    buf[0] = '\0';
    for (int i = n - 1; i >= 0; i--) {
        if (i < n - 1) strcat(buf, sep);
        strcat(buf, chunks[i]);
        free(chunks[i]);
    }
    strcat(buf, tail);
    return buf;
}

/* Stamp the child as delivered. Inside the notify transaction, so the stamp and
 * the write it records land together: a lost transaction leaves NULL, and that
 * NULL is the durable "push never landed" fact advance_sweep_unnotified reads. */
static void stamp_notified(sqlite3 *db, int64_t session_id) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "UPDATE sessions SET parent_notified_at=unixepoch() WHERE id=?",
            -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, session_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* Did this session's turn end on an error? entries.stop_reason is an INTEGER
 * column: reading it as text and comparing to "error" (as this did until
 * 2026-08-01) never matched, so every errored child told its parent it
 * succeeded. Shared by the llm_running terminal branch and the sweep — the
 * sweep only looks at sessions whose leaf is this very entry. */
static int session_stopped_with_error(sqlite3 *db, int64_t session_id) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT stop_reason FROM entries WHERE session_id=?"
            " AND role=2 ORDER BY id DESC LIMIT 1", -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int64(st, 1, session_id);
    int is_error = (sqlite3_step(st) == SQLITE_ROW) &&
                   sqlite3_column_int(st, 0) == STOP_REASON_ERROR;
    sqlite3_finalize(st);
    return is_error;
}

/* Notify a sub-agent's parent that the child finished. Blocking mode writes a
 * ToolResult for the parent's launch_agent call; background mode posts to the
 * parent inbox. Called on every terminal path (normal stop, error, max-iter) so
 * a parent blocked on launch_agent always gets a result and never parks forever.
 * On is_error the parent receives the error text with is_error=1. */
void advance_notify_parent(sqlite3 *db, int64_t session_id, int is_error) {
    SessionParentInfo pi = session_get_parent_info(db, session_id);
    if (pi.parent_session_id <= 0) {
        free(pi.parent_tool_call_id);
        return;
    }

    char *result_text = get_response_text(db, session_id);
    if (!result_text)
        result_text = strdup(is_error ? "error: sub-agent terminated abnormally"
                                      : "(no response)");

    LOG_INFO_("advance notify_parent session=%lld parent=%lld is_error=%d", (long long)session_id, (long long)pi.parent_session_id, is_error);

    int txn_ok = (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    if (!txn_ok)
        LOG_WARN_("advance notify_parent BEGIN failed child=%lld parent=%lld"
                  " — parent_notified_at stays NULL, sweep will retry",
                  (long long)session_id, (long long)pi.parent_session_id);
    if (txn_ok) {
        if (pi.parent_tool_call_id) {
            /* Blocking mode: write tool result for parent's tool call */
            ToolResult tr = { .tool_call_id = pi.parent_tool_call_id,
                              .content = result_text };
            Message rmsg = { .role = ROLE_TOOL, .tool_result = &tr,
                             .tool_name = "launch_agent", .is_error = is_error };
            int64_t rid = entry_append_with_iteration(db, pi.parent_session_id, &rmsg, 0);
            db_tool_call_complete_by_call(db, pi.parent_session_id,
                                          pi.parent_tool_call_id, rid);
            /* Unpark parent — but only from a state that permits it. A parent
             * sitting in awaiting_approval or compacting must not be stomped
             * back to tool_running (that double-prompts / re-emits per sibling
             * completion); the eventual wake re-advances it from its own state. */
            char pstate[32] = {0};
            sqlite3_stmt *ps;
            if (sqlite3_prepare_v2(db, "SELECT state FROM sessions WHERE id=?",
                                   -1, &ps, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(ps, 1, pi.parent_session_id);
                if (sqlite3_step(ps) == SQLITE_ROW) {
                    const char *s = (const char *)sqlite3_column_text(ps, 0);
                    if (s) snprintf(pstate, sizeof(pstate), "%s", s);
                }
                sqlite3_finalize(ps);
            }
            if (strcmp(pstate, "awaiting_approval") != 0 &&
                strcmp(pstate, "compacting") != 0)
                session_set_state(db, pi.parent_session_id, "tool_running");
        } else {
            /* Background mode: notice into the parent inbox. The prefix is
             * glued on in SQL so the result arrives intact at any length —
             * a fixed buffer here once cost a user half their answer. The
             * child session id is structural provenance (source_ref), not
             * prose; the drain re-attaches it as a [tag] the model can read. */
            char child_ref[24];
            snprintf(child_ref, sizeof(child_ref), "%lld", (long long)session_id);
            sqlite3_stmt *ins;
            if (sqlite3_prepare_v2(db,
                    "INSERT INTO inbox (session_id, source, source_ref, payload)"
                    " VALUES (?1, 'agent_result', ?2, 'Sub-agent ' || ?3 || ': ' || ?4)",
                    -1, &ins, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(ins, 1, pi.parent_session_id);
                sqlite3_bind_text(ins, 2, child_ref, -1, SQLITE_STATIC);
                sqlite3_bind_text(ins, 3, is_error ? "failed" : "completed",
                                  -1, SQLITE_STATIC);
                sqlite3_bind_text(ins, 4, result_text, -1, SQLITE_STATIC);
                sqlite3_step(ins);
                sqlite3_finalize(ins);
            }
        }

        stamp_notified(db, session_id);

        if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            txn_ok = 0;
            LOG_WARN_("advance notify_parent COMMIT failed child=%lld parent=%lld"
                      " — result rolled back, sweep will retry",
                      (long long)session_id, (long long)pi.parent_session_id);
        }
    }

    if (txn_ok)
        wake_session(pi.parent_session_id);

    free(result_text);
    free(pi.parent_tool_call_id);
}

/* Bounded work per tick — the next tick takes whatever is left. */
#define SWEEP_MAX 64

int advance_sweep_unnotified(sqlite3 *db) {
    /* A terminal child whose push never landed: idle, has a parent, no stamp,
     * and an assistant leaf — role 2 means a turn actually ended, so a child
     * that was spawned but never ran (role-0/1 leaf) is left alone. Collect the
     * ids before notifying: an open SELECT pins a WAL read snapshot, and the
     * notify below writes (see cron.c's DueJob collection for the same shape). */
    const char *sql =
        "SELECT s.id FROM sessions s"
        " WHERE s.state='idle' AND s.parent_session_id > 0"
        "   AND s.parent_notified_at IS NULL"
        "   AND EXISTS (SELECT 1 FROM entries e"
        "               WHERE e.id=s.leaf_id AND e.session_id=s.id AND e.role=2)"
        " LIMIT ?;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, SWEEP_MAX);
    int64_t ids[SWEEP_MAX];
    int n = 0;
    while (n < SWEEP_MAX && sqlite3_step(st) == SQLITE_ROW)
        ids[n++] = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);

    for (int i = 0; i < n; i++) {
        /* A notify that needed the sweep is a symptom, not routine. */
        LOG_WARN_("advance sweep re-notifying child=%lld (push was lost)",
                  (long long)ids[i]);
        advance_notify_parent(db, ids[i], session_stopped_with_error(db, ids[i]));
    }
    return n;
}

/* Iterations already spent in the leaf's turn: the number of distinct LLM
 * requests inside it. Derived, never shadowed — the bounce that stranded the
 * turn also zeroed sessions.turn_iteration, and the entries are the truth.
 * Entries with no request behind them (inbox-drained user messages: NULL, or 0)
 * are not iterations and don't count. */
static int turn_iterations_spent(sqlite3 *db, int64_t session_id) {
    return (int)db_scalar_i64(db,
        "SELECT COUNT(DISTINCT iteration_id) FROM entries"
        " WHERE session_id=?1 AND iteration_id > 0 AND turn_id="
        "  (SELECT turn_id FROM entries"
        "    WHERE id=(SELECT leaf_id FROM sessions WHERE id=?1));",
        session_id, 0);
}

/* ── Cross-turn runaway guard ─────────────────────────────────────
 * Turns generating turns with no human in the causal chain — cron chatter,
 * respawn ping-pong — are the one loop max_iterations cannot bound: each turn
 * is legal on its own. The guard lives at the turn open, the single choke
 * point every turn passes, and refuses *before* the drain so a refusal leaves
 * the inbox rows queued and durable: nothing is lost, the session goes quiet
 * until a human replies. Classification and the streak SQL live in advance.h;
 * there is no counter column — state has one home, and it is the entries. */
typedef struct {
    int cap;      /* max_autonomous_turn_streak; <= 0 disables the guard */
    int streak;   /* consecutive autonomous turns already on record */
    int refuse;   /* this turn must not open */
    int warn;     /* open it, but teach: we are near the cap */
} StreakGate;

static StreakGate streak_gate_check(sqlite3 *db, int64_t session_id) {
    StreakGate g = { .cap = config_get_int(db, "max_autonomous_turn_streak") };
    if (g.cap <= 0) return g;
    if (!db_scalar_i64(db, "SELECT " CCLAW_SQL_INBOX_ALL_AUTONOMOUS("?1") ";",
                       session_id, 0))
        return g;   /* nothing queued, or a human is in the batch */
    g.streak = (int)db_scalar_i64(db,
        "SELECT " CCLAW_SQL_AUTONOMOUS_STREAK("?1") ";", session_id, 0);
    if (g.streak >= g.cap) g.refuse = 1;
    /* Teach at ~80% of the cap, counting the turn about to open. Integer
     * math: (streak + 1) / cap >= 4/5. */
    else if ((g.streak + 1) * 5 >= g.cap * 4) g.warn = 1;
    return g;
}

/* One role-0 system entry on the session, stamped so it stays queryable after
 * compaction eats the prose. Role 0 is load-bearing twice over: the streak
 * reads role 1, so this can never be mistaken for a human turn, and a role-0
 * leaf is a legal resting place, so it never creates a turn to resume.
 * Caller holds the turn-open transaction. */
static void streak_note(sqlite3 *db, int64_t session_id, const char *note,
                        const char *text) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO entries (parent_id, session_id, iteration_id, type, role,"
            " content, token_estimate, content_bytes, data)"
            " VALUES ((SELECT leaf_id FROM sessions WHERE id=?1),?1,0,'system',0,"
            "         ?2,?3,?4, json_object('source','streak_guard','note',?5));",
            -1, &st, NULL) != SQLITE_OK)
        return;
    int len = (int)strlen(text);
    sqlite3_bind_int64(st, 1, session_id);
    sqlite3_bind_text(st, 2, text, len, SQLITE_STATIC);
    sqlite3_bind_int(st, 3, (len / 4) + 4);
    sqlite3_bind_int(st, 4, len);
    sqlite3_bind_text(st, 5, note, -1, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* Refuse this turn open. The one write a refusal makes is the trip marker,
 * and only on the *first* refusal of a trip: once-per-trip is derived, not
 * stored — a 'tripped' entry newer than the last human turn means this trip
 * already announced itself, and a human turn later starts a fresh one. */
static void streak_refuse(sqlite3 *db, int64_t session_id, const StreakGate *g) {
    /* Debug, not info: a tripped session is re-woken every daemon tick, and
     * run_advance already logs the resulting noop. The WARN below fires once. */
    LOG_DEBUG_("advance next=idle reason=autonomous_streak streak=%d cap=%d",
               g->streak, g->cap);
    if (db_scalar_i64(db,
            "SELECT EXISTS(SELECT 1 FROM entries WHERE session_id=?1 AND role=0"
            "  AND json_extract(data,'$.source')='streak_guard'"
            "  AND json_extract(data,'$.note')='tripped'"
            "  AND turn_id > " CCLAW_SQL_LAST_HUMAN_TURN("?1") ");",
            session_id, 0))
        return;

    LOG_WARN_("advance autonomous-turn guard tripped session=%lld streak=%d"
              " cap=%d — autonomous turns refused until a human replies",
              (long long)session_id, g->streak, g->cap);
    char text[256];
    snprintf(text, sizeof(text),
             "This agent has hit its autonomous-turn limit (%d consecutive"
             " turns with no human input) and is paused until someone replies.",
             g->streak);
    streak_note(db, session_id, "tripped", text);

    /* Same INSERT shape deliver_response uses; no-op for a session with no
     * chat binding. No FIFO wake from here — advance.c has no db_path (that
     * is main.c's g_cfg), and the channel runner re-drains its outbox on
     * every ~1s loop iteration, which is soon enough for a once-per-trip
     * notice. */
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO channel_outbox(channel_name, session_id, payload)"
            " SELECT channel_name, id,"
            "        json_object('chat_id', COALESCE(chat_id,'0'), 'text', ?2)"
            "   FROM sessions WHERE id=?1 AND channel_name IS NOT NULL;",
            -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, session_id);
    sqlite3_bind_text(st, 2, text, -1, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* Teach at the near edge: one system note inside the turn just opened. It is
 * appended after the drain, so it is the leaf — which also means a dispatch
 * that bounces on this very turn leaves a role-0 leaf the sweep's role-1/3
 * resume predicate skips. Accepted: the entries are durable and the next
 * autonomous event (this session is producing them by definition) re-opens
 * the session with them still in context. */
static void streak_warn(sqlite3 *db, int64_t session_id, const StreakGate *g) {
    char text[256];
    snprintf(text, sizeof(text),
             "This is autonomous turn %d in a row (limit %d). No human has been"
             " in the loop — wind down, checkpoint your state, or send your"
             " operator a summary.", g->streak + 1, g->cap);
    streak_note(db, session_id, "warn", text);
}

AdvanceOutput advance_session(sqlite3 *db, int64_t session_id, int max_iterations) {
    if (max_iterations <= 0) max_iterations = config_default_int("max_iterations");

    /* Read agent_name, state, turn_iteration in one query */
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
            "SELECT agent_name, state, turn_iteration FROM sessions WHERE id=?",
            -1, &stmt, NULL) != SQLITE_OK)
        return make_output(ADVANCE_ERROR, session_id, NULL, 0);
    sqlite3_bind_int64(stmt, 1, session_id);

    char *agent = NULL;
    char state[32] = {0};
    int iter = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *a = (const char *)sqlite3_column_text(stmt, 0);
        const char *s = (const char *)sqlite3_column_text(stmt, 1);
        if (a) agent = strdup(a);
        if (s) snprintf(state, sizeof(state), "%s", s);
        iter = sqlite3_column_int(stmt, 2);
    }
    sqlite3_finalize(stmt);

    if (!agent)
        return make_output(ADVANCE_ERROR, session_id, NULL, 0);
    if (!state[0]) { free(agent); return make_output(ADVANCE_ERROR, session_id, NULL, 0); }

    /* ── State machine ───────────────────────────────────── */

    if (strcmp(state, "awaiting_approval") == 0) {
        AdvanceOutput out = make_output(ADVANCE_WAITING, session_id, agent, iter);
        free(agent);
        return out;
    }

    if (strcmp(state, "idle") == 0) {
        /* Idle: atomically consume inbox + flip to llm_running */
        if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
            free(agent);
            return make_output(ADVANCE_ERROR, session_id, NULL, 0);
        }
        /* Turn-start reconciliation (D11). Idle means no turn is in flight, so
         * any tool_call still pending/running is a zombie the turn-join says
         * can't exist — close it before it can be re-dispatched with stale
         * arguments ahead of this turn's own calls. Inside the turn-open
         * transaction, and *before* the consume: the error result must parent
         * to the assistant entry that made the call, so it stays adjacent and
         * inside that call's turn instead of landing after the new user entry. */
        int stale = db_reconcile_stale_calls(db, session_id);
        if (stale < 0) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            free(agent);
            return make_output(ADVANCE_ERROR, session_id, NULL, 0);
        }
        if (stale > 0)
            LOG_WARN_("advance reconciled %d stale tool_call(s) at turn start", stale);
        /* Runaway guard, before the drain. It can only fire when there *is*
         * queued work, so the unanswered-leaf resume below is never gated —
         * a stranded turn resuming doesn't multiply. */
        StreakGate gate = streak_gate_check(db, session_id);
        if (gate.refuse) {
            streak_refuse(db, session_id, &gate);
            if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
                sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            AdvanceOutput out = make_output(ADVANCE_NOOP, session_id, agent, iter);
            free(agent);
            return out;
        }
        int consumed = inbox_consume_into_entries_locked(db, session_id, 100);
        int leaf_role = 0;
        if (consumed == 0) {
            /* Empty inbox, but the leaf may be unanswered — role 1: a refused
             * dispatch (rate limit, disk floor, full worker pool) consumed the
             * inbox into entries, then parked the session idle. Role 3: a
             * mid-turn LLM dispatch bounce reverted the session to idle, or D11
             * closed a zombie call. Both are a turn waiting on a request nobody
             * made; without this check they are stranded forever. Role 0/2/4
             * leaves are legal resting places and stay put. */
            sqlite3_stmt *ls;
            if (sqlite3_prepare_v2(db,
                    "SELECT role FROM entries"
                    " WHERE id=(SELECT leaf_id FROM sessions WHERE id=?1)"
                    "  AND session_id=?1 AND role IN (1,3)",
                    -1, &ls, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(ls, 1, session_id);
                if (sqlite3_step(ls) == SQLITE_ROW) {
                    leaf_role = sqlite3_column_int(ls, 0);
                    consumed = 1;
                }
                sqlite3_finalize(ls);
            }
            if (consumed > 0)
                LOG_INFO_("advance state=idle next=llm_running reason=unanswered_leaf role=%d",
                          leaf_role);
        } else if (consumed > 0) {
            LOG_INFO_("advance state=idle next=llm_running inbox=%d", consumed);
            if (gate.warn) streak_warn(db, session_id, &gate);
        }
        /* A role-3 leaf continues the turn it was already in; everything else
         * opens a new one at iteration 0. Resuming re-derives the count rather
         * than preserving it — session_set_state(…,'idle') zeroed
         * turn_iteration at the bounce — so the max_iterations budget stays
         * honest and the dispatch (iteration > 0 ⇒ recall 0) reuses the frozen
         * turn_context instead of rebuilding it mid-turn. */
        int resume_iter = (leaf_role == ROLE_TOOL)
                        ? turn_iterations_spent(db, session_id) : 0;
        if (consumed > 0) {
            session_set_iteration(db, session_id, resume_iter);
            session_set_state(db, session_id, "llm_running");
        }
        if (consumed < 0) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            free(agent);
            return make_output(ADVANCE_ERROR, session_id, NULL, 0);
        }
        if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            free(agent);
            return make_output(ADVANCE_ERROR, session_id, NULL, 0);
        }
        if (consumed > 0) {
            AdvanceOutput out = make_output(ADVANCE_DISPATCH_LLM, session_id, agent,
                                            resume_iter);
            free(agent);
            return out;
        }
        AdvanceOutput out = make_output(ADVANCE_NOOP, session_id, agent, iter);
        free(agent);
        return out;
    }

    if (strcmp(state, "llm_running") == 0) {
        /* A wake while llm_running is only a *completion* once the job row is
         * gone — the worker deletes it (llm_worker.c) before notifying. A
         * redundant wake (e.g. resolve_approval's explicit run_advance racing
         * its own wake_session ping through the CLI's wake-pipe drain) can
         * land here before the just-submitted job has even run; tc_count==0
         * at that point means "no tool calls yet", not "plain-text answer",
         * so without this check the turn is misdiagnosed as done and the
         * final response is dropped. Stay parked and wait for the real
         * signal, matching the "compacting" branch below. */
        int job_in_flight = 0;
        {   sqlite3_stmt *js;
            if (sqlite3_prepare_v2(db,
                    "SELECT 1 FROM llm_jobs WHERE session_id=? AND job_type=0 LIMIT 1",
                    -1, &js, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(js, 1, session_id);
                if (sqlite3_step(js) == SQLITE_ROW) job_in_flight = 1;
                sqlite3_finalize(js);
            }
        }
        if (job_in_flight) {
            AdvanceOutput out = make_output(ADVANCE_WAITING, session_id, agent, iter);
            free(agent);
            return out;
        }

        /* LLM finished — check what to do next */
        int tc_count = 0;
        PendingToolCall *calls = db_tool_call_get_pending(db, session_id, &tc_count);
        if (tc_count > 0) {
            /* Has pending tool calls */
            session_set_state(db, session_id, "tool_running");
            LOG_INFO_("advance state=llm_running next=tool_running tools=%d", tc_count);
            AdvanceOutput out = make_output(ADVANCE_DISPATCH_TOOLS, session_id, agent, iter);
            out.tc_count = tc_count;
            out.calls = calls;
            free(agent);
            return out;
        }
        db_tool_call_free_pending(calls, tc_count);

        /* Check if last assistant entry was an error */
        int is_error = session_stopped_with_error(db, session_id);

        if (is_error) {
            session_set_state(db, session_id, "idle");
            LOG_INFO_("advance state=llm_running next=idle reason=error");
            /* Abnormal stop: still notify a waiting parent so a blocking
             * launch_agent gets an error result instead of hanging. */
            advance_notify_parent(db, session_id, 1);
            AdvanceOutput out = make_output(ADVANCE_ERROR, session_id, agent, iter);
            free(agent);
            return out;
        }

        /* Normal stop — turn complete */
        session_set_state(db, session_id, "idle");
        LOG_INFO_("advance state=llm_running next=idle reason=done");

        /* Notify parent session if this is a sub-agent */
        advance_notify_parent(db, session_id, 0);

        AdvanceOutput out = make_output(ADVANCE_DONE, session_id, agent, iter);
        free(agent);
        return out;
    }

    if (strcmp(state, "tool_running") == 0) {
        /* All tools done — check for more, then back to LLM */
        int tc_count = 0;
        PendingToolCall *calls = db_tool_call_get_pending(db, session_id, &tc_count);
        if (tc_count > 0) {
            /* More tools pending (parallel tool calls) */
            AdvanceOutput out = make_output(ADVANCE_DISPATCH_TOOLS, session_id, agent, iter);
            out.tc_count = tc_count;
            out.calls = calls;
            free(agent);
            return out;
        }
        db_tool_call_free_pending(calls, tc_count);

        /* No un-dispatched calls left, but async ones (forked tools or
         * sub-agents) may still be in flight. Stay in tool_running and wait;
         * each completion wakes us again and re-checks. The turn only proceeds
         * to the LLM once every tool_call has a result. */
        if (db_tool_call_any_running(db, session_id)) {
            AdvanceOutput out = make_output(ADVANCE_WAITING, session_id, agent, iter);
            free(agent);
            return out;
        }

        /* All tools done — dispatch next LLM iteration */
        int new_iter = session_bump_iteration(db, session_id);
        if (new_iter >= max_iterations) {
            /* Hit iteration cap */
            LOG_INFO_("advance state=tool_running next=idle reason=max_iterations iter=%d max=%d", new_iter, max_iterations);
            char *rich_msg = rich_max_iter_message(db, session_id);
            Message msg = { .role = ROLE_ASSISTANT,
                            .content = rich_msg,
                            .stop_reason = STOP_REASON_ERROR };
            entry_append_with_iteration(db, session_id, &msg, 0);
            free(rich_msg);
            session_set_state(db, session_id, "idle");
            /* Max-iter is a terminal error for a sub-agent too — notify parent. */
            advance_notify_parent(db, session_id, 1);
            AdvanceOutput out = make_output(ADVANCE_DONE, session_id, agent, new_iter);
            free(agent);
            return out;
        }
        session_set_state(db, session_id, "llm_running");
        LOG_INFO_("advance state=tool_running next=llm_running iter=%d", new_iter);
        AdvanceOutput out = make_output(ADVANCE_DISPATCH_LLM, session_id, agent, new_iter);
        free(agent);
        return out;
    }

    if (strcmp(state, "compacting") == 0) {
        /* A wake while compacting is only a *completion* once the compaction
         * job row is gone — the worker deletes it (llm_worker.c) before
         * notifying. Any other wake (inbox insert, sibling sub-agent finish)
         * must not flip us to idle and consume the inbox while the worker is
         * still mutating the branch; stay parked and wait for the real signal. */
        int compaction_in_flight = 0;
        {   sqlite3_stmt *js;
            if (sqlite3_prepare_v2(db,
                    "SELECT 1 FROM llm_jobs WHERE session_id=? AND job_type=1 LIMIT 1",
                    -1, &js, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(js, 1, session_id);
                if (sqlite3_step(js) == SQLITE_ROW) compaction_in_flight = 1;
                sqlite3_finalize(js);
            }
        }
        if (compaction_in_flight) {
            AdvanceOutput out = make_output(ADVANCE_WAITING, session_id, agent, iter);
            free(agent);
            return out;
        }
        /* Compaction finished — atomically go idle + check inbox */
        if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
            free(agent);
            return make_output(ADVANCE_ERROR, session_id, NULL, 0);
        }
        session_set_state(db, session_id, "idle");
        /* The idle branch's twin: this is the other place a turn opens off the
         * inbox, so it takes the same guard. The session stays idle (already
         * set above — compaction is done either way). */
        StreakGate gate = streak_gate_check(db, session_id);
        if (gate.refuse) {
            streak_refuse(db, session_id, &gate);
            if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
                sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            AdvanceOutput out = make_output(ADVANCE_NOOP, session_id, agent, 0);
            free(agent);
            return out;
        }
        int consumed = inbox_consume_into_entries_locked(db, session_id, 100);
        if (consumed > 0) {
            if (gate.warn) streak_warn(db, session_id, &gate);
            session_set_iteration(db, session_id, 0);
            session_set_state(db, session_id, "llm_running");
            LOG_INFO_("advance state=compacting next=llm_running inbox=%d", consumed);
        }
        if (consumed < 0) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            free(agent);
            return make_output(ADVANCE_ERROR, session_id, NULL, 0);
        }
        if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
            sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            free(agent);
            return make_output(ADVANCE_ERROR, session_id, NULL, 0);
        }
        if (consumed > 0) {
            AdvanceOutput out = make_output(ADVANCE_DISPATCH_LLM, session_id, agent, 0);
            free(agent);
            return out;
        }
        LOG_INFO_("advance state=compacting next=idle reason=done");
        AdvanceOutput out = make_output(ADVANCE_NOOP, session_id, agent, 0);
        free(agent);
        return out;
    }

    if (strcmp(state, "rate_limited") == 0) {
        session_set_state(db, session_id, "idle");
        LOG_INFO_("advance state=rate_limited next=idle");
        AdvanceOutput out = make_output(ADVANCE_NOOP, session_id, agent, iter);
        free(agent);
        return out;
    }

    /* Unknown state */
    free(agent);
    return make_output(ADVANCE_ERROR, session_id, NULL, iter);
}
