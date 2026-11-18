#define _POSIX_C_SOURCE 200809L
#include "context.h"
#include "run_tool.h"
#include "config.h"
#include "db.h"
#include "http.h"
#include "llm.h"
#include "llm_payload.h"
#include "agent_config.h"
#include "log.h"
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Shared with the sandboxed child, which applies the same cut before its
 * result crosses the wire — see RUNTOOL_RESULT_MAX. */
#define TRUNCATE_MAX_BYTES  RUNTOOL_RESULT_MAX
#define TRUNCATE_MAX_LINES  RUNTOOL_RESULT_MAX_LINES

/* helpers for context_plan — integer column mapping */
static Role plan_int_to_role(int i) {
    switch (i) {
        case 0: return ROLE_SYSTEM;
        case 1: return ROLE_USER;
        case 2: return ROLE_ASSISTANT;
        case 3: return ROLE_TOOL;
        case 4: return ROLE_COMPACTION;
    }
    return ROLE_USER;
}

static StopReason plan_int_to_stop_reason(int i) {
    switch (i) {
        case 1: return STOP_REASON_STOP;
        case 2: return STOP_REASON_LENGTH;
        case 3: return STOP_REASON_TOOL_USE;
        case 4: return STOP_REASON_ERROR;
        case 5: return STOP_REASON_ABORTED;
    }
    return STOP_REASON_NONE;
}

/* Snap an index back to the first entry of the turn it falls inside, so a cut
 * never splits a turn. Backwards, not forwards: a turn legitimately opens with
 * several consecutive user entries (the inbox drain takes a channel message, a
 * job result and a sub-agent result in one transaction), and snapping forward
 * past the newest turn would drop the very message being answered. Keeping a
 * whole turn can overshoot the budget, but only by that one turn. */
static int snap_to_turn_start(const PlanEntry *entries, int i) {
    while (i > 0 && entries[i].turn_id == entries[i - 1].turn_id)
        i--;
    return i;
}

/* Find the cut point in a PlanEntry array (oldest entries dropped to fit budget). */
static int plan_find_cut(const PlanEntry *entries, int count, int budget) {
    if (count <= 0) return 0;
    int total = 0;
    int cut = 0;
    int i = count - 1;
    while (i >= 0) {
        int group_start = i;
        int group_tokens = 0;

        if (entries[i].role == ROLE_TOOL) {
            /* Walk backwards past tool results to find owning assistant */
            while (group_start > 0 && entries[group_start - 1].role == ROLE_TOOL)
                group_start--;
            if (group_start > 0 && entries[group_start - 1].role == ROLE_ASSISTANT
                && entries[group_start - 1].tool_call_count > 0)
                group_start--;
            for (int j = group_start; j <= i; j++)
                group_tokens += entries[j].tokens;
        } else {
            group_tokens = entries[i].tokens;
        }

        if (total + group_tokens > budget) {
            cut = i + 1;
            break;
        }
        total += group_tokens;
        i = group_start - 1;
    }

    if (cut >= count) cut = count - 1;   /* nothing fit — keep the newest turn */
    return snap_to_turn_start(entries, cut);
}

/* ── Measuring the branch ─────────────────────────────────────────
 *
 * An entry's size is entries.tokens — the provider's count, written once
 * when the report that measures it arrives (db_response.c, usage_attribute)
 * — and its estimate (token_estimate, TOKEN_ESTIMATE) until then. Sums over
 * any run of entries — a turn, the kept tail, the whole branch — are sums of
 * those.
 *
 * What is not an entry is the head: tools, system prompt, the user context
 * block and the JSON around everything. It is measured from one report
 * alone, never a difference of two: the newest report's usage_in, less the
 * entries that request was sent with. That measure is the head whenever it
 * is usable, larger or smaller than the estimate. It is usable when the
 * report was written after every edit on the branch — every summary, and
 * every entry carrying an edit's record, which turn_edit.c always puts on an
 * entry at least as new as the newest report it changed the ground under
 * (each edit changes what "the entries before it" are) — and its request
 * was sent whole: a request that had been cut (entries.request_cut, stamped
 * by llm_req) was not sent the entries subtracted here. Otherwise, and when
 * there is no report, the estimate passed in stands. */
