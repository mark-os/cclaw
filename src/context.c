#define _POSIX_C_SOURCE 200809L
#include "context.h"
#include <stdlib.h>
#include <string.h>

static const char *CUTOFF_NOTICE =
    "[Earlier conversation history was truncated to fit context window. "
    "Use search to find older messages.]";

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

int context_build(const Entry *entries, int count, const Config *cfg,
                  Message **out_msgs, int *out_count) {
    if (!entries || count <= 0 || !cfg || !out_msgs || !out_count)
        return -1;

    /* V7: budget = 60% of context window */
    int budget = (cfg->provider.context_window * 60) / 100;
    if (budget <= 0) budget = 8000; /* fallback */

    int cut = find_cut_point(entries, count, budget);
    int included = count - cut;
    int truncated = (cut > 0);

    int result_count = included + (truncated ? 1 : 0);
    Message *msgs = calloc((size_t)result_count, sizeof(Message));
    if (!msgs) return -1;

    int idx = 0;

    /* Prepend cutoff notice if truncated */
    if (truncated) {
        msgs[idx].role = ROLE_SYSTEM;
        msgs[idx].content = strdup(CUTOFF_NOTICE);
        idx++;
    }

    /* Copy included messages (shallow copy — caller must not free entry strings) */
    for (int i = cut; i < count; i++) {
        msgs[idx] = entries[i].message;
        /* Deep copy strings so context_free works independently */
        if (entries[i].message.content)
            msgs[idx].content = strdup(entries[i].message.content);
        if (entries[i].message.tool_calls && entries[i].message.tool_call_count > 0) {
            msgs[idx].tool_calls = malloc(entries[i].message.tool_call_count * sizeof(ToolCall));
            for (size_t t = 0; t < entries[i].message.tool_call_count; t++) {
                msgs[idx].tool_calls[t].id = entries[i].message.tool_calls[t].id
                    ? strdup(entries[i].message.tool_calls[t].id) : NULL;
                msgs[idx].tool_calls[t].name = entries[i].message.tool_calls[t].name
                    ? strdup(entries[i].message.tool_calls[t].name) : NULL;
                msgs[idx].tool_calls[t].arguments = entries[i].message.tool_calls[t].arguments
                    ? strdup(entries[i].message.tool_calls[t].arguments) : NULL;
            }
        }
        if (entries[i].message.tool_result) {
            msgs[idx].tool_result = malloc(sizeof(ToolResult));
            msgs[idx].tool_result->tool_call_id = entries[i].message.tool_result->tool_call_id
                ? strdup(entries[i].message.tool_result->tool_call_id) : NULL;
            msgs[idx].tool_result->content = entries[i].message.tool_result->content
                ? strdup(entries[i].message.tool_result->content) : NULL;
        }
        idx++;
    }

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
