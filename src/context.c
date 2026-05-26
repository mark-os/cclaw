#define _POSIX_C_SOURCE 200809L
#include "context.h"
#include "db.h"
#include "templates.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define TRUNCATE_MAX_BYTES  (50 * 1024)
#define TRUNCATE_MAX_LINES  2000

static const char *CUTOFF_NOTICE = TPL_CUTOFF_NOTICE_TXT;

/* V17: notice injected when an incomplete turn is detected */
static const char *INCOMPLETE_TURN_NOTICE = TPL_INCOMPLETE_TURN_NOTICE_TXT;

static const char *INCOMPLETE_TOOL_CONTENT = TPL_INCOMPLETE_TOOL_CONTENT_TXT;

int context_estimate_tokens(const Message *msg) {
    int tokens = 4; /* per-message overhead */
    if (msg->content)
        tokens += (int)strlen(msg->content) / 4;
    if (msg->tool_calls) {
        for (size_t i = 0; i < msg->tool_call_count; i++) {
            if (msg->tool_calls[i].name)
                tokens += (int)strlen(msg->tool_calls[i].name) / 4;
            if (msg->tool_calls[i].arguments)
                tokens += (int)strlen(msg->tool_calls[i].arguments) / 4;
        }
    }
    if (msg->tool_result) {
        if (msg->tool_result->content)
            tokens += (int)strlen(msg->tool_result->content) / 4;
    }
    return tokens;
}

/* V40: Truncate tool result to 50KB / 2000 lines (whichever first). */
char *truncate_result(const char *src, size_t len) {
    if (!src) return NULL;
    if (len == 0) len = strlen(src);

    /* Find cut point: min(50KB, 2000 lines) */
    size_t cut = len;
    int lines = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '\n') {
            lines++;
            if (lines >= TRUNCATE_MAX_LINES) { cut = i + 1; break; }
        }
        if (i + 1 >= TRUNCATE_MAX_BYTES && cut == len) { cut = TRUNCATE_MAX_BYTES; break; }
    }
    if (cut >= len) return strndup(src, len);

    /* Build truncated string with suffix */
    size_t omitted_bytes = len - cut;
    int omitted_lines = 0;
    for (size_t i = cut; i < len; i++)
        if (src[i] == '\n') omitted_lines++;

    char suffix[128];
    snprintf(suffix, sizeof(suffix),
             "\n[truncated — %zu bytes / %d lines omitted, use search to find full output]",
             omitted_bytes, omitted_lines);

    size_t result_len = cut + strlen(suffix);
    char *result = malloc(result_len + 1);
    if (!result) return strndup(src, len);
    memcpy(result, src, cut);
    memcpy(result + cut, suffix, strlen(suffix) + 1);
    return result;
}

/* V8: Find valid cut point — never cut mid-tool-call.
 * A valid boundary is before a user msg or after a complete assistant response
 * (i.e., after all tool results that follow an assistant tool_calls msg).
 * Returns the index of the first message to include (cut everything before it). */
static int find_cut_point(const Entry *entries, int count, int budget) {
    /* Walk backwards from end, accumulating tokens.
     * Track "groups": an assistant msg with tool_calls + its tool results form a group. */
    int total = 0;
    int cut = 0; /* include everything by default */

    /* First pass: find where we'd exceed budget walking from the end */
    int i = count - 1;
    while (i >= 0) {
        /* Identify the group starting at entries[i] going backwards.
         * A group is either:
         *   - A single user/system/assistant-without-tool-calls message
         *   - An assistant-with-tool-calls + all following tool results */
        int group_start = i;
        int group_tokens = 0;

        if (entries[i].message.role == ROLE_TOOL) {
            /* Walk backwards to find the assistant msg that owns these tool results */
            while (group_start > 0 && entries[group_start - 1].message.role == ROLE_TOOL)
                group_start--;
            /* The message before the tool results should be the assistant with tool_calls */
            if (group_start > 0 && entries[group_start - 1].message.role == ROLE_ASSISTANT
                && entries[group_start - 1].message.tool_calls) {
                group_start--;
            }
            for (int j = group_start; j <= i; j++)
                group_tokens += context_estimate_tokens(&entries[j].message);
        } else {
            group_tokens = context_estimate_tokens(&entries[i].message);
        }

        if (total + group_tokens > budget) {
            cut = i + 1; /* can't fit this group */
            break;
        }
        total += group_tokens;
        i = group_start - 1;
    }

    /* V8: ensure cut is at a valid boundary — before a user msg or at start */
    while (cut < count && entries[cut].message.role != ROLE_USER
           && entries[cut].message.role != ROLE_SYSTEM) {
        cut++;
    }

    return cut;
}