static int segment_start(const PlanEntry *e, int a) {
    /* The response begins at its first part (a reasoning entry precedes the
     * assistant_message): that is where the request that produced it ended. */
    while (a > 0 && e[a - 1].role == ROLE_ASSISTANT && e[a].iteration_id != 0
           && e[a - 1].iteration_id == e[a].iteration_id)
        a--;
    return a;
}

static int plan_head(const PlanEntry *e, int n, int head, int *measured_out) {
    int r = -1;
    *measured_out = 0;
    for (int i = n - 1; i >= 0 && r < 0; i--)
        if (e[i].usage_in > 0) r = i;
    if (r < 0 || !e[r].fresh || e[r].cut) return head;
    int sent = 0, start = segment_start(e, r);
    for (int j = 0; j < start; j++)
        if (e[j].id < e[r].id) sent += e[j].tokens;
    int measured = e[r].usage_in - sent;
    /* Nothing but a broken report (counts attributed past what was sent)
     * reads at or below zero: that is no measure. */
    if (measured <= 0) return head;
    *measured_out = 1;
    return measured;
}

/* What the per-row query leaves to a pass over the branch: fresh, written
 * after the newest edit marker (ids are creation order, branch order is
 * not). */
static void branch_marks(PlanEntry *e, int n) {
    int64_t newest = -1;
    for (int i = 0; i < n; i++)
        if (e[i].reset && e[i].id > newest) newest = e[i].id;
    for (int i = 0; i < n; i++) e[i].fresh = e[i].id > newest;
}

/* The branch as the next request would send it — error/aborted responses
 * and their results left out — measured. One query, no content loaded. */
static int branch_load(sqlite3 *db, int64_t session_id, int head, ContextPlan *out) {
    memset(out, 0, sizeof(*out));
    /* Level counter for path ordering (compaction entries may have higher
     * ids). A response is a report when it carries usage_in. */
    const char *sql =
        "WITH RECURSIVE branch(id, parent_id, lvl) AS ("
        "  SELECT id, parent_id, 0"
        "    FROM entries WHERE id=(SELECT leaf_id FROM sessions WHERE id=?1) AND session_id=?1"
        "  UNION ALL"
        "  SELECT e.id, e.parent_id, b.lvl+1"
        "    FROM entries e JOIN branch b ON e.id=b.parent_id"
        ") SELECT e.id, e.role, e.stop_reason, COALESCE(e.tokens, e.token_estimate),"
        "         e.tool_call_count, e.turn_id, e.iteration_id,"
        "         CASE WHEN e.role=2 AND e.stop_reason NOT IN (4,5) THEN e.usage_in END,"
        "         e.role=4 OR CASE WHEN json_valid(e.data)"
        "           THEN json_type(e.data,'$.turn_edits') END IS 'array',"
        "         COALESCE(e.request_cut,0)"
        "    FROM branch b JOIN entries e ON e.id=b.id ORDER BY b.lvl DESC;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, session_id);

    int cap = 32;
    PlanEntry *raw = malloc((size_t)cap * sizeof(PlanEntry));
    if (!raw) { sqlite3_finalize(stmt); return -1; }
    int raw_count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (raw_count >= cap) {
            cap *= 2;
            PlanEntry *tmp = realloc(raw, (size_t)cap * sizeof(PlanEntry));
            if (!tmp) {
                free(raw);
                sqlite3_finalize(stmt);
                return -1;
            }
            raw = tmp;
        }
        PlanEntry *pe = &raw[raw_count];
        pe->id = sqlite3_column_int64(stmt, 0);
        pe->role = plan_int_to_role(sqlite3_column_int(stmt, 1));
        pe->stop_reason = plan_int_to_stop_reason(sqlite3_column_int(stmt, 2));
        /* A row with neither a count nor an estimate still costs its framing;
         * a measured 0 (reasoning no longer replayed) is a real 0. */
        pe->tokens = sqlite3_column_type(stmt, 3) == SQLITE_NULL ? 4
                                                                 : sqlite3_column_int(stmt, 3);
        pe->tool_call_count = sqlite3_column_int(stmt, 4);
        pe->turn_id = sqlite3_column_int64(stmt, 5);
        pe->iteration_id = sqlite3_column_int64(stmt, 6);
        pe->usage_in = sqlite3_column_int(stmt, 7);
        pe->reset = (unsigned char)sqlite3_column_int(stmt, 8);
        pe->cut = (unsigned char)(sqlite3_column_int(stmt, 9) != 0);
        raw_count++;
    }
    sqlite3_finalize(stmt);
    branch_marks(raw, raw_count);

    if (raw_count == 0) { free(raw); return -1; }

    /* filter out error/aborted assistant entries + their following tool_results */
    PlanEntry *filtered = malloc((size_t)raw_count * sizeof(PlanEntry));
    if (!filtered) { free(raw); return -1; }
    int fcount = 0;
    for (int i = 0; i < raw_count; i++) {
        if (raw[i].role == ROLE_ASSISTANT &&
            (raw[i].stop_reason == STOP_REASON_ERROR ||
             raw[i].stop_reason == STOP_REASON_ABORTED)) {
            /* Skip this + following tool_results */
            int j = i + 1;
            while (j < raw_count && raw[j].role == ROLE_TOOL) j++;
            i = j - 1;
            continue;
        }
        filtered[fcount++] = raw[i];
    }
    free(raw);

    out->entries = filtered;
    out->count = fcount;
    out->head = plan_head(filtered, fcount, head, &out->head_measured);
    return 0;
}

