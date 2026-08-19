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

/* The cutoff notice must carry the norm at the moment it matters: the limit
 * that fired, that work was cut mid-flight (not finished, not denied), and
 * that picking it back up next turn is legal. */
#define MAX_ITER_NOTICE_FMT \
    "error: iteration limit reached (%d per turn) — work was cut mid-flight," \
    " not finished. Continuing where you left off next turn is legal and" \
    " expected."

static char *max_iter_notice(int max_iterations) {
    char buf[256];
    snprintf(buf, sizeof(buf), MAX_ITER_NOTICE_FMT, max_iterations);
    return strdup(buf);
}

/* Build a richer max-iterations error message by concatenating the last few
 * substantive assistant content blocks (tool_use responses with real text)
 * before appending the error notice. Returns heap-allocated string. */
static char *rich_max_iter_message(sqlite3 *db, int64_t session_id, int max_iterations) {
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
        return max_iter_notice(max_iterations);
    sqlite3_bind_int64(s, 1, session_id);

    /* Collect up to 3 chunks */
    char *chunks[3] = {NULL, NULL, NULL};
    int n = 0;
    while (n < 3 && sqlite3_step(s) == SQLITE_ROW) {
        const char *c = (const char *)sqlite3_column_text(s, 0);
        if (c && c[0]) chunks[n++] = strdup(c);
    }
    sqlite3_finalize(s);

    if (n == 0) return max_iter_notice(max_iterations);

    /* Concatenate oldest→newest (the branch walk collects leaf-first, so
     * iterate chunks in reverse): chunk3\n\n---\n\nchunk2\n\n---\n\n...error */
    const char *sep = "\n\n---\n\n";
    char tail[280];
    snprintf(tail, sizeof(tail), "\n\n---\n\n" MAX_ITER_NOTICE_FMT, max_iterations);
    size_t len = strlen(tail) + 1;
    for (int i = 0; i < n; i++)
        len += strlen(chunks[i]) + (i > 0 ? strlen(sep) : 0);

    char *buf = malloc(len);
    if (!buf) {
        for (int i = 0; i < n; i++) free(chunks[i]);
        return max_iter_notice(max_iterations);
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

/* ── Edge delivery (specs/delivery.md) ────────────────────────────
 * A session's outbound edges are delivery_edges rows, evaluated here at its
 * turn boundaries (and mid-turn for 'iteration'). The edge cursor, stamped
 * inside the delivery transaction, is the only delivery state: a lost
 * transaction leaves it behind the leaf, which is exactly the predicate
 * advance_sweep_undelivered re-derives from. Pushes are latency, the sweep
 * is the guarantee. */

typedef struct {
    int64_t id;
    int64_t cursor;
    int one_shot;
    char kind[16];
    char policy[16];
    char ref[192];       /* parent session id / channel name / tool call_id */
} DeliveryEdge;

#define EDGE_MAX 8

static int edges_load(sqlite3 *db, int64_t session_id, DeliveryEdge *out) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT id, cursor, one_shot, target_kind, policy, target_ref"
            " FROM delivery_edges WHERE session_id=?"
            " ORDER BY one_shot DESC, id;", -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, session_id);
    int n = 0;
    while (n < EDGE_MAX && sqlite3_step(st) == SQLITE_ROW) {
        DeliveryEdge *e = &out[n];
        e->id = sqlite3_column_int64(st, 0);
        e->cursor = sqlite3_column_int64(st, 1);
        e->one_shot = sqlite3_column_int(st, 2);
        const char *s = (const char *)sqlite3_column_text(st, 3);
        snprintf(e->kind, sizeof(e->kind), "%s", s ? s : "");
        s = (const char *)sqlite3_column_text(st, 4);
        snprintf(e->policy, sizeof(e->policy), "%s", s ? s : "");
        s = (const char *)sqlite3_column_text(st, 5);
        snprintf(e->ref, sizeof(e->ref), "%s", s ? s : "");
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

static void edge_cursor_set(sqlite3 *db, int64_t edge_id, int64_t cursor) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "UPDATE delivery_edges SET cursor=?1 WHERE id=?2;",
            -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, cursor);
    sqlite3_bind_int64(st, 2, edge_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

/* Content-bearing assistant prose in (cursor, ∞), oldest→newest, joined with
 * the rich_max_iter_message separator. Session-scoped by id order rather than
 * a branch walk: mid-turn entries are linear, and a digest window spanning a
 * compaction re-parent is not worth a CTE here. NULL if none. */
static char *digest_since(sqlite3 *db, int64_t session_id, int64_t cursor) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT content FROM entries"
            " WHERE session_id=?1 AND id>?2 AND role=2"
            "   AND content IS NOT NULL AND content != ''"
            "   AND type NOT IN ('tool_call','reasoning')"
            " ORDER BY id;", -1, &st, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(st, 1, session_id);
    sqlite3_bind_int64(st, 2, cursor);
    const char *sep = "\n\n---\n\n";
    char *buf = NULL;
    size_t len = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *c = (const char *)sqlite3_column_text(st, 0);
        if (!c || !c[0]) continue;
        size_t cl = strlen(c), sl = len ? strlen(sep) : 0;
        char *nb = realloc(buf, len + sl + cl + 1);
        if (!nb) { free(buf); sqlite3_finalize(st); return NULL; }
        buf = nb;
        if (sl) memcpy(buf + len, sep, sl);
        memcpy(buf + len + sl, c, cl + 1);
        len += sl + cl;
    }
    sqlite3_finalize(st);
    return buf;
}

