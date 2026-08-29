#define _POSIX_C_SOURCE 200809L
#include "context.h"
#include "run_tool.h"
#include "config.h"
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

    if (cut >= count) cut = count - 1;   /* nothing fit — keep the newest turn */
    return snap_to_turn_start(entries, cut);
}

int context_compaction_keep_from(const ContextPlan *plan, int target_tokens) {
    if (!plan || plan->count <= 0) return 0;

    /* Walk backwards from the tail, accumulating until the target is spent. */
    int keep_from = 0;
    int cumulative = 0;
    for (int i = plan->count - 1; i >= 0; i--) {
        cumulative += plan->entries[i].token_estimate;
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

    /* Query branch metadata — CTE walks leaf→root by rowid seeks and
     * selects all plan columns inline so the final SELECT needs no JOIN
     * back to entries.
     * Use level counter for path ordering (compaction entries may have higher ids). */
    const char *plan_sql =
        "WITH RECURSIVE branch(id, parent_id, role, stop_reason, token_estimate, tool_call_count, turn_id, lvl) AS ("
        "  SELECT id, parent_id, role, stop_reason, token_estimate, tool_call_count, turn_id, 0"
        "    FROM entries WHERE id=? AND session_id=?"
        "  UNION ALL"
        "  SELECT e.id, e.parent_id, e.role, e.stop_reason, e.token_estimate, e.tool_call_count, e.turn_id, b.lvl+1"
        "    FROM entries e JOIN branch b ON e.id=b.parent_id"
        ") SELECT id, role, stop_reason, token_estimate, tool_call_count, turn_id"
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
        pe->turn_id = sqlite3_column_int64(stmt, 5);
        raw_count++;
    }
    sqlite3_finalize(stmt);

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

    /* Hook suppress directives (this request only): drop suppressed entries
     * before budgeting so they don't eat the cut. Pin conflicts were already
     * resolved at apply time (hook_apply_pre_advance never inserts a suppress
     * for a pinned entry). */
    {
        sqlite3_stmt *ss;
        if (sqlite3_prepare_v2(db,
                "SELECT entry_id FROM hook_directives"
                " WHERE session_id=?1 AND kind='suppress' AND entry_id IS NOT NULL;",
                -1, &ss, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(ss, 1, session_id);
            while (sqlite3_step(ss) == SQLITE_ROW) {
                int64_t sup = sqlite3_column_int64(ss, 0);
                int w = 0;
                for (int i = 0; i < fcount; i++)
                    if (filtered[i].id != sup) filtered[w++] = filtered[i];
                fcount = w;
            }
            sqlite3_finalize(ss);
        }
    }

    /* compute budget from context_threshold */
    int budget = cfg->max_history_tokens > 0
        ? cfg->max_history_tokens
        : (int)((cfg->context_threshold > 0 ? cfg->context_threshold : 0.6f)
                * (float)cfg->context_window);
    if (budget <= 0) budget = 8000;

    /* Subtract fixed overhead (tool definitions, max_tokens reply reservation) */
    int effective = budget - overhead;
    if (effective < 256) effective = 256;

    int cut = plan_find_cut(filtered, fcount, effective);

    /* Include = past the cut, or pinned (entries.data $.pin=1 — hook `pin`
     * command). Pinned entries survive the cut but still count nothing toward
     * it; only the cut boundary honors pin — compaction's keep_from walk
     * ignores it (known gap, accepted).
     *
     * The asymmetry is deliberate: the cut hides an entry for one request, so
     * re-including a pinned one is free, while a pin that vetoed compaction
     * would let a single entry hold an unbounded branch in the tree forever.
     * State that must survive compaction rides the mechanical coda instead
     * (compaction_state_coda, llm_proc.c) — see specs/memory.md. */
    for (int i = 0; i < fcount; i++)
        filtered[i].include = (i >= cut);
    if (cut > 0) {
        sqlite3_stmt *ps;
        if (sqlite3_prepare_v2(db,
                "SELECT id FROM entries WHERE session_id=?1"
                " AND json_extract(data,'$.pin')=1;",
                -1, &ps, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(ps, 1, session_id);
            while (sqlite3_step(ps) == SQLITE_ROW) {
                int64_t pid = sqlite3_column_int64(ps, 0);
                for (int i = 0; i < cut; i++)
                    if (filtered[i].id == pid) { filtered[i].include = 1; break; }
            }
            sqlite3_finalize(ps);
        }
    }

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
        " WHERE am.agent_name = ? AND m.status != 'disabled'"
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

/* Compaction trigger — recovered from f4b50e0's dead-code purge,
 * now driven post-turn by the worker-job path instead of synchronously. */
int session_needs_compaction(sqlite3 *db, int64_t session_id, const Config *cfg) {
    if (!db || !cfg || !cfg->compaction) return 0;
    float threshold = cfg->context_threshold > 0 ? cfg->context_threshold : 0.6f;

    /* Resolve effective context window from the session's agent model */
    char *agent = session_get_agent_name(db, session_id);
    int window = model_context_window(db, agent, cfg->context_window);
    free(agent);
    int token_limit = (int)(threshold * (float)window);
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
