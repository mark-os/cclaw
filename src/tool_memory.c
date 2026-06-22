#define _POSIX_C_SOURCE 200809L
#include "tool_memory.h"
#include "db.h"
#include "tool_parse.h"
#include "jsmn_util.h"
#include "json_escape.h"
#include "validate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM_TOK_CAP 512
#define MEM_MAX_ENTRIES 64

/* --- jsmn helpers --- */

static int jtok_key_eq(const jsmntok_t *t, const char *json, const char *key) {
    size_t klen = strlen(key);
    return t->type == JSMN_STRING && (size_t)(t->end - t->start) == klen &&
           memcmp(json + t->start, key, klen) == 0;
}

static int jtok_find(const jsmntok_t *t, int ntok, const char *json, const char *key) {
    int j = 1;
    for (int k = 0; k < t[0].size; k++) {
        int vi = j + 1;
        if (jtok_key_eq(&t[j], json, key)) return vi;
        j = jsmn_skip(t, vi, ntok);
    }
    return -1;
}

/* --- Render helper --- */

static char *render_block(sqlite3 *db, const char *agent, const char *label) {
    int count = 0;
    MemoryEntry *entries = memory_entries_list(db, agent, label, &count);
    MemoryBlock *mb = memory_block_get(db, agent, label);
    int limit = mb ? mb->char_limit : 0;

    if (count == 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s (empty)", label);
        if (mb) memory_block_free(mb);
        if (entries) memory_entries_free(entries, count);
        return strdup(buf);
    }

    /* Compute total chars and buffer size */
    size_t total_chars = 0;
    size_t est = 128;
    for (int i = 0; i < count; i++) {
        size_t tl = entries[i].text ? strlen(entries[i].text) : 0;
        total_chars += tl;
        est += tl + 16; /* "N. " + text + newline */
    }

    char *out = malloc(est);
    if (!out) {
        memory_entries_free(entries, count);
        if (mb) memory_block_free(mb);
        return strdup(label);
    }

    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, est - pos, "%s (%d entr%s, %zu/%d chars):\n",
                            label, count, count == 1 ? "y" : "ies",
                            total_chars, limit);
    for (int i = 0; i < count; i++) {
        pos += (size_t)snprintf(out + pos, est - pos, "%d. %s\n",
                                entries[i].pos, entries[i].text ? entries[i].text : "");
    }
    /* Trim trailing newline */
    if (pos > 0 && out[pos - 1] == '\n') out[--pos] = '\0';

    memory_entries_free(entries, count);
    if (mb) memory_block_free(mb);
    return out;
}

/* --- Tool schemas --- */

static const char *MEMORY_CREATE_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"label\":{\"type\":\"string\",\"description\":\"Unique label for this memory block\"},"
    "\"description\":{\"type\":\"string\",\"description\":\"What this block is for\"}"
    "},\"required\":[\"label\",\"description\"]}";

static const char *MEMORY_ADD_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"block\":{\"type\":\"string\",\"description\":\"Block label\"},"
    "\"text\":{\"type\":\"string\",\"description\":\"Text for the new entry\"}"
    "},\"required\":[\"block\",\"text\"]}";

static const char *MEMORY_EDIT_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"block\":{\"type\":\"string\",\"description\":\"Block label\"},"
    "\"edits\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{"
    "\"number\":{\"type\":\"integer\"},\"text\":{\"type\":\"string\"}},\"required\":[\"number\",\"text\"]}}"
    "},\"required\":[\"block\",\"edits\"]}";

static const char *MEMORY_DELETE_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"block\":{\"type\":\"string\",\"description\":\"Block label\"},"
    "\"numbers\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"},\"description\":\"Entry numbers to delete\"}"
    "},\"required\":[\"block\",\"numbers\"]}";

/* --- Handlers --- */

static char *tool_memory_create_handler(const char *arguments, void *user_data) {
    ToolMemoryCtx *ctx = (ToolMemoryCtx *)user_data;
    if (!ctx || !ctx->db || !ctx->agent_name)
        return strdup("error: memory tools unavailable (no agent context)");

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0) return strdup("error: invalid JSON arguments");

    const char *label = targ_str(&ta, "label");
    const char *desc = targ_str(&ta, "description");
    if (!label || !desc) {
        tool_parse_free(&ta);
        return strdup("error: 'label' and 'description' required");
    }
    if (!is_valid_name(label)) {
        tool_parse_free(&ta);
        return strdup("error: label must be alphanumeric (A-Z, a-z, 0-9, _, -)");
    }

    int64_t id = memory_block_create(ctx->db, ctx->agent_name, label, desc, "", 5000);
    if (id < 0) {
        tool_parse_free(&ta);
        return strdup("error: failed to create block (label may already exist)");
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "ok: memory block '%s' created", label);
    tool_parse_free(&ta);  /* frees label — build the message first */
    return strdup(buf);
}