/* One notice into the parent inbox. The prefix is glued on in SQL so the
 * result arrives intact at any length — a fixed buffer here once cost a user
 * half their answer. The child session id is structural provenance
 * (source_ref), not prose; the drain re-attaches it as a [tag]. */
static void parent_push(sqlite3 *db, int64_t parent_sid, int64_t child_sid,
                        const char *label, const char *text) {
    char child_ref[24];
    snprintf(child_ref, sizeof(child_ref), "%lld", (long long)child_sid);
    sqlite3_stmt *ins;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO inbox (session_id, source, source_ref, payload)"
            " VALUES (?1, 'agent_result', ?2, 'Sub-agent ' || ?3 || ': ' || ?4)",
            -1, &ins, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(ins, 1, parent_sid);
    sqlite3_bind_text(ins, 2, child_ref, -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 3, label, -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 4, text, -1, SQLITE_STATIC);
    sqlite3_step(ins);
    sqlite3_finalize(ins);
}

/* One outbox row toward the session's bound chat (deliver_response's shape).
 * Error dedup (model-routing.md R4): an unattended source (cron, chatter)
 * during a provider outage produces the same "error: ..." answer every turn;
 * deliver the first, suppress identical repeats until a human message
 * intervenes (they deserve a fresh answer, even the same bad news). Success
 * text is never deduped, so recovery speaks by itself. */
static void channel_push(sqlite3 *db, const char *channel, int64_t session_id,
                         const char *chat_id, const char *text) {
    if (text && strncmp(text, "error: ", 7) == 0) {
        sqlite3_stmt *chk;
        if (sqlite3_prepare_v2(db,
                "SELECT 1 FROM channel_outbox o"
                " WHERE o.channel_name=?1 AND o.session_id=?2"
                " AND json_extract(o.payload,'$.text')=?3"
                " AND o.id=(SELECT MAX(id) FROM channel_outbox"
                "            WHERE channel_name=?1 AND session_id=?2)"
                /* human sources are named by channel ('discord', 'cli'...);
                 * the machinery's own sources are the fixed set below */
                " AND NOT EXISTS (SELECT 1 FROM inbox i"
                "   WHERE i.session_id=?2 AND i.created_at >= o.created_at"
                "   AND i.source NOT IN ('cron','cron_error','system',"
                "                        'approval','agent_result'))",
                -1, &chk, NULL) == SQLITE_OK) {
            sqlite3_bind_text(chk, 1, channel, -1, SQLITE_STATIC);
            sqlite3_bind_int64(chk, 2, session_id);
            sqlite3_bind_text(chk, 3, text, -1, SQLITE_STATIC);
            int dup = sqlite3_step(chk) == SQLITE_ROW;
            sqlite3_finalize(chk);
            if (dup) {
                LOG_INFO_("advance deliver suppressed duplicate error"
                          " session=%lld", (long long)session_id);
                return;
            }
        }
    }
    sqlite3_stmt *ins;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO channel_outbox(channel_name, session_id, payload)"
            " VALUES(?1, ?2, json_object('chat_id', ?3, 'text', ?4));",
            -1, &ins, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(ins, 1, channel, -1, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 2, session_id);
    sqlite3_bind_text(ins, 3, chat_id && chat_id[0] ? chat_id : "0", -1,
                      SQLITE_STATIC);
    sqlite3_bind_text(ins, 4, text, -1, SQLITE_STATIC);
    sqlite3_step(ins);
    sqlite3_finalize(ins);
}