int context_measure(sqlite3 *db, int64_t session_id, int head, ContextPlan *out) {
    if (!db || !out) return -1;
    return branch_load(db, session_id, head, out);
}

int context_plan(sqlite3 *db, int64_t session_id, const Config *cfg, int overhead, ContextPlan *out) {
    if (!db || !cfg || !out) return -1;
    if (branch_load(db, session_id, overhead, out) != 0) return -1;

    /* The request is cut at the same line the trim keeps the session under
     * (turn_edit.c): the governing entry's trim_threshold. Between
     * iterations the trim has already brought the session below it, so in
     * practice this acts only on a turn's first request, when the messages
     * that opened it pushed the session over; the trim at the end of that
     * iteration then makes the stored branch agree. */
    int budget = cfg->max_history_tokens;
    if (budget <= 0) {
        char *agent = session_get_agent_name(db, session_id);
        ContextLimits lim;
        context_limits(db, agent, cfg->context_window, &lim);
        free(agent);
        budget = lim.threshold;
    }
    if (budget <= 0) budget = 8000;

    /* History gets what the head leaves: budget and head are both in the
     * provider's tokens where it reported, the entries' sizes too. */
    int effective = budget - out->head;
    if (effective < 256) effective = 256;

    out->cut = plan_find_cut(out->entries, out->count, effective);
    out->budget = budget;
    return 0;
}

void context_plan_free(ContextPlan *plan) {
    if (!plan) return;
    free(plan->entries);
    plan->entries = NULL;
    plan->count = 0;
}

/* ── Write-time truncation with spill to temp file ────────────────── */

