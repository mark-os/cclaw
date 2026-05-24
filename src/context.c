#define _POSIX_C_SOURCE 200809L
#include "context.h"
#include <stdlib.h>
#include <string.h>

static const char *CUTOFF_NOTICE =
    "[Earlier conversation history was truncated to fit context window. "
    "Use search to find older messages.]";

/* V17: notice injected when an incomplete turn is detected */
static const char *INCOMPLETE_TURN_NOTICE =
    "[Previous turn was interrupted. Tool results may be incomplete. Retry if needed.]";

static const char *INCOMPLETE_TOOL_CONTENT =
    "error: process terminated during execution";

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