/* Resolve a blocking reply edge: write the ToolResult for the parent's
 * launch_agent call, mark it done, unpark the parent, delete the edge. The
 * CAS on tool_calls.status makes it race-safe against recovery having
 * answered the call already (synthetic error) — then the edge just dies here
 * and the child's real answer ships later via its standing edge. Returns the
 * parent session id to wake (>0) if the result landed, 0 if the edge was
 * dropped without one. Caller holds the transaction. */
static int64_t oneshot_resolve(sqlite3 *db, int64_t child_sid, int64_t parent_sid,
                               const char *call_id, const char *text,
                               int is_error) {
    int open = 0;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT status IN ('pending','running') FROM tool_calls"
            " WHERE session_id=?1 AND call_id=?2;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, parent_sid);
        sqlite3_bind_text(st, 2, call_id, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) open = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }

    int64_t woken = 0;
    if (open && parent_sid > 0) {
        ToolResult tr = { .tool_call_id = (char *)call_id,
                          .content = (char *)text };
        Message rmsg = { .role = ROLE_TOOL, .tool_result = &tr,
                         .tool_name = "launch_agent", .is_error = is_error };
        int64_t rid = entry_append_with_iteration(db, parent_sid, &rmsg, 0);
        db_tool_call_complete_by_call(db, parent_sid, call_id, rid);
        /* Unpark parent — but only from a state that permits it. A parent
         * sitting in awaiting_approval or compacting must not be stomped
         * back to tool_running (that double-prompts / re-emits per sibling
         * completion); the eventual wake re-advances it from its own state. */
        char pstate[32] = {0};
        sqlite3_stmt *ps;
        if (sqlite3_prepare_v2(db, "SELECT state FROM sessions WHERE id=?",
                               -1, &ps, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(ps, 1, parent_sid);
            if (sqlite3_step(ps) == SQLITE_ROW) {
                const char *s = (const char *)sqlite3_column_text(ps, 0);
                if (s) snprintf(pstate, sizeof(pstate), "%s", s);
            }
            sqlite3_finalize(ps);
        }
        if (strcmp(pstate, "awaiting_approval") != 0 &&
            strcmp(pstate, "compacting") != 0)
            session_set_state(db, parent_sid, "tool_running");
        woken = parent_sid;
    }

    sqlite3_stmt *del;
    if (sqlite3_prepare_v2(db,
            "DELETE FROM delivery_edges WHERE session_id=?1"
            " AND target_kind='tool_call' AND target_ref=?2;",
            -1, &del, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(del, 1, child_sid);
        sqlite3_bind_text(del, 2, call_id, -1, SQLITE_STATIC);
        sqlite3_step(del);
        sqlite3_finalize(del);
    }
    return woken;
}

/* The per-session row every evaluation starts from. */
typedef struct {
    int64_t leaf_id;
    int64_t parent_sid;
    char channel[64];
    char chat_id[64];
} EdgeSessionRow;

static int edge_session_row(sqlite3 *db, int64_t sid, EdgeSessionRow *out) {
    memset(out, 0, sizeof(*out));
    out->parent_sid = -1;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT leaf_id, parent_session_id, channel_name, chat_id"
            " FROM sessions WHERE id=?;", -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, sid);
    int ok = -1;
    if (sqlite3_step(st) == SQLITE_ROW) {
        ok = 0;
        out->leaf_id = sqlite3_column_int64(st, 0);
        out->parent_sid = sqlite3_column_int64(st, 1);
        const char *s = (const char *)sqlite3_column_text(st, 2);
        if (s) snprintf(out->channel, sizeof(out->channel), "%s", s);
        s = (const char *)sqlite3_column_text(st, 3);
        if (s) snprintf(out->chat_id, sizeof(out->chat_id), "%s", s);
    }
    sqlite3_finalize(st);
    return ok;
}

/* Lazily materialize the channel edge for a chat-bound session, frozen from
 * the route template at this first boundary ('auto' = quiescent; no route =
 * quiescent). tool_filter precedent: later route edits don't retro-apply.
 * INSERT OR IGNORE, so a present row always wins. */
static void channel_edge_ensure(sqlite3 *db, int64_t sid, const EdgeSessionRow *sr) {
    if (!sr->channel[0]) return;
    char *tmpl = NULL;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT delivery_mode FROM channel_routes"
            " WHERE channel_name=?1 AND chat_id=?2;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, sr->channel, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, sr->chat_id[0] ? sr->chat_id : "0", -1,
                          SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *m = (const char *)sqlite3_column_text(st, 0);
            if (m) tmpl = strdup(m);
        }
        sqlite3_finalize(st);
    }
    const char *policy = "quiescent";
    if (tmpl && delivery_policy_valid(tmpl)) policy = tmpl;
    delivery_edge_create(db, sid, "channel", sr->channel, policy, 0);
    free(tmpl);
}

