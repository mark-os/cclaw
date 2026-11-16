#define _POSIX_C_SOURCE 200809L
#include "context.h"
#include "run_tool.h"
#include "config.h"
#include "db.h"
#include "http.h"
#include "llm.h"
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

int context_compaction_keep_from(const ContextPlan *plan, int target_tokens) {
    if (!plan || plan->count <= 0) return 0;

    /* Walk backwards from the tail, accumulating until the target is spent. */
    int keep_from = 0;
    int cumulative = 0;
    for (int i = plan->count - 1; i >= 0; i--) {
        cumulative += plan->entries[i].tokens;
        if (cumulative > target_tokens) { keep_from = i + 1; break; }
    }
    /* Even the newest entry alone exceeds the target: keep just the leaf. */
    if (keep_from >= plan->count) keep_from = plan->count - 1;

    keep_from = snap_to_turn_start(plan->entries, keep_from);
    /* 0 = the whole branch fits, or the tail turn reaches back to the root —
     * either way there is no prefix to summarize without splitting a turn.
     * 1 leaves a single entry, which entry_compact cannot straddle. */
    return keep_from <= 1 ? 0 : keep_from;
}

/* ── Measuring the branch ─────────────────────────────────────────
 *
 * The provider reports usage on every response; that is the measure. Its
 * usage_in counts the whole request that produced the response: head, user
 * context block and every entry sent before it. So the entries between two
 * reported responses — a *segment*: one response with its tool calls and
 * results, plus whatever user entries opened the next turn — measure
 * usage_in(next) − usage_in(this), with no ratio and no guess. The estimate
 * (token_estimate, chars/3) is the fallback: a segment counts the larger of
 * the two, and the estimate alone where the difference does not measure it:
 *   - a model switch (another tokenizer) or a compaction between the two
 *     reports;
 *   - no next report yet (the newest response): usage_out counts what the
 *     model wrote (less the reasoning tokens the provider reports in it,
 *     which are not sent back), the rest is estimated.
 * A report whose turn was edited after it was sent, where the edit reached
 * into what that request carried (report_intact), no longer describes the
 * branch before it and is passed over like a response with no report: the
 * segment runs from the intact report before it to the intact one after,
 * so an edited turn is measured again by the first request sent after the
 * edit, and by the estimate (and usage_out) until then. Entries written
 * after the closing report (a hook's note) add their estimate on top: that
 * report never saw them. Entries before the first report are estimated. A
 * response that stopped on an error is never sent (filtered before this
 * runs) and is no report.
 *
 * The excess over the estimate lands on the response entry that opens the
 * segment, so sums over any run of entries — a turn, the kept tail, the
 * whole branch — read measured sizes. */
static int segment_start(const PlanEntry *e, int a) {
    /* The response begins at its first part (a reasoning entry precedes the
     * assistant_message): that is where the request that produced it ended. */
    while (a > 0 && e[a - 1].role == ROLE_ASSISTANT && e[a].iteration_id != 0
           && e[a - 1].iteration_id == e[a].iteration_id)
        a--;
    return a;
}

/* Does report a's usage_in still describe the branch before it? Yes unless
 * its turn was edited after it was sent and the edit reached into what that
 * request carried: whatever an edit removes, replaces or inserts, the entry
 * after it is re-pointed, and what it writes is newer than a. */
static int report_intact(const PlanEntry *e, int start, int a) {
    if (!e[a].turn_edited) return 1;
    for (int j = a; j >= 0 && e[j].turn_id == e[a].turn_id; j--)
        if (e[j].repointed || (j < start && e[j].id > e[a].id))
            return 0;
    return 1;
}

/* Provider's count above the estimate for the segment [s, end) opened by
 * report a and closed by report next (-1: none yet). */
static int segment_excess(const PlanEntry *e, int s, int end, int a, int next) {
    int measured, est = 0;
    if (next >= 0) {
        if (e[next].epoch != e[a].epoch || e[next].model != e[a].model) return 0;
        measured = e[next].usage_in - e[a].usage_in;
        for (int j = s; j < end; j++)
            if (e[j].id < e[next].id) est += e[j].token_estimate;
    } else {
        measured = e[a].usage_out;
        for (int j = s; j < end; j++)
            if (e[j].role == ROLE_ASSISTANT && e[j].iteration_id == e[a].iteration_id)
                est += e[j].token_estimate;
    }
    return measured > est ? measured - est : 0;
}

/* Fill e[].tokens, and return the head to reserve: `head` (the estimate of
 * tools + system prompt), or the provider's measure of what precedes the
 * entries when that is larger. That measure is the newest intact report r
 * after every compaction, less the measured entries it was sent with. It
 * takes in the user context block and the JSON framing, which no estimate
 * sees; if r's request had been cut it reads low, and the estimate stands. */