void session_tmp_dir(sqlite3 *db, int64_t session_id, char *buf, size_t bufsz) {
    /* Spill into the agent workspace (natural lifetime, no symlink-prone
     * world-writable /tmp), under the same .tool_results/ home web_fetch
     * uses for its full copies — one place to look for full tool output.
     *
     * The path is advertised to the model, so it must be one a sandboxed
     * tool child can actually read: the workspace is bind-mounted rw at the
     * same absolute path inside the sandbox; the host's /tmp is not (the
     * child's /tmp is the agent's private scratch). An operator-pinned
     * CCLAW_WORKSPACE stands for every agent (same rule as agent_setup's
     * workspace_pinned); otherwise derive the advancing agent's own
     * workspace from the session row, mirroring workspace_refresh. */
    const char *ws = getenv("CCLAW_WORKSPACE");
    if (ws && ws[0]) {
        snprintf(buf, bufsz, "%s/.tool_results/%lld", ws, (long long)session_id);
        return;
    }
    if (db) {
        sqlite3_stmt *s;
        char agent[128] = "";
        if (sqlite3_prepare_v2(db, "SELECT agent_name FROM sessions WHERE id=?1",
                               -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(s, 1, session_id);
            if (sqlite3_step(s) == SQLITE_ROW) {
                const char *v = (const char *)sqlite3_column_text(s, 0);
                if (v) snprintf(agent, sizeof(agent), "%s", v);
            }
            sqlite3_finalize(s);
        }
        if (agent[0] && !strchr(agent, '/') && strcmp(agent, "..") != 0) {
            char agents_dir[PATH_MAX];
            agent_dir_resolve(NULL, sqlite3_db_filename(db, "main"),
                              agents_dir, sizeof(agents_dir));
            int n = snprintf(buf, bufsz, "%s/%s/workspace/.tool_results/%lld",
                             agents_dir, agent, (long long)session_id);
            if (n > 0 && (size_t)n < bufsz) return;
        }
    }
    snprintf(buf, bufsz, "/tmp/cclaw-%lld", (long long)session_id);
}

/* Resolve the spill file for one tool call and make its directory. Shared by
 * the parent (truncate_and_spill) and by the sandboxed child, which is handed
 * the finished path over the wire — the call_id sanitising below is why: it is
 * model-emitted, and a '/' or ".." would otherwise steer a write anywhere the
 * agent UID reaches. Doing it here keeps that check in the trusted parent.
 * Returns 0 on success. */
int spill_path_build(sqlite3 *db, int64_t session_id, const char *tool_call_id,
                     char *buf, size_t bufsz) {
    if (!buf || bufsz == 0) return -1;
    char dir[PATH_MAX];
    session_tmp_dir(db, session_id, dir, sizeof(dir));

    /* Create the immediate parent, then the session dir; ignore EEXIST. */
    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", dir);
    char *last_slash = strrchr(parent, '/');
    if (last_slash && last_slash != parent) {
        *last_slash = '\0';
        mkdir(parent, 0700);
    }
    mkdir(dir, 0700);

    const char *safe_id = tool_call_id;
    if (safe_id) {
        if (strstr(safe_id, "..") || !safe_id[0]) {
            safe_id = NULL;
        } else {
            for (const char *p = safe_id; *p; p++) {
                if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                      (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) {
                    safe_id = NULL;
                    break;
                }
            }
        }
    }
    int n = snprintf(buf, bufsz, "%s/%s.out", dir, safe_id ? safe_id : "unknown");
    return (n > 0 && (size_t)n < bufsz) ? 0 : -1;
}

int job_log_path_build(sqlite3 *db, int64_t session_id, const char *tool_call_id,
                       char *buf, size_t bufsz) {
    /* Same home, same call-id sanitising as the spill file; a job's live log
     * is just a differently-suffixed sibling ("<call>.log" vs "<call>.out"). */
    if (spill_path_build(db, session_id, tool_call_id, buf, bufsz) != 0)
        return -1;
    size_t l = strlen(buf);
    if (l < 4 || l + 1 >= bufsz) return -1;
    memcpy(buf + l - 4, ".log", 5);
    return 0;
}

char *job_log_tail(sqlite3 *db, int64_t session_id, const char *call_id,
                   size_t cap) {
    char logp[PATH_MAX + 64];
    if (job_log_path_build(db, session_id, call_id, logp, sizeof(logp)) != 0)
        return NULL;
    int fd = open(logp, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return NULL;
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz <= 0) { close(fd); return NULL; }
    off_t start = (size_t)sz > cap ? sz - (off_t)cap : 0;
    lseek(fd, start, SEEK_SET);
    size_t want = (size_t)(sz - start);
    char *buf = malloc(want + 1);
    if (!buf) { close(fd); return NULL; }
    size_t got = 0;
    while (got < want) {
        ssize_t r = read(fd, buf + got, want - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    buf[got] = '\0';
    return buf;
}

char *truncate_and_spill(sqlite3 *db, const char *src, int64_t session_id,
                         const char *tool_call_id) {
    if (!src) return NULL;
    size_t len = strlen(src);

    /* Check if truncation needed */
    size_t cut = len;
    int lines = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '\n') {
            lines++;
            if (lines >= TRUNCATE_MAX_LINES) { cut = i + 1; break; }
        }
        if (i + 1 >= TRUNCATE_MAX_BYTES && cut == len) { cut = TRUNCATE_MAX_BYTES; break; }
    }
    if (cut >= len) return strdup(src);
    /* A byte-budget cut can land inside a multi-byte character; back up to
     * its first byte so the kept prefix stays valid UTF-8. */
    while (cut > 0 && ((unsigned char)src[cut] & 0xC0) == 0x80) cut--;

    char path[PATH_MAX + 64];
    if (spill_path_build(db, session_id, tool_call_id, path, sizeof(path)) != 0)
        return strdup(src);
    /* O_NOFOLLOW: never follow a symlink planted at the spill path — a
     * pre-existing symlink makes open() fail (ELOOP) and we skip the spill. */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        ssize_t off = 0;
        while ((size_t)off < len) {
            ssize_t w = write(fd, src + off, len - (size_t)off);
            if (w <= 0) break;
            off += w;
        }
        close(fd);
    }

    /* Count total lines */
    int total_lines = 0;
    for (size_t i = 0; i < len; i++)
        if (src[i] == '\n') total_lines++;

    /* Build suffix with file path reference (sized to hold the full path) */
    char suffix[PATH_MAX + 256];
    snprintf(suffix, sizeof(suffix),
             "\n[truncated — showing first %d lines of %d. Full output: %s]",
             lines < TRUNCATE_MAX_LINES ? lines : TRUNCATE_MAX_LINES,
             total_lines > 0 ? total_lines : 1, path);

    size_t result_len = cut + strlen(suffix);
    char *result = malloc(result_len + 1);
    if (!result) return strdup(src);
    memcpy(result, src, cut);
    memcpy(result + cut, suffix, strlen(suffix) + 1);
    return result;
}

