#define _POSIX_C_SOURCE 200809L
#include "tool_memory.h"
#include "db.h"
#include "validate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM_MAX_ENTRIES 64

/* --- Argument parsing (via SQLite's JSON1, not jsmn) ---
 * This tool always runs in-process with a live db handle (EXEC_THREAD,
 * never forked into the DB-less --run-tool child), so there's no reason to
 * hand-tokenize "arguments" — SQLite already owns JSON parsing for data we
 * hold a db connection for. See AGENTS.md: jsmn is for untrusted/streaming
 * JSON with no db behind it; this isn't that. */

/* Top-level string field, or NULL if absent/wrong-type/malformed JSON. */
static char *arg_str(sqlite3 *db, const char *args_json, const char *path) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT CASE WHEN json_type(?1,?2)='text' THEN json_extract(?1,?2) END",
            -1, &st, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(st, 1, args_json, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) out = strdup(v);
    }
    sqlite3_finalize(st);
    return out;
}

/* True iff args_json's field at path is a JSON array (false if absent,
 * a different type, or args_json itself isn't valid JSON). */
static int arg_is_array(sqlite3 *db, const char *args_json, const char *path) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "SELECT json_type(?1,?2)", -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, args_json, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC);
    int is_arr = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(st, 0);
        is_arr = t && strcmp(t, "array") == 0;
    }
    sqlite3_finalize(st);
    return is_arr;
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
    "\"description\":{\"type\":\"string\",\"description\":\"What this block is for\"},"
    "\"placement\":{\"type\":\"string\",\"enum\":[\"system\",\"context\"],"
    "\"description\":\"'context' (default): rendered in the session context block,"
    " re-materialized at each turn start."
    " 'system': baked into the system prompt once per session — reserve for stable"
    " identity content\"}"
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

    char *label = arg_str(ctx->db, arguments, "$.label");
    char *desc = arg_str(ctx->db, arguments, "$.description");
    char *placement = arg_str(ctx->db, arguments, "$.placement");
    if (!label || !desc) {
        free(label); free(desc); free(placement);
        return strdup("error: 'label' and 'description' required");
    }
    if (!is_valid_name(label)) {
        free(label); free(desc); free(placement);
        return strdup("error: label must be alphanumeric (A-Z, a-z, 0-9, _, -)");
    }
    if (placement && strcmp(placement, "system") != 0 && strcmp(placement, "context") != 0) {
        free(label); free(desc); free(placement);
        return strdup("error: placement must be 'system' or 'context'");
    }

    int64_t id = memory_block_create(ctx->db, ctx->agent_name, label, desc, "", 5000, placement);
    free(desc); free(placement);
    if (id < 0) {
        free(label);
        return strdup("error: failed to create block (label may already exist)");
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "ok: memory block '%s' created", label);
    free(label);
    return strdup(buf);
}

static char *tool_memory_add_handler(const char *arguments, void *user_data) {
    ToolMemoryCtx *ctx = (ToolMemoryCtx *)user_data;
    if (!ctx || !ctx->db || !ctx->agent_name)
        return strdup("error: memory tools unavailable (no agent context)");

    char *block = arg_str(ctx->db, arguments, "$.block");
    char *text = arg_str(ctx->db, arguments, "$.text");
    if (!block || !text) {
        free(block); free(text);
        return strdup("error: 'block' and 'text' required");
    }

    MemoryBlock *mb = memory_block_get(ctx->db, ctx->agent_name, block);
    if (!mb) { free(block); free(text); return strdup("error: block not found"); }
    if (mb->read_only) { memory_block_free(mb); free(block); free(text); return strdup("error: block is read-only"); }
    memory_block_free(mb);

    int rc = memory_entry_add_guarded(ctx->db, ctx->agent_name, block, text);
    free(text);
    if (rc == 0) { free(block); return strdup("error: would exceed char_limit"); }
    if (rc < 0) { free(block); return strdup("error: failed to add entry"); }

    char *out = render_block(ctx->db, ctx->agent_name, block);
    free(block);
    return out;
}