/* Did this session's final assistant entry stop at the model's output cap?
 * (stop_reason 2 = STOP_REASON_LENGTH, db.c stop_reason_to_int.) Mechanical:
 * one column, no prose reading. */
int session_final_truncated(sqlite3 *db, int64_t session_id) {
    return (int)db_scalar_i64(db,
        "SELECT stop_reason=2 FROM entries WHERE session_id=?1 AND role=2"
        " ORDER BY id DESC LIMIT 1;", session_id, 0);
}

/* Push the content-bearing assistant entries in (cursor, ∞) one by one on an
 * iteration edge. At a boundary the last push carries the boundary label;
 * mid-turn every push is an update. Returns the count pushed and leaves
 * *last_id at the newest pushed entry. */
static int iteration_push_since(sqlite3 *db, int64_t sid, const DeliveryEdge *e,
                                const EdgeSessionRow *sr, const char *final_label,
                                int64_t *last_id) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT id, content FROM entries"
            " WHERE session_id=?1 AND id>?2 AND role=2"
            "   AND content IS NOT NULL AND content != ''"
            "   AND type NOT IN ('tool_call','reasoning')"
            " ORDER BY id;", -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int64(st, 1, sid);
    sqlite3_bind_int64(st, 2, e->cursor);
    /* Collect first — the pushes below write while this statement reads. */
    int cap = 8, n = 0;
    struct { int64_t id; char *text; } *rows = malloc((size_t)cap * sizeof(*rows));
    if (!rows) { sqlite3_finalize(st); return 0; }
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= 2;
            void *tmp = realloc(rows, (size_t)cap * sizeof(*rows));
            if (!tmp) break;
            rows = tmp;
        }
        rows[n].id = sqlite3_column_int64(st, 0);
        const char *c = (const char *)sqlite3_column_text(st, 1);
        rows[n].text = strdup(c ? c : "");
        if (rows[n].text) n++;
    }
    sqlite3_finalize(st);

    for (int i = 0; i < n; i++) {
        const char *label = (final_label && i == n - 1) ? final_label : "update";
        if (strcmp(e->kind, "parent") == 0)
            parent_push(db, sr->parent_sid, sid, label, rows[i].text);
        else if (strcmp(e->kind, "channel") == 0)
            channel_push(db, e->ref, sid, sr->chat_id, rows[i].text);
        if (last_id) *last_id = rows[i].id;
        free(rows[i].text);
    }
    free(rows);
    return n;
}