static int plan_measure(PlanEntry *e, int n, int head) {
    for (int i = 0; i < n; i++) e[i].tokens = e[i].token_estimate;
    int prev = -1, prev_start = 0, r = -1, r_start = 0;
    for (int i = 0; i <= n; i++) {
        int start = n;
        if (i < n) {
            if (e[i].usage_in <= 0) continue;
            start = segment_start(e, i);
            if (!report_intact(e, start, i)) continue;
        }
        if (prev >= 0)
            e[prev].tokens += segment_excess(e, prev_start, start, prev, i < n ? i : -1);
        if (i == n) break;
        if (e[i].fresh) { r = i; r_start = start; }
        prev = i;
        prev_start = start;
    }
    if (r < 0) return head;
    int sent = 0;
    for (int j = 0; j < r_start; j++)
        if (e[j].id < e[r].id) sent += e[j].tokens;
    int measured = e[r].usage_in - sent;
    return measured > head ? measured : head;
}

/* What the per-row query leaves to a pass over the branch: epoch (the
 * compactions on the branch created before each entry — ids are creation
 * order, branch order is not), fresh (after the newest of them), and
 * turn_edited spread over the whole turn (turns are contiguous). */
static void branch_marks(PlanEntry *e, int n) {
    int64_t comp[64];       /* each compaction drops the summary before it, so
                             * a branch carries one or two */
    int nc = 0;
    int64_t newest = -1;
    for (int i = 0; i < n; i++) {
        if (e[i].role != ROLE_COMPACTION) continue;
        if (e[i].id > newest) newest = e[i].id;
        if (nc < 64) comp[nc++] = e[i].id;
    }
    for (int i = 0; i < n; i++) {
        e[i].fresh = e[i].id > newest;
        e[i].epoch = 0;
        if (e[i].usage_in > 0)
            for (int k = 0; k < nc; k++) e[i].epoch += comp[k] < e[i].id;
    }
    for (int i = 0; i < n;) {
        int j = i, edited = 0;
        for (; j < n && e[j].turn_id == e[i].turn_id; j++) edited |= e[j].turn_edited;
        for (; i < j; i++) e[i].turn_edited = (unsigned char)edited;
    }
}

/* The branch as the next request would send it — error/aborted responses
 * and their results left out — measured. One query, no content loaded. */
static int branch_load(sqlite3 *db, int64_t session_id, int head, ContextPlan *out) {
    memset(out, 0, sizeof(*out));
    /* Level counter for path ordering (compaction entries may have higher
     * ids). A response is a report when it carries usage_in. */
    const char *sql =
        "WITH RECURSIVE branch(id, parent_id, role, stop_reason, token_estimate,"
        "                      tool_call_count, turn_id, iteration_id, usage_in,"
        "                      usage_out, model, repointed, edited, lvl) AS ("
        "  SELECT id, parent_id, role, stop_reason, token_estimate, tool_call_count,"
        "         turn_id, iteration_id, usage_in, usage_out, model,"
        "         original_parent_id IS NOT NULL,"
        "         CASE WHEN json_valid(data) THEN json_type(data,'$.turn_edits') END IS 'array', 0"
        "    FROM entries WHERE id=(SELECT leaf_id FROM sessions WHERE id=?1) AND session_id=?1"
        "  UNION ALL"
        "  SELECT e.id, e.parent_id, e.role, e.stop_reason, e.token_estimate,"
        "         e.tool_call_count, e.turn_id, e.iteration_id, e.usage_in, e.usage_out,"
        "         e.model, e.original_parent_id IS NOT NULL,"
        "         CASE WHEN json_valid(e.data) THEN json_type(e.data,'$.turn_edits') END"
        "           IS 'array', b.lvl+1"
        "    FROM entries e JOIN branch b ON e.id=b.parent_id"
        ") SELECT id, role, stop_reason, token_estimate, tool_call_count, turn_id,"
        "         iteration_id,"
        "         CASE WHEN role=2 AND stop_reason NOT IN (4,5) THEN usage_in END,"
        /* usage_out less the reasoning the provider says it holds, which is
         * not sent back; a provider that reports none counts in full. */
        "         CASE WHEN usage_out > 0 THEN usage_out - COALESCE(("
        "           SELECT r.reasoning_tokens FROM llm_responses r"
        "            WHERE r.session_id=?1 AND r.iteration_id=branch.iteration_id"
        "              AND r.status='ok' ORDER BY r.id DESC LIMIT 1), 0) END,"
        "         model, edited, repointed"
        "    FROM branch ORDER BY lvl DESC;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, session_id);

    int cap = 32, model_run = 0;
    char last_model[256] = "";
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
        pe->token_estimate = sqlite3_column_int(stmt, 3);
        if (pe->token_estimate <= 0) {
            pe->token_estimate = 4;
        }
        pe->tool_call_count = sqlite3_column_int(stmt, 4);
        pe->turn_id = sqlite3_column_int64(stmt, 5);
        pe->iteration_id = sqlite3_column_int64(stmt, 6);
        pe->usage_in = sqlite3_column_int(stmt, 7);
        pe->usage_out = sqlite3_column_int(stmt, 8);
        /* A report's model run: a new one starts whenever the model
         * differs from the previous report's. */
        if (pe->usage_in > 0) {
            const char *m = (const char *)sqlite3_column_text(stmt, 9);
            if (!m) m = "";
            if (model_run == 0 || strcmp(m, last_model) != 0) model_run++;
            snprintf(last_model, sizeof(last_model), "%s", m);
        }
        pe->model = model_run;
        pe->turn_edited = (unsigned char)sqlite3_column_int(stmt, 10);
        pe->repointed = (unsigned char)sqlite3_column_int(stmt, 11);
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
    out->head = plan_measure(filtered, fcount, head);
    return 0;
}