static char *tool_memory_edit_handler(const char *arguments, void *user_data) {
    ToolMemoryCtx *ctx = (ToolMemoryCtx *)user_data;
    if (!ctx || !ctx->db || !ctx->agent_name)
        return strdup("error: memory tools unavailable (no agent context)");

    char *block = arg_str(ctx->db, arguments, "$.block");
    if (!block) return strdup("error: missing or invalid 'block'");
    if (!arg_is_array(ctx->db, arguments, "$.edits")) {
        free(block);
        return strdup("error: missing or invalid 'edits'");
    }

    MemoryBlock *mb = memory_block_get(ctx->db, ctx->agent_name, block);
    if (!mb) { free(block); return strdup("error: block not found"); }
    if (mb->read_only) { memory_block_free(mb); free(block); return strdup("error: block is read-only"); }
    memory_block_free(mb);

    sqlite3_stmt *cnt;
    int n_edits = 0;
    if (sqlite3_prepare_v2(ctx->db,
            "SELECT COUNT(*) FROM json_each(json_extract(?1,'$.edits'))",
            -1, &cnt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(cnt, 1, arguments, -1, SQLITE_STATIC);
        if (sqlite3_step(cnt) == SQLITE_ROW) n_edits = sqlite3_column_int(cnt, 0);
        sqlite3_finalize(cnt);
    }
    if (n_edits > MEM_MAX_ENTRIES) { free(block); return strdup("error: too many edits"); }

    int succeeded = 0, failed = 0;
    char failed_nums[256] = "";
    size_t fpos = 0;

    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(ctx->db,
            "SELECT json_extract(value,'$.number'), json_extract(value,'$.text')"
            " FROM json_each(json_extract(?1,'$.edits'))",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, arguments, -1, SQLITE_STATIC);
        while (sqlite3_step(st) == SQLITE_ROW) {
            int number = (sqlite3_column_type(st, 0) == SQLITE_INTEGER)
                             ? sqlite3_column_int(st, 0) : -1;
            const char *text = (const char *)sqlite3_column_text(st, 1);
            if (number < 1 || !text) continue;

            int rc = memory_entry_set(ctx->db, ctx->agent_name, block, number, text);
            if (rc == 0) {
                succeeded++;
            } else {
                failed++;
                fpos += (size_t)snprintf(failed_nums + fpos, sizeof(failed_nums) - fpos,
                                         "%s#%d", fpos > 0 ? ", " : "", number);
            }
        }
        sqlite3_finalize(st);
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

    char *block = arg_str(ctx->db, arguments, "$.block");
    if (!block) return strdup("error: missing or invalid 'block'");
    if (!arg_is_array(ctx->db, arguments, "$.numbers")) {
        free(block);
        return strdup("error: missing or invalid 'numbers'");
    }

    MemoryBlock *mb = memory_block_get(ctx->db, ctx->agent_name, block);
    if (!mb) { free(block); return strdup("error: block not found"); }
    if (mb->read_only) { memory_block_free(mb); free(block); return strdup("error: block is read-only"); }
    memory_block_free(mb);

    sqlite3_stmt *cnt;
    int n = 0;
    if (sqlite3_prepare_v2(ctx->db,
            "SELECT COUNT(*) FROM json_each(json_extract(?1,'$.numbers'))",
            -1, &cnt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(cnt, 1, arguments, -1, SQLITE_STATIC);
        if (sqlite3_step(cnt) == SQLITE_ROW) n = sqlite3_column_int(cnt, 0);
        sqlite3_finalize(cnt);
    }
    if (n > MEM_MAX_ENTRIES) { free(block); return strdup("error: too many numbers"); }

    int nums[MEM_MAX_ENTRIES];
    int idx = 0;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(ctx->db,
            "SELECT value FROM json_each(json_extract(?1,'$.numbers'))",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, arguments, -1, SQLITE_STATIC);
        while (idx < MEM_MAX_ENTRIES && sqlite3_step(st) == SQLITE_ROW)
            nums[idx++] = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }

    int deleted = memory_entries_delete(ctx->db, ctx->agent_name, block, nums, idx);

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

/* --- EXEC_THREAD shims: rebuild the ctx around the thread's own db handle --- */
static char *memory_create_thread_run(sqlite3 *db, const char *agent_name,
                                      int64_t session_id, const char *args) {
    (void)session_id;
    ToolMemoryCtx c = {.db = db, .agent_name = agent_name};
    return tool_memory_create_handler(args, &c);
}
static char *memory_add_thread_run(sqlite3 *db, const char *agent_name,
                                   int64_t session_id, const char *args) {
    (void)session_id;
    ToolMemoryCtx c = {.db = db, .agent_name = agent_name};
    return tool_memory_add_handler(args, &c);
}
static char *memory_edit_thread_run(sqlite3 *db, const char *agent_name,
                                    int64_t session_id, const char *args) {
    (void)session_id;
    ToolMemoryCtx c = {.db = db, .agent_name = agent_name};
    return tool_memory_edit_handler(args, &c);
}
static char *memory_delete_thread_run(sqlite3 *db, const char *agent_name,
                                      int64_t session_id, const char *args) {
    (void)session_id;
    ToolMemoryCtx c = {.db = db, .agent_name = agent_name};
    return tool_memory_delete_handler(args, &c);
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
    /* All DB-only — fire-and-forget threads with their own db handle. */
    tools_set_recipe(reg, "memory_create", (ToolRecipe){EXEC_THREAD, SBX_NONE, memory_create_thread_run});
    tools_set_recipe(reg, "memory_add",    (ToolRecipe){EXEC_THREAD, SBX_NONE, memory_add_thread_run});
    tools_set_recipe(reg, "memory_edit",   (ToolRecipe){EXEC_THREAD, SBX_NONE, memory_edit_thread_run});
    tools_set_recipe(reg, "memory_delete", (ToolRecipe){EXEC_THREAD, SBX_NONE, memory_delete_thread_run});
    return 0;
}