/* Auto-recall — FTS5 search across sessions for relevant context. */
char *context_auto_recall(sqlite3 *db, int64_t session_id, const char *user_msg,
                          int max_tokens) {
    if (!db || !user_msg || !user_msg[0]) return NULL;

    /* Extract keywords: words ≥ 3 chars, skip duplicates, max 8 keywords.
     * (Short floor — dynamic stopword filtering below handles common words;
     * 3 lets useful short terms through: git, ssh, npm, sql.) */
    char query[512] = {0};
    int qlen = 0;
    char *dup = strdup(user_msg);
    if (!dup) return NULL;

    char *words[8];
    int nwords = 0;
    char *tok = strtok(dup, " \t\n\r.,;:!?\"'()[]{}");
    while (tok && nwords < 8) {
        if (strlen(tok) < 3) { tok = strtok(NULL, " \t\n\r.,;:!?\"'()[]{}"); continue; }
        /* skip duplicates */
        int found = 0;
        for (int i = 0; i < nwords; i++)
            if (strcasecmp(words[i], tok) == 0) { found = 1; break; }
        if (!found) words[nwords++] = tok;
        tok = strtok(NULL, " \t\n\r.,;:!?\"'()[]{}");
    }

    if (nwords == 0) { free(dup); return NULL; }

    /* Dynamic stopwords: drop keywords present in > 25% of indexed rows.
     * Single query folds total count + per-word DF filter via VALUES CTE. */
    {
        static const char *stop_sql =
            "WITH t(total) AS (SELECT COUNT(*) FROM entries),"
            "     cand(idx, w) AS (VALUES (0,?1),(1,?2),(2,?3),(3,?4),(4,?5),(5,?6),(6,?7),(7,?8))"
            " SELECT cand.idx FROM cand, t"
            " WHERE cand.w IS NOT NULL"
            "   AND (t.total < 8"
            "        OR (SELECT COUNT(*) FROM entries_fts"
            "            WHERE entries_fts MATCH '\"' || cand.w || '\"') * 4 <= t.total)"
            " ORDER BY cand.idx;";
        sqlite3_stmt *ss;
        if (sqlite3_prepare_v2(db, stop_sql, -1, &ss, NULL) == SQLITE_OK) {
            for (int i = 0; i < 8; i++) {
                if (i < nwords)
                    sqlite3_bind_text(ss, i + 1, words[i], -1, SQLITE_STATIC);
                else
                    sqlite3_bind_null(ss, i + 1);
            }
            int kept = 0;
            while (sqlite3_step(ss) == SQLITE_ROW)
                words[kept++] = words[sqlite3_column_int(ss, 0)];
            sqlite3_finalize(ss);
            nwords = kept;
        }
    }

    if (nwords == 0) { free(dup); return NULL; }

    for (int i = 0; i < nwords; i++) {
        /* snprintf returns the would-have-written length; once the buffer fills,
         * sizeof(query) - qlen underflows (size_t) into a huge bound and the
         * next write runs off the stack. Clamp the remaining space and stop as
         * soon as a write is truncated. */
        size_t rem = (size_t)qlen >= sizeof(query) ? 0 : sizeof(query) - (size_t)qlen;
        if (rem == 0) break;
        if (i > 0) {
            int n = snprintf(query + qlen, rem, " OR ");
            if (n < 0 || (size_t)n >= rem) { query[qlen] = '\0'; break; }
            qlen += n;
            rem -= (size_t)n;
        }
        int n = snprintf(query + qlen, rem, "\"%s\"", words[i]);
        if (n < 0 || (size_t)n >= rem) { query[qlen] = '\0'; break; }
        qlen += n;
    }
    free(dup);

    /* FTS5 search — skip entries from current session */
    const char *sql =
        "SELECT e.content, e.session_id, e.role, f.rank FROM entries_fts f"
        " JOIN entries e ON e.id = f.rowid"
        " WHERE entries_fts MATCH ? AND e.session_id != ? AND e.role IN (1,2)"
        " ORDER BY rank LIMIT 5;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(stmt, 1, query, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, session_id);

    /* Build output: search-result list, one line per hit, tagged with the
     * source session so the agent can expand via db_query on entries. */
    int max_chars = BYTES_OF_TOKENS(max_tokens);
    size_t cap = (size_t)max_chars + 512;
    char *out = malloc(cap);
    if (!out) { sqlite3_finalize(stmt); return NULL; }
    int olen = snprintf(out, cap,
        "---Possibly relevant context---\n"
        "Keyword-search results from other sessions; may be irrelevant —"
        " the user's latest message takes precedence. To read a full"
        " conversation, query the entries table by session_id.\n");

    int hits = 0;
    double best_rank = 0.0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *content = (const char *)sqlite3_column_text(stmt, 0);
        if (!content || !content[0]) continue;
        /* Relevance gate: BM25 rank is negative, best first. Stop once a
         * hit scores less than half the best — top-5 is a cap, not a quota. */
        double rank = sqlite3_column_double(stmt, 3);
        if (hits == 0) best_rank = rank;
        else if (rank > best_rank * 0.5) break;
        int64_t src_sid = sqlite3_column_int64(stmt, 1);
        const char *who = sqlite3_column_int(stmt, 2) == 1 ? "user" : "assistant";
        /* Truncate individual entries to ~100 tokens */
        int entry_max = 400;
        int clen = (int)strlen(content);
        if (clen > entry_max) clen = entry_max;
        if (olen + clen + 64 > max_chars) break;
        olen += snprintf(out + olen, cap - (size_t)olen,
                         "[session %lld, %s] %.*s\n",
                         (long long)src_sid, who, clen, content);
        hits++;
    }
    sqlite3_finalize(stmt);

    if (hits == 0) { free(out); return NULL; }

    snprintf(out + olen, cap - (size_t)olen, "---End of context---");
    return out;
}