int context_plan(sqlite3 *db, int64_t session_id, const Config *cfg, int overhead, ContextPlan *out) {
    if (!db || !cfg || !out) return -1;
    if (branch_load(db, session_id, overhead, out) != 0) return -1;

    /* compute budget from context_threshold */
    int budget = cfg->max_history_tokens > 0
        ? cfg->max_history_tokens
        : (int)((cfg->context_threshold > 0 ? cfg->context_threshold : 0.6f)
                * (float)cfg->context_window);
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
    int max_chars = max_tokens * 4;
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

/* Resolve the effective context window for an agent's model.
 * Looks up the agent's model preference, finds the matching models row,
 * returns its context_window if non-NULL. Falls back to global_default. */
int model_context_window(sqlite3 *db, const char *agent_name, int global_default) {
    if (!db || !agent_name || !agent_name[0]) return global_default;

    /* Agent's model preference → matching models row's context_window */
    const char *sql =
        "SELECT m.context_window FROM agent_models am"
        " JOIN models m ON m.id = am.model_id"
        " WHERE am.agent_name = ? AND am.purpose = 'chat'"
        "   AND m.status != 'disabled'"
        " ORDER BY am.pos LIMIT 1;";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK)
        return global_default;
    sqlite3_bind_text(s, 1, agent_name, -1, SQLITE_STATIC);
    int result = global_default;
    if (sqlite3_step(s) == SQLITE_ROW && sqlite3_column_type(s, 0) != SQLITE_NULL)
        result = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return result > 0 ? result : global_default;
}

/* Head of a request: the tools array plus the system prompt, neither of
 * which is an entry. The cut has always budgeted for it (llm_req passes it
 * as `overhead`); the trigger must see the same number or it loses the race
 * to the cut by exactly the head's size and history gets dropped rather
 * than summarized. system_prompt may be NULL — then it is built and freed
 * here (the post-turn trigger); llm_req passes the one it already holds. */
int context_head_estimate(sqlite3 *db, const char *agent_name, int64_t session_id,
                          const Config *cfg, const char *system_prompt) {
    int head = 0;
    sqlite3_stmt *ts;
    if (sqlite3_prepare_v2(db,
            "SELECT COALESCE(SUM(length(name)+length(description)+length(parameters_json)),0)/3"
            " FROM tools WHERE enabled=1 AND (agent_name IS NULL OR agent_name=?)",
            -1, &ts, NULL) == SQLITE_OK) {
        sqlite3_bind_text(ts, 1, agent_name, -1, SQLITE_STATIC);
        if (sqlite3_step(ts) == SQLITE_ROW) head = sqlite3_column_int(ts, 0);
        sqlite3_finalize(ts);
    }
    char *built = NULL;
    if (!system_prompt) {
        built = agent_build_system_prompt(db, agent_name, session_id, "agents", cfg);
        system_prompt = built;
    }
    if (system_prompt) head += (int)strlen(system_prompt) / CHARS_PER_TOKEN;
    free(built);
    return head;
}