int advance_deliver_boundary(sqlite3 *db, int64_t session_id, int is_error,
                             int include_channel) {
    /* Most sessions (every CLI root, every unbound daemon session) have no
     * edges and no chat — don't open a write transaction per turn for them. */
    if (!db_scalar_i64(db,
            "SELECT EXISTS(SELECT 1 FROM delivery_edges WHERE session_id=?1)"
            " OR EXISTS(SELECT 1 FROM sessions WHERE id=?1"
            "           AND channel_name IS NOT NULL);", session_id, 0))
        return 0;

    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        LOG_WARN_("advance deliver BEGIN failed session=%lld"
                  " — cursors stay behind, sweep will retry",
                  (long long)session_id);
        return -1;
    }

    /* Session row read inside the transaction: BEGIN IMMEDIATE serializes
     * against other writers, so the leaf this boundary delivers can't move
     * under us (a co-pointed CLI opening the next turn). */
    EdgeSessionRow sr;
    if (edge_session_row(db, session_id, &sr) != 0 || sr.leaf_id <= 0) {
        sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
        return 0;
    }

    channel_edge_ensure(db, session_id, &sr);

    DeliveryEdge edges[EDGE_MAX];
    int n = edges_load(db, session_id, edges);
    if (n <= 0) {
        sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
        return n < 0 ? -1 : 0;
    }

    /* One quiescence answer serves every edge at this boundary: the hold
     * decision for 'quiescent' and the truthful label for every push. */
    int q = session_subtree_quiescent(db, session_id);
    const char *label = is_error ? "failed" : (q ? "completed" : "update");
    /* F2: the outcome tag is derived from the child's final state, never from
     * its prose — a model that says "done" after hitting the output cap must
     * still reach the parent as truncated. */
    if (!is_error && session_final_truncated(db, session_id))
        label = "truncated";

    int shipped = 0;
    int64_t wake_parent = 0;
    for (int i = 0; i < n; i++) {
        DeliveryEdge *e = &edges[i];
        if (strcmp(e->policy, "explicit") == 0) continue;

        if (e->one_shot && strcmp(e->kind, "tool_call") == 0) {
            /* Errors bypass the quiescence hold: a parked parent must get an
             * error result rather than hang on a subtree that never settles. */
            if (!is_error && strcmp(e->policy, "quiescent") == 0 && !q)
                continue;
            char *text = (strcmp(e->policy, "digest") == 0)
                           ? digest_since(db, session_id, e->cursor)
                           : get_response_text(db, session_id);
            if (!text)
                text = strdup(is_error ? "error: sub-agent terminated abnormally"
                                       : "(no response)");
            int64_t w = oneshot_resolve(db, session_id, sr.parent_sid,
                                        e->ref, text, is_error);
            free(text);
            if (w > 0) {
                wake_parent = w;
                shipped++;
                /* The one-shot was the delivery of this boundary: advance the
                 * standing edge in the same transaction so the same content
                 * never ships twice — post-unblock turns still ship normally. */
                for (int j = 0; j < n; j++) {
                    if (!edges[j].one_shot &&
                        strcmp(edges[j].kind, "parent") == 0 &&
                        edges[j].cursor < sr.leaf_id) {
                        edge_cursor_set(db, edges[j].id, sr.leaf_id);
                        edges[j].cursor = sr.leaf_id;
                    }
                }
                LOG_INFO_("advance deliver one-shot session=%lld parent=%lld"
                          " policy=%s is_error=%d", (long long)session_id,
                          (long long)sr.parent_sid, e->policy, is_error);
            }
            continue;
        }
        if (e->one_shot) continue;               /* 'session' reply edges: M2 */
        if (e->cursor >= sr.leaf_id) continue;   /* nothing owed */

        int is_parent = strcmp(e->kind, "parent") == 0;
        int is_channel = strcmp(e->kind, "channel") == 0;
        if (!is_parent && !is_channel) continue; /* 'session' standing: M2 */

        if (is_parent && sr.parent_sid <= 0) continue;

        if (is_channel && !include_channel) {
            /* Suppressed boundary (LLM-error turn): nothing will ever ship
             * for it, so record it as evaluated or the sweep re-picks the
             * edge every tick forever. */
            edge_cursor_set(db, e->id, sr.leaf_id);
            continue;
        }
        if (is_channel) {
            /* Ingestion-only channel (no onOutbox handler): nothing would
             * ever drain the row. */
            int ingest_only = 0;
            sqlite3_stmt *os;
            if (sqlite3_prepare_v2(db,
                    "SELECT value='0' FROM channel_state"
                    " WHERE channel_name=?1 AND key='has_outbox';",
                    -1, &os, NULL) == SQLITE_OK) {
                sqlite3_bind_text(os, 1, e->ref, -1, SQLITE_STATIC);
                if (sqlite3_step(os) == SQLITE_ROW)
                    ingest_only = sqlite3_column_int(os, 0);
                sqlite3_finalize(os);
            }
            if (ingest_only) {
                edge_cursor_set(db, e->id, sr.leaf_id);
                continue;
            }
        }

        if (!is_error && strcmp(e->policy, "quiescent") == 0 && !q)
            continue;                            /* hold — delivery still owed */

        int pushed = 0;
        if (strcmp(e->policy, "iteration") == 0) {
            pushed = iteration_push_since(db, session_id, e, &sr,
                                          is_parent ? label : NULL, NULL);
        } else if (strcmp(e->policy, "digest") == 0) {
            char *text = digest_since(db, session_id, e->cursor);
            if (text) {
                if (is_parent) parent_push(db, sr.parent_sid, session_id, label, text);
                else channel_push(db, e->ref, session_id, sr.chat_id, text);
                free(text);
                pushed = 1;
            }
        } else {                                 /* turn | quiescent */
            char *text = get_response_text(db, session_id);
            if (!text && is_parent)
                text = strdup(is_error ? "error: sub-agent terminated abnormally"
                                       : "(no response)");
            if (text) {
                if (is_parent) parent_push(db, sr.parent_sid, session_id, label, text);
                else channel_push(db, e->ref, session_id, sr.chat_id, text);
                free(text);
                pushed = 1;
            }
        }
        edge_cursor_set(db, e->id, sr.leaf_id);
        if (pushed) {
            shipped += pushed;
            if (is_parent) wake_parent = sr.parent_sid;
            LOG_INFO_("advance deliver session=%lld edge=%s policy=%s label=%s",
                      (long long)session_id, e->kind, e->policy,
                      is_parent ? label : "-");
        }
    }

    if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
        LOG_WARN_("advance deliver COMMIT failed session=%lld"
                  " — rolled back, sweep will retry", (long long)session_id);
        return -1;
    }

    if (wake_parent > 0)
        wake_session(wake_parent);
    return shipped;
}