/* The governing routing entry — the agent's first enabled chat model
 * (decision 19: a smaller fallback is never trimmed for) — supplies the
 * window, the two trim numbers and the cache lifetime. The 50% defaults below are the only copy
 * of them: an empty trim_threshold is half the window, an empty
 * prune_target half the threshold. The schema refuses nonsense at write
 * time, but nothing here relies on it: the trim works whatever these are. */
void context_limits(sqlite3 *db, const char *agent_name, int default_window,
                    ContextLimits *out) {
    out->window = default_window;
    out->threshold = out->target = -1;
    out->lifetime = 0;
    if (db && agent_name && agent_name[0]) {
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db,
                "SELECT m.context_window, am.trim_threshold, am.prune_target,"
                "       am.cache_lifetime"
                "  FROM agent_models am JOIN models m ON m.id = am.model_id"
                " WHERE am.agent_name = ?1 AND am.purpose = 'chat'"
                "   AND m.status != 'disabled'"
                " ORDER BY am.pos LIMIT 1;", -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, agent_name, -1, SQLITE_STATIC);
            if (sqlite3_step(s) == SQLITE_ROW) {
                if (sqlite3_column_int(s, 0) > 0) out->window = sqlite3_column_int(s, 0);
                if (sqlite3_column_type(s, 1) != SQLITE_NULL)
                    out->threshold = sqlite3_column_int(s, 1);
                if (sqlite3_column_type(s, 2) != SQLITE_NULL)
                    out->target = sqlite3_column_int(s, 2);
                if (sqlite3_column_int(s, 3) > 0) out->lifetime = sqlite3_column_int(s, 3);
            }
            sqlite3_finalize(s);
        }
    }
    if (out->threshold <= 0) out->threshold = out->window / 2;
    if (out->target <= 0) out->target = out->threshold / 2;
}

