#define _POSIX_C_SOURCE 200809L
#include "context.h"
#include "arena.h"
#include "db.h"
#include "http.h"
#include "llm.h"
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define TRUNCATE_MAX_BYTES  (50 * 1024)
#define TRUNCATE_MAX_LINES  2000

/* V41: helpers for context_plan — integer column mapping */
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

/* V41,V8: Find cut point in PlanEntry array — same logic as find_cut_point but on PlanEntry. */
static int plan_find_cut(const PlanEntry *entries, int count, int budget) {
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
                group_tokens += entries[j].token_estimate;
        } else {
            group_tokens = entries[i].token_estimate;
        }

        if (total + group_tokens > budget) {
            cut = i + 1;
            break;
        }
        total += group_tokens;
        i = group_start - 1;
    }

    /* V8: ensure cut is at valid boundary — before user/system msg */
    while (cut < count && entries[cut].role != ROLE_USER
           && entries[cut].role != ROLE_SYSTEM)
        cut++;

    return cut;
}

int context_plan(sqlite3 *db, int64_t session_id, const Config *cfg, int overhead, ContextPlan *out) {
    if (!db || !cfg || !out) return -1;
    memset(out, 0, sizeof(*out));

    /* Get leaf_id */
    const char *leaf_sql = "SELECT leaf_id FROM sessions WHERE id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, leaf_sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, session_id);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return -1;
    }
    int64_t leaf_id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    if (leaf_id < 0) return -1;

    /* V56: Query branch metadata — covering index only, no main table access.
     * CTE walks leaf→root via parent_id; selects all plan columns inline
     * so the final SELECT needs no JOIN back to entries.
     * V58: use level counter for path ordering (compaction entries may have higher ids). */
    const char *plan_sql =
        "WITH RECURSIVE branch(id, parent_id, role, stop_reason, token_estimate, tool_call_count, lvl) AS ("
        "  SELECT id, parent_id, role, stop_reason, token_estimate, tool_call_count, 0"
        "    FROM entries WHERE id=? AND session_id=?"
        "  UNION ALL"
        "  SELECT e.id, e.parent_id, e.role, e.stop_reason, e.token_estimate, e.tool_call_count, b.lvl+1"
        "    FROM entries e JOIN branch b ON e.id=b.parent_id"
        ") SELECT id, role, stop_reason, token_estimate, tool_call_count"
        " FROM branch ORDER BY lvl DESC;";

    if (sqlite3_prepare_v2(db, plan_sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, leaf_id);
    sqlite3_bind_int64(stmt, 2, session_id);

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
        pe->token_estimate = sqlite3_column_int(stmt, 3);
        if (pe->token_estimate <= 0) {
            pe->token_estimate = 4;
        }
        pe->tool_call_count = sqlite3_column_int(stmt, 4);
        raw_count++;
    }
    sqlite3_finalize(stmt);

    if (raw_count == 0) { free(raw); return -1; }

    /* V28: filter out error/aborted assistant entries + their following tool_results */
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

    /* V7,V91: compute budget from context_threshold */
    int budget = cfg->max_history_tokens > 0
        ? cfg->max_history_tokens
        : (int)((cfg->context_threshold > 0 ? cfg->context_threshold : 0.6f)
                * (float)cfg->provider.context_window);
    if (budget <= 0) budget = 8000;

    /* Subtract fixed overhead (tool definitions, max_tokens reply reservation) */
    int effective = budget - overhead;
    if (effective < 256) effective = 256;

    int cut = plan_find_cut(filtered, fcount, effective);

    out->entries = filtered;
    out->count = fcount;
    out->cut = cut;
    out->budget = budget;
    return 0;
}

void context_plan_free(ContextPlan *plan) {
    if (!plan) return;
    free(plan->entries);
    plan->entries = NULL;
    plan->count = 0;
}

/* ── T118: Write-time truncation with spill to temp file ─────────── */

void session_tmp_dir(int64_t session_id, char *buf, size_t bufsz) {
    /* Spill into the agent workspace (natural lifetime, no symlink-prone
     * world-writable /tmp). Fall back to /tmp when no workspace is set
     * (CLI without a workspace, tests). */
    const char *ws = getenv("CCLAW_WORKSPACE");
    if (ws && ws[0])
        snprintf(buf, bufsz, "%s/spill/%lld", ws, (long long)session_id);
    else
        snprintf(buf, bufsz, "/tmp/cclaw-%lld", (long long)session_id);
}

char *truncate_and_spill(const char *src, int64_t session_id, const char *tool_call_id) {
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

    /* Write full output to a spill file. Create the immediate parent then the
     * session dir (one extra level under the workspace's spill/), ignoring
     * EEXIST. */
    char dir[PATH_MAX];
    session_tmp_dir(session_id, dir, sizeof(dir));
    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", dir);
    char *last_slash = strrchr(parent, '/');
    if (last_slash && last_slash != parent) {
        *last_slash = '\0';
        mkdir(parent, 0700);  /* ignore error if exists */
    }
    mkdir(dir, 0700);  /* ignore error if exists */

    /* The call_id is model-emitted: reject anything that isn't a flat,
     * filesystem-safe token before it becomes a path component. A '/' or ".."
     * would otherwise let the (unsandboxed) parent write ≤60KB anywhere the
     * agent UID reaches; fall back to the "unknown" sentinel inside dir. */
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
    char path[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/%s.out", dir, safe_id ? safe_id : "unknown");
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

/* T269: Auto-recall — FTS5 search across sessions for relevant context. */
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

/* V58,T161: Compaction trigger — recovered from f4b50e0's dead-code purge,
 * now driven post-turn by the worker-job path instead of synchronously. */
int session_needs_compaction(sqlite3 *db, int64_t session_id, const Config *cfg) {
    if (!db || !cfg || !cfg->compaction) return 0;
    float threshold = cfg->context_threshold > 0 ? cfg->context_threshold : 0.6f;
    int token_limit = (int)(threshold * (float)cfg->provider.context_window);
    if (token_limit <= 0) token_limit = 8000;

    const char *sql =
        "WITH RECURSIVE branch(id, parent_id, token_estimate) AS ("
        "  SELECT id, parent_id, token_estimate FROM entries"
        "    WHERE id=(SELECT leaf_id FROM sessions WHERE id=?) AND session_id=?"
        "  UNION ALL"
        "  SELECT e.id, e.parent_id, e.token_estimate"
        "    FROM entries e JOIN branch b ON e.id=b.parent_id"
        ") SELECT COALESCE(SUM(token_estimate),0) FROM branch;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_int64(stmt, 2, session_id);
    int total_tokens = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        total_tokens = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    return total_tokens > token_limit;
}