static char *tool_memory_add_handler(const char *arguments, void *user_data) {
    ToolMemoryCtx *ctx = (ToolMemoryCtx *)user_data;
    if (!ctx || !ctx->db || !ctx->agent_name)
        return strdup("error: memory tools unavailable (no agent context)");

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0) return strdup("error: invalid JSON arguments");

    const char *block = targ_str(&ta, "block");
    const char *text = targ_str(&ta, "text");
    if (!block || !text) {
        tool_parse_free(&ta);
        return strdup("error: 'block' and 'text' required");
    }

    MemoryBlock *mb = memory_block_get(ctx->db, ctx->agent_name, block);
    if (!mb) { tool_parse_free(&ta); return strdup("error: block not found"); }
    if (mb->read_only) { memory_block_free(mb); tool_parse_free(&ta); return strdup("error: block is read-only"); }
    memory_block_free(mb);

    int rc = memory_entry_add_guarded(ctx->db, ctx->agent_name, block, text);
    if (rc == 0) { tool_parse_free(&ta); return strdup("error: would exceed char_limit"); }
    if (rc < 0) { tool_parse_free(&ta); return strdup("error: failed to add entry"); }

    /* Render while `block` (owned by ta) is still valid, then free ta. */
    char *out = render_block(ctx->db, ctx->agent_name, block);
    tool_parse_free(&ta);
    return out;
}

static char *tool_memory_edit_handler(const char *arguments, void *user_data) {
    ToolMemoryCtx *ctx = (ToolMemoryCtx *)user_data;
    if (!ctx || !ctx->db || !ctx->agent_name)
        return strdup("error: memory tools unavailable (no agent context)");

    jsmntok_t toks[MEM_TOK_CAP];
    jsmn_parser parser;
    jsmn_init(&parser);
    int ntoks = jsmn_parse(&parser, arguments, strlen(arguments), toks, MEM_TOK_CAP);
    if (ntoks < 1 || toks[0].type != JSMN_OBJECT)
        return strdup("error: invalid JSON arguments");

    int bvi = jtok_find(toks, ntoks, arguments, "block");
    int evi = jtok_find(toks, ntoks, arguments, "edits");
    if (bvi < 0 || toks[bvi].type != JSMN_STRING)
        return strdup("error: missing or invalid 'block'");
    if (evi < 0 || toks[evi].type != JSMN_ARRAY)
        return strdup("error: missing or invalid 'edits'");

    /* Extract block label */
    size_t blen = (size_t)(toks[bvi].end - toks[bvi].start);
    char *block = malloc(blen + 1);
    blen = json_unescape(block, blen + 1, arguments + toks[bvi].start, blen);
    block[blen] = '\0';

    MemoryBlock *mb = memory_block_get(ctx->db, ctx->agent_name, block);
    if (!mb) { free(block); return strdup("error: block not found"); }
    if (mb->read_only) { memory_block_free(mb); free(block); return strdup("error: block is read-only"); }
    memory_block_free(mb);

    int n_edits = toks[evi].size;
    if (n_edits > MEM_MAX_ENTRIES) { free(block); return strdup("error: too many edits"); }

    int succeeded = 0, failed = 0;
    char failed_nums[256] = "";
    size_t fpos = 0;

    int ei = evi + 1;
    for (int i = 0; i < n_edits; i++) {
        if (toks[ei].type != JSMN_OBJECT) { ei = jsmn_skip(toks, ei, ntoks); continue; }

        int number = -1;
        const char *tp = NULL;
        size_t tl = 0;

        int fj = ei + 1;
        for (int k = 0; k < toks[ei].size; k++) {
            const jsmntok_t *kt = &toks[fj];
            const jsmntok_t *vt = &toks[fj + 1];
            if (jtok_key_eq(kt, arguments, "number") && vt->type == JSMN_PRIMITIVE) {
                number = atoi(arguments + vt->start);
            } else if (jtok_key_eq(kt, arguments, "text") && vt->type == JSMN_STRING) {
                tp = arguments + vt->start;
                tl = (size_t)(vt->end - vt->start);
            }
            fj = jsmn_skip(toks, fj + 1, ntoks);
        }
        ei = jsmn_skip(toks, ei, ntoks);

        if (number < 1 || !tp) continue;

        char *text = malloc(tl + 1);
        tl = json_unescape(text, tl + 1, tp, tl);
        text[tl] = '\0';

        int rc = memory_entry_set(ctx->db, ctx->agent_name, block, number, text);
        free(text);
        if (rc == 0) {
            succeeded++;
        } else {
            failed++;
            fpos += (size_t)snprintf(failed_nums + fpos, sizeof(failed_nums) - fpos,
                                     "%s#%d", fpos > 0 ? ", " : "", number);
        }
    }

    char *rendered = render_block(ctx->db, ctx->agent_name, block);
    free(block);

    /* Build summary */
    char summary[384];
    if (failed > 0)
        snprintf(summary, sizeof(summary), "edited %d entr%s; no entry %s",
                 succeeded, succeeded == 1 ? "y" : "ies", failed_nums);
    else
        snprintf(summary, sizeof(summary), "edited %d entr%s",
                 succeeded, succeeded == 1 ? "y" : "ies");

    size_t rlen = rendered ? strlen(rendered) : 0;
    size_t slen = strlen(summary);
    char *out = malloc(rlen + slen + 2);
    snprintf(out, rlen + slen + 2, "%s\n%s", summary, rendered ? rendered : "");
    free(rendered);
    return out;
}