void advance_deliver_iteration(sqlite3 *db, int64_t session_id) {
    /* Almost every session has no iteration edge — don't open a write
     * transaction per LLM completion for them. */
    if (!db_scalar_i64(db,
            "SELECT EXISTS(SELECT 1 FROM delivery_edges WHERE session_id=?"
            " AND policy='iteration' AND one_shot=0);", session_id, 0))
        return;

    EdgeSessionRow sr;
    if (edge_session_row(db, session_id, &sr) != 0) return;

    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        return;                       /* cursor guard: the next pass catches up */

    DeliveryEdge edges[EDGE_MAX];
    int n = edges_load(db, session_id, edges);
    int64_t wake_parent = 0;
    for (int i = 0; i < n; i++) {
        DeliveryEdge *e = &edges[i];
        if (e->one_shot || strcmp(e->policy, "iteration") != 0) continue;
        int is_parent = strcmp(e->kind, "parent") == 0;
        if (is_parent && sr.parent_sid <= 0) continue;
        if (!is_parent && strcmp(e->kind, "channel") != 0) continue;
        int64_t last = e->cursor;
        if (iteration_push_since(db, session_id, e, &sr, NULL, &last) > 0) {
            edge_cursor_set(db, e->id, last);
            if (is_parent) wake_parent = sr.parent_sid;
        }
    }

    if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    else if (wake_parent > 0)
        wake_session(wake_parent);
}

/* Bounded work per tick — the next tick takes whatever is left. */
#define SWEEP_MAX 64

int advance_sweep_undelivered(sqlite3 *db) {
    /* An edge whose cursor is behind an idle session's assistant leaf is owed
     * a delivery: a lost push (transaction failed), a quiescent hold waiting
     * for the subtree to settle, or an unfired one-shot. Role 2 means a turn
     * actually ended — a child that was spawned but never ran (role-0/1 leaf)
     * is left alone, and so is a streak-tripped session (role-0 note leaf).
     * Collect ids before delivering: an open SELECT pins a WAL read snapshot,
     * and the delivery below writes (see cron.c's DueJob collection). */
    const char *sql =
        "SELECT DISTINCT s.id FROM sessions s"
        " JOIN delivery_edges e ON e.session_id = s.id"
        " WHERE s.state='idle' AND e.policy <> 'explicit'"
        "   AND e.cursor < s.leaf_id"
        "   AND EXISTS (SELECT 1 FROM entries en"
        "               WHERE en.id=s.leaf_id AND en.session_id=s.id"
        "                 AND en.role=2)"
        " LIMIT ?;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(st, 1, SWEEP_MAX);
    int64_t ids[SWEEP_MAX];
    int n = 0;
    while (n < SWEEP_MAX && sqlite3_step(st) == SQLITE_ROW)
        ids[n++] = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);

    int delivered = 0;
    for (int i = 0; i < n; i++) {
        /* A quiescent hold re-evaluating here is routine (ships 0); an actual
         * sweep delivery means a push was lost or a subtree settled silently. */
        int is_err = session_stopped_with_error(db, ids[i]);
        int rc = advance_deliver_boundary(db, ids[i], is_err, !is_err);
        if (rc > 0) {
            delivered += rc;
            LOG_INFO_("advance sweep delivered session=%lld payloads=%d",
                      (long long)ids[i], rc);
        }
    }
    return delivered;
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
 * already announced itself, and a human turn later starts a fresh one.
 * Returns a parent session id the caller must wake after COMMIT (a parked
 * blocking waiter that was failure-notified), or 0. */