/* V17: Count how many tool_results are missing at the tail of the entry list.
 * Returns the number of synthetic results needed, and sets *asst_idx to the
 * index of the incomplete assistant message. Returns 0 if tail is complete. */
static int count_missing_tool_results(const Entry *entries, int count, int *asst_idx) {
    if (count <= 0) return 0;

    /* Walk backwards from end to find the last assistant with tool_calls */
    int last_asst = -1;
    for (int i = count - 1; i >= 0; i--) {
        if (entries[i].message.role == ROLE_ASSISTANT && entries[i].message.tool_calls) {
            last_asst = i;
            break;
        }
        /* Stop if we hit a user message — turn boundary */
        if (entries[i].message.role == ROLE_USER) break;
    }
    if (last_asst < 0) return 0;

    /* Count tool_results that follow this assistant */
    int results_found = 0;
    for (int i = last_asst + 1; i < count; i++) {
        if (entries[i].message.role == ROLE_TOOL)
            results_found++;
        else
            break;
    }

    int expected = (int)entries[last_asst].message.tool_call_count;
    if (results_found >= expected) return 0;

    *asst_idx = last_asst;
    return expected - results_found;
}

int context_build(const Entry *entries, int count, const Config *cfg,
                  Message **out_msgs, int *out_count) {
    if (!entries || count <= 0 || !cfg || !out_msgs || !out_count)
        return -1;

    /* V28/V36: filter out errored/aborted assistant entries + their tool_results */
    Entry *v28 = malloc((size_t)count * sizeof(Entry));
    if (!v28) return -1;
    int v28count = 0;
    for (int i = 0; i < count; i++) {
        if (entries[i].message.role == ROLE_ASSISTANT &&
            (entries[i].message.stop_reason == STOP_REASON_ERROR ||
             entries[i].message.stop_reason == STOP_REASON_ABORTED)) {
            /* Skip this assistant entry and all following tool_results */
            int j = i + 1;
            while (j < count && entries[j].message.role == ROLE_TOOL)
                j++;
            i = j - 1; /* loop will increment */
            continue;
        }
        v28[v28count++] = entries[i];
    }

    /* V36: synthesize error tool_results for mid-conversation orphaned tool_calls.
     * Tail orphans are handled by V17 below (which also adds system notice).
     * Pass 1: count synthetics needed. Pass 2: build array with synthetics inserted. */
    int synth_needed = 0;
    for (int i = 0; i < v28count; i++) {
        if (v28[i].message.role == ROLE_ASSISTANT && v28[i].message.tool_calls &&
            v28[i].message.tool_call_count > 0) {
            int results = 0;
            for (int j = i + 1; j < v28count && v28[j].message.role == ROLE_TOOL; j++)
                results++;
            int missing = (int)v28[i].message.tool_call_count - results;
            /* Only count mid-conversation orphans (not tail — V17 handles tail) */
            if (missing > 0 && i + 1 + results < v28count)
                synth_needed += missing;
        }
    }

    /* Allocate synthetic ToolResult structs (process-lifetime, single-threaded) */
    static ToolResult synth_results[32];
    int synth_idx = 0;

    Entry *filtered = malloc((size_t)(v28count + synth_needed) * sizeof(Entry));
    if (!filtered) { free(v28); return -1; }
    int fcount = 0;

    for (int i = 0; i < v28count; i++) {
        filtered[fcount++] = v28[i];
        if (v28[i].message.role == ROLE_ASSISTANT && v28[i].message.tool_calls &&
            v28[i].message.tool_call_count > 0) {
            /* Copy existing tool_results that follow */
            int results = 0;
            while (i + 1 + results < v28count &&
                   v28[i + 1 + results].message.role == ROLE_TOOL)
                results++;
            for (int j = 0; j < results; j++)
                filtered[fcount++] = v28[i + 1 + j];
            /* Synthesize missing results only for mid-conversation orphans */
            int missing = (int)v28[i].message.tool_call_count - results;
            int is_tail = (i + 1 + results >= v28count);
            if (missing > 0 && !is_tail) {
                for (int m = 0; m < missing && synth_idx < 32; m++) {
                    synth_results[synth_idx].tool_call_id =
                        v28[i].message.tool_calls[results + m].id;
                    synth_results[synth_idx].content = (char *)INCOMPLETE_TOOL_CONTENT;
                    Entry synth = {0};
                    synth.message.role = ROLE_TOOL;
                    synth.message.tool_result = &synth_results[synth_idx];
                    filtered[fcount++] = synth;
                    synth_idx++;
                }
            }
            i += results; /* skip results already copied */
        }
    }
    free(v28);

    /* V7: budget = max_history_tokens if set, else 60% of context window */
    int budget = cfg->max_history_tokens > 0
        ? cfg->max_history_tokens
        : (cfg->provider.context_window * 60) / 100;
    if (budget <= 0) budget = 8000; /* fallback */

    int cut = find_cut_point(filtered, fcount, budget);
    int included = fcount - cut;
    int truncated = (cut > 0);

    /* V17: detect incomplete turn in the included tail */
    int asst_idx = 0;
    int missing = count_missing_tool_results(filtered + cut, included, &asst_idx);

    int extra = missing > 0 ? missing + 1 : 0; /* synthetic results + notice */
    int result_count = included + (truncated ? 1 : 0) + extra;
    Message *msgs = calloc((size_t)result_count, sizeof(Message));
    if (!msgs) { free(filtered); return -1; }

    int idx = 0;

    /* Prepend cutoff notice if truncated */
    if (truncated) {
        msgs[idx].role = ROLE_SYSTEM;
        msgs[idx].content = strdup(CUTOFF_NOTICE);
        idx++;
    }

    /* Copy included messages (deep copy strings so context_free works independently) */
    for (int i = cut; i < fcount; i++) {
        msgs[idx] = filtered[i].message;
        if (filtered[i].message.content)
            msgs[idx].content = strdup(filtered[i].message.content);
        if (filtered[i].message.tool_calls && filtered[i].message.tool_call_count > 0) {
            msgs[idx].tool_calls = malloc(filtered[i].message.tool_call_count * sizeof(ToolCall));
            for (size_t t = 0; t < filtered[i].message.tool_call_count; t++) {
                msgs[idx].tool_calls[t].id = filtered[i].message.tool_calls[t].id
                    ? strdup(filtered[i].message.tool_calls[t].id) : NULL;
                msgs[idx].tool_calls[t].name = filtered[i].message.tool_calls[t].name
                    ? strdup(filtered[i].message.tool_calls[t].name) : NULL;
                msgs[idx].tool_calls[t].arguments = filtered[i].message.tool_calls[t].arguments
                    ? strdup(filtered[i].message.tool_calls[t].arguments) : NULL;
            }
        }
        if (filtered[i].message.tool_result) {
            msgs[idx].tool_result = malloc(sizeof(ToolResult));
            msgs[idx].tool_result->tool_call_id = filtered[i].message.tool_result->tool_call_id
                ? strdup(filtered[i].message.tool_result->tool_call_id) : NULL;
            msgs[idx].tool_result->content = filtered[i].message.tool_result->content
                ? strdup(filtered[i].message.tool_result->content) : NULL;
        }
        idx++;
    }

    /* V17: synthesize missing tool_results + system notice */
    if (missing > 0) {
        const Entry *asst_entry = &filtered[cut + asst_idx];
        int existing_results = (int)asst_entry->message.tool_call_count - missing;
        for (int m = 0; m < missing; m++) {
            int tc_idx = existing_results + m;
            msgs[idx].role = ROLE_TOOL;
            msgs[idx].content = NULL;
            msgs[idx].tool_calls = NULL;
            msgs[idx].tool_call_count = 0;
            msgs[idx].tool_result = malloc(sizeof(ToolResult));
            msgs[idx].tool_result->tool_call_id =
                asst_entry->message.tool_calls[tc_idx].id
                    ? strdup(asst_entry->message.tool_calls[tc_idx].id) : NULL;
            msgs[idx].tool_result->content = strdup(INCOMPLETE_TOOL_CONTENT);
            idx++;
        }
        /* System notice about interruption */
        msgs[idx].role = ROLE_SYSTEM;
        msgs[idx].content = strdup(INCOMPLETE_TURN_NOTICE);
        idx++;
    }

    free(filtered);
    *out_msgs = msgs;
    *out_count = result_count;
    return 0;
}