/* Compaction trigger. The size of the context is the larger of:
 *   1. the measured branch (branch_load): head + every entry's size, the
 *      provider's count where it reported and the estimate elsewhere. With
 *      no edit since the newest report that is at least that report plus
 *      what came after it; with no report at all it is the estimate.
 *   2. the request body we actually sent, /3 (llm_responses.request_body)
 *      for the newest response, less the estimate of what a turn edit has
 *      removed since (the $.turn_edits record on that response).
 * 2 is our own count of a whole request, framing included; absent or pruned
 * it drops out, and it is ignored when it predates the newest compaction (a
 * just-compacted session would otherwise re-read its pre-compaction bill and
 * compact again) — as is any report, by branch_load.
 *
 * The limit is threshold × window, the same ceiling the cut enforces, less
 * the size of the turn just completed: compaction only runs at a turn
 * boundary, so it must fire while the *next* turn (assumed to be like the
 * last) can still fit under the cut. That keeps compaction — one cache
 * invalidation, then append-only — ahead of the cut, which invalidates the
 * prompt-cache prefix on every request it moves. */
int session_needs_compaction(sqlite3 *db, int64_t session_id, const Config *cfg) {
    if (!db || !cfg || !cfg->compaction) return 0;
    float threshold = cfg->context_threshold > 0 ? cfg->context_threshold : 0.6f;

    char *agent = session_get_agent_name(db, session_id);
    int window = model_context_window(db, agent, cfg->context_window);
    int head = context_head_estimate(db, agent ? agent : "Assistant", session_id, cfg, NULL);
    free(agent);
    int token_limit = (int)(threshold * (float)window);
    if (token_limit <= 0) token_limit = 8000;

    ContextPlan b;
    if (branch_load(db, session_id, head, &b) != 0) return 0;
    int branch = 0, last_turn = 0, newest = -1;
    for (int i = 0; i < b.count; i++) {
        branch += b.entries[i].tokens;
        if (b.entries[i].turn_id == b.entries[b.count - 1].turn_id)
            last_turn += b.entries[i].tokens;
        if (b.entries[i].role == ROLE_ASSISTANT && b.entries[i].fresh) newest = i;
    }

    int sent = 0;
    if (newest >= 0) {
        /* Turn edits record on the answer itself (turn-edit.md, "The
         * record"), so the ones on the newest response are exactly those
         * made since that request was sent. */
        const char *sql =
            "SELECT (SELECT COALESCE(length(json(request_body)),0) FROM llm_responses"
            "         WHERE session_id=?1 AND request_body IS NOT NULL AND iteration_id=?2"
            "         ORDER BY id DESC LIMIT 1),"
            "       (SELECT COALESCE(SUM(json_extract(j.value,'$.tokens.before')"
            "                          - json_extract(j.value,'$.tokens.after')),0)"
            "          FROM entries n, json_each(CASE WHEN json_valid(n.data) THEN n.data END,"
            "                                    '$.turn_edits') j"
            "         WHERE n.id=?3);";
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, session_id);
            sqlite3_bind_int64(stmt, 2, b.entries[newest].iteration_id);
            sqlite3_bind_int64(stmt, 3, b.entries[newest].id);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                int body = sqlite3_column_int(stmt, 0);
                if (body > 0) sent = body / CHARS_PER_TOKEN - sqlite3_column_int(stmt, 1);
            }
            sqlite3_finalize(stmt);
        }
    }
    context_plan_free(&b);

    int size = b.head + branch;
    if (sent > size) size = sent;
    int fire = size > token_limit - last_turn;
    LOG_DEBUG_("compaction check session=%lld size=%d head=%d (estimate %d) branch=%d"
               " sent=%d last_turn=%d limit=%d fire=%d",
               (long long)session_id, size, b.head, head, branch, sent, last_turn,
               token_limit, fire);
    return fire;
}

/* Mid-turn overflow probe: the cut can only drop whole turns, so once the
 * current turn's own entries exceed what the window can hold beside the
 * head, no cut fits and the next request would 400 (E5). Compared against
 * the window itself, not the threshold — the threshold is a cost margin,
 * this is the wall. The turn's size is measured as branch_load measures it
 * — the provider's count for every segment it has reported, the results
 * since the newest request by estimate — and so is the head. Returns 1 when
 * the turn no longer fits; *turn_tokens gets the turn's size either way (0
 * when nothing is known). */
int context_turn_overflows(sqlite3 *db, int64_t session_id, int window, int head,
                           int *turn_tokens) {
    if (turn_tokens) *turn_tokens = 0;
    if (!db || window <= 0) return 0;
    ContextPlan b;
    if (branch_load(db, session_id, head, &b) != 0) return 0;
    int turn = 0;
    for (int i = 0; i < b.count; i++)
        if (b.entries[i].turn_id == b.entries[b.count - 1].turn_id)
            turn += b.entries[i].tokens;
    context_plan_free(&b);
    if (turn_tokens) *turn_tokens = turn;
    return turn > window - b.head;
}