static int64_t streak_refuse(sqlite3 *db, int64_t session_id, const StreakGate *g) {
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
        return 0;

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
     * is proc.c's proc_cfg()), and the channel runner re-drains its outbox on
     * every ~1s loop iteration, which is soon enough for a once-per-trip
     * notice. */
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO channel_outbox(channel_name, session_id, payload)"
            " SELECT channel_name, id,"
            "        json_object('chat_id', COALESCE(chat_id,'0'), 'text', ?2)"
            "   FROM sessions WHERE id=?1 AND channel_name IS NOT NULL;",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, session_id);
        sqlite3_bind_text(st, 2, text, -1, SQLITE_STATIC);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }

    /* Pause fail-notify (specs/delivery.md): a paused session never settles,
     * so a parent parked on a blocking launch of this session would wait
     * forever — stale-session recovery never fires for a legitimately idle
     * child. Resolve its one-shot reply edge with an error now, once per
     * trip like the marker above. The standing edge is left alone: its
     * cursor stays behind, and the real answer ships at the boundary after
     * a human revives the session. */
    int64_t wake = 0;
    char oref[192] = {0};
    if (sqlite3_prepare_v2(db,
            "SELECT target_ref FROM delivery_edges WHERE session_id=?1"
            " AND one_shot=1 AND target_kind='tool_call' LIMIT 1;",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, session_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *r = (const char *)sqlite3_column_text(st, 0);
            if (r) snprintf(oref, sizeof(oref), "%s", r);
        }
        sqlite3_finalize(st);
    }
    if (oref[0]) {
        int64_t parent = db_scalar_i64(db,
            "SELECT parent_session_id FROM sessions WHERE id=?;", session_id, -1);
        wake = oneshot_resolve(db, session_id, parent, oref,
                               "error: sub-agent paused by autonomy guard"
                               " (autonomous-turn limit reached); it stays"
                               " paused until a human replies to it", 1);
    }
    return wake;
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

/* ── Drain-side concurrency gate ──────────────────────────────────
 * The execution half of the old AGENT_MAX_TOTAL, moved from the launch site
 * to the turn open and from refusal to deferral: by drain time nobody is
 * present to hear a refusal, so over the cap the turn simply doesn't open —
 * the inbox rows stay queued (streak-guard shape) and the sweep or a
 * resource-free edge wake retries. Three exemptions, each load-bearing:
 *   human batch  — a human anywhere in the pending set opens even over cap
 *                  (the priority bit; refusal is for autonomous chatter);
 *   role-3 leaf  — a half-spent turn resuming; gating it strands paid-for
 *                  work, and it holds no new resource until dispatch anyway;
 *   blocked parents — not exempted here but in the *counting rule*
 *                  (session_count_resource_holders): a parent waiting on
 *                  blocking sub-agents holds nothing, else nested delegation
 *                  at the cap deadlocks — parents hold every slot, children
 *                  can never start, parents never finish. */
typedef struct {
    int cap;      /* session_max_concurrent; <= 0 disables the gate */
    int defer;    /* this turn must not open now */
} ConcGate;