void context_free(Message *msgs, int count) {
    if (!msgs) return;
    for (int i = 0; i < count; i++) {
        free(msgs[i].content);
        if (msgs[i].tool_calls) {
            for (size_t t = 0; t < msgs[i].tool_call_count; t++) {
                free(msgs[i].tool_calls[t].id);
                free(msgs[i].tool_calls[t].name);
                free(msgs[i].tool_calls[t].arguments);
            }
            free(msgs[i].tool_calls);
        }
        if (msgs[i].tool_result) {
            free(msgs[i].tool_result->tool_call_id);
            free(msgs[i].tool_result->content);
            free(msgs[i].tool_result);
        }
    }
    free(msgs);
}

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

int context_plan(sqlite3 *db, int64_t session_id, const Config *cfg, ContextPlan *out) {
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
            if (!tmp) break;
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

    /* V7: compute budget */
    int budget = cfg->max_history_tokens > 0
        ? cfg->max_history_tokens
        : (cfg->provider.context_window * 60) / 100;
    if (budget <= 0) budget = 8000;

    int cut = plan_find_cut(filtered, fcount, budget);

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

    /* Write full output to temp file */
    char dir[64];
    session_tmp_dir(session_id, dir, sizeof(dir));
    mkdir(dir, 0700);  /* ignore error if exists */

    char path[128];
    snprintf(path, sizeof(path), "%s/%s.out", dir, tool_call_id ? tool_call_id : "unknown");
    FILE *f = fopen(path, "w");
    if (f) {
        fwrite(src, 1, len, f);
        fclose(f);
    }

    /* Count total lines */
    int total_lines = 0;
    for (size_t i = 0; i < len; i++)
        if (src[i] == '\n') total_lines++;

    /* Build suffix with file path reference */
    char suffix[256];
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

void session_tmp_cleanup(int64_t session_id) {
    char dir[64];
    session_tmp_dir(session_id, dir, sizeof(dir));
    /* Simple recursive delete: list files and remove */
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    (void)system(cmd);
}

/* ── V58,T161: Compaction trigger ─────────────────────────────────── */

#define DEFAULT_COMPACTION_THRESHOLD 200

int session_needs_compaction(sqlite3 *db, int64_t session_id, const Config *cfg) {
    if (!db || !cfg) return 0;
    ContextPlan plan;
    if (context_plan(db, session_id, cfg, &plan) != 0) return 0;
    int threshold = cfg->compaction_threshold > 0
        ? cfg->compaction_threshold : DEFAULT_COMPACTION_THRESHOLD;
    int beyond_budget = plan.cut; /* entries before cut = excluded from context */
    context_plan_free(&plan);
    return beyond_budget > threshold;
}

int64_t session_try_compact(sqlite3 *db, int64_t session_id, const Config *cfg) {
    if (!db || !cfg) return -1;

    ContextPlan plan;
    if (context_plan(db, session_id, cfg, &plan) != 0) return -1;

    int threshold = cfg->compaction_threshold > 0
        ? cfg->compaction_threshold : DEFAULT_COMPACTION_THRESHOLD;
    if (plan.cut <= threshold) {
        context_plan_free(&plan);
        return 0; /* no compaction needed */
    }

    /* Determine reparent points:
     * last_kept_id = entry just before the compacted range (plan.entries[0] = root)
     * We compact entries [1..plan.cut-1] (skip root), keeping entries[0] as anchor.
     * last_kept_id = entries[0].id (the root/first entry)
     * first_after_id = entries[plan.cut].id (first entry that stays in context) */
    if (plan.count < 2 || plan.cut >= plan.count) {
        context_plan_free(&plan);
        return 0;
    }

    int64_t last_kept_id = plan.entries[0].id;
    int64_t first_after_id = plan.entries[plan.cut].id;

    /* T162 will replace this with an LLM-generated summary.
     * For now, produce a placeholder summary with entry count. */
    char summary[256];
    snprintf(summary, sizeof(summary),
             "[Compacted %d entries. Context continues below.]", plan.cut - 1);

    context_plan_free(&plan);

    /* Atomic reparent via entry_compact (V58,V59) */
    return entry_compact(db, session_id, last_kept_id, first_after_id, summary);
}