static char *tool_memory_delete_handler(const char *arguments, void *user_data) {
    ToolMemoryCtx *ctx = (ToolMemoryCtx *)user_data;
    if (!ctx || !ctx->db || !ctx->agent_name)
        return strdup("error: memory tools unavailable (no agent context)");

    jsmntok_t toks[MEM_TOK_CAP];
    jsmn_parser parser;
    jsmn_init(&parser);
    int ntoks = jsmn_parse(&parser, arguments, strlen(arguments), toks, MEM_TOK_CAP);
    if (ntoks < 1 || toks[0].type != JSMN_OBJECT)
        return strdup("error: invalid JSON arguments");

    int bvi = jtok_find(toks, ntoks, arguments, "block");
    int nvi = jtok_find(toks, ntoks, arguments, "numbers");
    if (bvi < 0 || toks[bvi].type != JSMN_STRING)
        return strdup("error: missing or invalid 'block'");
    if (nvi < 0 || toks[nvi].type != JSMN_ARRAY)
        return strdup("error: missing or invalid 'numbers'");

    /* Extract block label */
    size_t blen = (size_t)(toks[bvi].end - toks[bvi].start);
    char *block = malloc(blen + 1);
    blen = json_unescape(block, blen + 1, arguments + toks[bvi].start, blen);
    block[blen] = '\0';

    MemoryBlock *mb = memory_block_get(ctx->db, ctx->agent_name, block);
    if (!mb) { free(block); return strdup("error: block not found"); }
    if (mb->read_only) { memory_block_free(mb); free(block); return strdup("error: block is read-only"); }
    memory_block_free(mb);

    int n = toks[nvi].size;
    if (n > MEM_MAX_ENTRIES) { free(block); return strdup("error: too many numbers"); }

    int nums[MEM_MAX_ENTRIES];
    int ni = nvi + 1;
    for (int i = 0; i < n; i++) {
        nums[i] = atoi(arguments + toks[ni].start);
        ni++;
    }

    int deleted = memory_entries_delete(ctx->db, ctx->agent_name, block, nums, n);

    char *rendered = render_block(ctx->db, ctx->agent_name, block);
    free(block);

    char summary[64];
    snprintf(summary, sizeof(summary), "deleted %d entr%s", deleted, deleted == 1 ? "y" : "ies");

    size_t rlen = rendered ? strlen(rendered) : 0;
    size_t slen = strlen(summary);
    char *out = malloc(rlen + slen + 2);
    snprintf(out, rlen + slen + 2, "%s\n%s", summary, rendered ? rendered : "");
    free(rendered);
    return out;
}

/* --- Registration --- */

int tool_memory_register(ToolRegistry *reg, ToolMemoryCtx *ctx) {
    if (tools_register(reg, "memory_create",
                       "Create a new memory block (a named container of numbered notes). Args: label, description.",
                       MEMORY_CREATE_PARAMS, tool_memory_create_handler, ctx) != 0)
        return -1;
    if (tools_register(reg, "memory_add",
                       "Add a numbered note to a memory block. Args: block (label), text.",
                       MEMORY_ADD_PARAMS, tool_memory_add_handler, ctx) != 0)
        return -1;
    if (tools_register(reg, "memory_edit",
                       "Replace the text of existing notes by number. Args: block, edits (array of {number, text}).",
                       MEMORY_EDIT_PARAMS, tool_memory_edit_handler, ctx) != 0)
        return -1;
    if (tools_register(reg, "memory_delete",
                       "Delete notes by number (others renumber). Args: block, numbers (array of integers).",
                       MEMORY_DELETE_PARAMS, tool_memory_delete_handler, ctx) != 0)
        return -1;
    return 0;
}