static ConcGate conc_gate_check(sqlite3 *db, int64_t session_id) {
    ConcGate g = { .cap = config_get_int(db, "session_max_concurrent") };
    if (g.cap <= 0) return g;
    int gated;
    if (db_scalar_i64(db,
            "SELECT EXISTS(SELECT 1 FROM inbox q"
            " WHERE q.session_id=?1 AND q.consumed=0);", session_id, 0)) {
        gated = (int)db_scalar_i64(db,
            "SELECT " CCLAW_SQL_INBOX_ALL_AUTONOMOUS("?1") ";", session_id, 0);
    } else {
        /* Empty inbox: the only other opener is the unanswered-leaf resume.
         * Role 3 is the half-spent turn — exempt. Role 1 is a turn that never
         * dispatched (refused-dispatch repark); gate it only when the entry is
         * stamped autonomous — no stamp means human, same rule as the streak. */
        gated = (int)db_scalar_i64(db,
            "SELECT EXISTS(SELECT 1 FROM entries e"
            " WHERE e.id=(SELECT leaf_id FROM sessions WHERE id=?1)"
            "   AND e.session_id=?1 AND e.role=1"
            "   AND COALESCE(json_extract(e.data,'$.source'),'')"
            "       IN " CCLAW_AUTONOMOUS_SOURCES ");", session_id, 0);
    }
    if (!gated) return g;
    if (session_count_resource_holders(db, session_id) >= g.cap) {
        /* Debug, not info: the sweep re-advances a deferred session every
         * tick until a slot frees, and each pass lands here again. */
        LOG_DEBUG_("advance defer reason=session_max_concurrent cap=%d", g.cap);
        g.defer = 1;
    }
    return g;
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
            int64_t wake = streak_refuse(db, session_id, &gate);
            if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
                sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            else if (wake > 0)
                wake_session(wake);   /* fail-notified blocking waiter */
            AdvanceOutput out = make_output(ADVANCE_NOOP, session_id, agent, iter);
            free(agent);
            return out;
        }
        /* Concurrency gate, also before the drain: a deferral must leave the
         * inbox rows queued and durable. COMMIT, not ROLLBACK — the D11
         * reconcile above is real work that stands regardless. */
        ConcGate cgate = conc_gate_check(db, session_id);
        if (cgate.defer) {
            if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
                sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            AdvanceOutput out = make_output(ADVANCE_NOOP, session_id, agent, iter);
            out.deferred = 1;
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

        /* Job gone but the leaf is not an assistant entry: the request was
         * never submitted — a pool-full revert (or rate-limit bounce) lost its
         * idle write to a BUSY give-up. There is no completion to process;
         * without this guard the logic below reads the *previous* turn's
         * assistant entry as this turn's answer (stale delivery, premature
         * parent notify). Re-park idle so the sweep's unanswered-leaf arm
         * resumes the turn; if the re-park write fails too, the
         * stuck-completion sweep lands here again next tick. */
        {
            int64_t leaf_role = db_scalar_i64(db,
                "SELECT role FROM entries"
                " WHERE id=(SELECT leaf_id FROM sessions WHERE id=?1)",
                session_id, 2);
            if (leaf_role == 1 || leaf_role == 3) {
                LOG_WARN_("advance state=llm_running job=gone leaf_role=%d"
                          " — lost submit, reparking idle", (int)leaf_role);
                session_set_state(db, session_id, "idle");
                AdvanceOutput out = make_output(ADVANCE_NOOP, session_id, agent, iter);
                free(agent);
                return out;
            }
        }

        /* LLM finished — check what to do next */
        int tc_count = 0;
        PendingToolCall *calls = db_tool_call_get_pending(db, session_id, &tc_count);
        if (tc_count > 0) {
            /* The turn continues: 'iteration' edges ship the tool_use-stop
             * commentary as it lands (cursor-guarded — a give-up below just
             * re-runs this branch without duplicates). Every other policy
             * waits for a boundary. */
            advance_deliver_iteration(db, session_id);
            /* Has pending tool calls */
            if (session_set_state(db, session_id, "tool_running") != 0) {
                /* BUSY give-up mid-flip: don't dispatch against a stale state —
                 * the stuck-completion sweep re-runs this branch. */
                db_tool_call_free_pending(calls, tc_count);
                free(agent);
                return make_output(ADVANCE_ERROR, session_id, NULL, 0);
            }
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
            if (session_set_state(db, session_id, "idle") != 0) {
                /* Give-up on the flip: return without notifying — a premature
                 * notify hands the parent a result for a turn still marked
                 * running. The stuck-completion sweep retries the whole stop. */
                free(agent);
                return make_output(ADVANCE_ERROR, session_id, NULL, 0);
            }
            LOG_INFO_("advance state=llm_running next=idle reason=error");
            /* Abnormal stop: parent edges still deliver (errors bypass the
             * quiescence hold) so a blocking launch_agent gets an error
             * result instead of hanging. Channel edges stay quiet on an LLM
             * error, as ever — their cursor just records the boundary. */
            advance_deliver_boundary(db, session_id, 1, 0);
            AdvanceOutput out = make_output(ADVANCE_ERROR, session_id, agent, iter);
            free(agent);
            return out;
        }

        /* Normal stop — turn complete */
        if (session_set_state(db, session_id, "idle") != 0) {
            free(agent);
            return make_output(ADVANCE_ERROR, session_id, NULL, 0);
        }
        LOG_INFO_("advance state=llm_running next=idle reason=done");

        /* Turn boundary: every delivery edge evaluates (parent, one-shot,
         * channel). The caller's ADVANCE_DONE handling wakes the channel
         * FIFO for any outbox rows written here. */
        advance_deliver_boundary(db, session_id, 0, 1);

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
            char *rich_msg = rich_max_iter_message(db, session_id, max_iterations);
            Message msg = { .role = ROLE_ASSISTANT,
                            .content = rich_msg,
                            .stop_reason = STOP_REASON_ERROR };
            entry_append_with_iteration(db, session_id, &msg, 0);
            free(rich_msg);
            session_set_state(db, session_id, "idle");
            /* Max-iter is a terminal error boundary — every edge delivers,
             * chat included (it always saw the rich max-iter message). */
            advance_deliver_boundary(db, session_id, 1, 1);
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
            int64_t wake = streak_refuse(db, session_id, &gate);
            if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
                sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            else if (wake > 0)
                wake_session(wake);   /* fail-notified blocking waiter */
            AdvanceOutput out = make_output(ADVANCE_NOOP, session_id, agent, 0);
            free(agent);
            return out;
        }
        /* The idle branch's concurrency twin. The idle flip above stands
         * (compaction is done either way); only the drain is deferred. */
        ConcGate cgate = conc_gate_check(db, session_id);
        if (cgate.defer) {
            if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
                sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
            AdvanceOutput out = make_output(ADVANCE_NOOP, session_id, agent, 0);
            out.deferred = 1;
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