/* Head of a request, estimated: the tools it carries (llm_tools_sent — the
 * payload's own selection, so the two cannot drift), the system prompt and
 * the user context block the session holds, none of which is an entry. It
 * stands wherever no usable report measures the head (plan_head); the cut
 * (llm_req passes it as `overhead`) and the trim pass the same number.
 * system_prompt may be NULL — then it is built and freed here (the trim
 * check); llm_req passes the one it already holds. At a turn's first request
 * the block counted is the previous turn's, which the new one replaces. */
int context_head_estimate(sqlite3 *db, const char *agent_name, int64_t session_id,
                          const Config *cfg, const char *system_prompt) {
    int tools = 0;
    llm_tools_sent(db, session_id, agent_name, &tools, NULL);
    int64_t block = db_scalar_i64(db,
        "SELECT COALESCE(length(CAST(turn_context AS BLOB)),0) FROM sessions WHERE id=?1;",
        session_id, 0);
    char *built = NULL;
    if (!system_prompt) {
        built = agent_build_system_prompt(db, agent_name, session_id, "agents", cfg);
        system_prompt = built;
    }
    int64_t bytes = (int64_t)tools + block + (system_prompt ? (int64_t)strlen(system_prompt) : 0);
    free(built);
    return TOKENS_OF_BYTES(bytes);
}
