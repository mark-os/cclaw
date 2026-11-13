#define _POSIX_C_SOURCE 200809L
#include "tool_memory.h"
#include "db.h"
#include "validate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MEM_MAX_NOTES 64

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

/* What a note tool returns: the block as it now stands, with the same
 * [id] handles the prompt shows, so the model's next call uses live ids. */
static char *render_block(sqlite3 *db, const MemoryBlock *mb) {
    int count = 0;
    MemoryNote *notes = memory_notes_list(db, mb->id, &count);
    if (count == 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%.63s (no notes)", mb->label);
        return strdup(buf);
    }

    size_t total_chars = 0;
    size_t est = 160;
    for (int i = 0; i < count; i++) {
        size_t tl = strlen(notes[i].text);
        total_chars += tl;
        est += tl + 48; /* "[id date] " + text + newline */
    }

    char *out = malloc(est);
    if (!out) { memory_notes_free(notes, count); return strdup(mb->label); }

    size_t pos = 0;
    pos += (size_t)snprintf(out + pos, est - pos, "%s (%d note%s, %zu/%d chars):\n",
                            mb->label, count, count == 1 ? "" : "s",
                            total_chars, mb->char_limit);
    for (int i = 0; i < count; i++) {
        char day[16] = "";
        time_t t = (time_t)notes[i].updated_at;
        struct tm tm;
        if (gmtime_r(&t, &tm)) strftime(day, sizeof(day), "%Y-%m-%d", &tm);
        pos += (size_t)snprintf(out + pos, est - pos, "[%lld %s] %s\n",
                                (long long)notes[i].id, day, notes[i].text);
    }
    if (pos > 0 && out[pos - 1] == '\n') out[--pos] = '\0';

    memory_notes_free(notes, count);
    return out;
}

static char *with_summary(const char *summary, char *rendered) {
    size_t n = strlen(summary) + (rendered ? strlen(rendered) : 0) + 2;
    char *out = malloc(n);
    if (!out) { free(rendered); return strdup(summary); }
    snprintf(out, n, "%s\n%s", summary, rendered ? rendered : "");
    free(rendered);
    return out;
}

/* --- Tool schemas --- */

#define PLACEMENT_PARAM \
    "\"placement\":{\"type\":\"string\",\"enum\":[\"system\",\"context\"]," \
    "\"description\":\"'context' (default for a new block): shown in the session" \
    " context block, refreshed at each turn start — right for notes you edit." \
    " 'system': part of the system prompt, rendered once per session — only" \
    " for stable identity content\"}"

static const char *BLOCK_CREATE_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"label\":{\"type\":\"string\",\"description\":\"Unique label for the block\"},"
    "\"description\":{\"type\":\"string\",\"description\":\"What the block is for and what belongs in it\"},"
    PLACEMENT_PARAM
    "},\"required\":[\"label\",\"description\"]}";

static const char *BLOCK_UPDATE_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"block\":{\"type\":\"string\",\"description\":\"Block label\"},"
    "\"description\":{\"type\":\"string\",\"description\":\"New description (omit to keep)\"},"
    PLACEMENT_PARAM
    "},\"required\":[\"block\"]}";

static const char *NOTE_ADD_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"block\":{\"type\":\"string\",\"description\":\"Block label\"},"
    "\"text\":{\"type\":\"string\",\"description\":\"Text of the new note\"}"
    "},\"required\":[\"block\",\"text\"]}";

static const char *NOTE_EDIT_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"block\":{\"type\":\"string\",\"description\":\"Block label\"},"
    "\"notes\":{\"type\":\"array\",\"items\":{\"type\":\"object\",\"properties\":{"
    "\"id\":{\"type\":\"integer\",\"description\":\"Note id, the number shown in [brackets]\"},"
    "\"text\":{\"type\":\"string\"}},\"required\":[\"id\",\"text\"]}}"
    "},\"required\":[\"block\",\"notes\"]}";

static const char *NOTE_DELETE_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"block\":{\"type\":\"string\",\"description\":\"Block label\"},"
    "\"ids\":{\"type\":\"array\",\"items\":{\"type\":\"integer\"},"
    "\"description\":\"Note ids to delete, the numbers shown in [brackets]\"}"
    "},\"required\":[\"block\",\"ids\"]}";

/* --- Handlers --- */

/* The char_limit is the one thing that forces a block to stay clean, so the
 * refusal says what to do about it and what a note is for. */
#define FULL_MSG \
    "the block is full (char_limit). Make room first: delete notes that are" \
    " out of date or no longer change what you do, and merge ones that" \
    " overlap. Keep facts that change what you do next time, not a record of" \
    " what happened."

#define NO_CTX "error: memory tools unavailable (no agent context)"

/* The block this agent may change, or NULL with *err set to the reason. */
static MemoryBlock *writable_block(ToolMemoryCtx *ctx, const char *label, const char **err) {
    MemoryBlock *mb = memory_block_get(ctx->db, ctx->agent_name, label);
    if (!mb) { *err = "error: block not found"; return NULL; }
    if (mb->read_only) {
        memory_block_free(mb);
        *err = "error: block is read-only";
        return NULL;
    }
    return mb;
}

static int placement_ok(const char *p) {
    return !p || strcmp(p, "system") == 0 || strcmp(p, "context") == 0;
}

static char *tool_block_create_handler(const char *arguments, void *user_data, int *is_error) {
    ToolMemoryCtx *ctx = (ToolMemoryCtx *)user_data;
    if (!ctx || !ctx->db || !ctx->agent_name) return tool_fail(is_error, NO_CTX);

    char *label = arg_str(ctx->db, arguments, "$.label");
    char *desc = arg_str(ctx->db, arguments, "$.description");
    char *placement = arg_str(ctx->db, arguments, "$.placement");
    const char *err = NULL;
    if (!label || !desc) err = "error: 'label' and 'description' required";
    else if (!is_valid_name(label)) err = "error: label must be alphanumeric (A-Z, a-z, 0-9, _, -)";
    else if (!placement_ok(placement)) err = "error: placement must be 'system' or 'context'";
    else if (memory_block_create(ctx->db, ctx->agent_name, label, desc, 5000, placement) < 0)
        err = "error: failed to create block (you already have a block with that label)";

    char buf[160];
    if (!err) snprintf(buf, sizeof(buf), "ok: memory block '%.63s' created", label);
    free(label); free(desc); free(placement);
    return err ? tool_fail(is_error, "%s", err) : strdup(buf);
}

static char *tool_block_update_handler(const char *arguments, void *user_data, int *is_error) {
    ToolMemoryCtx *ctx = (ToolMemoryCtx *)user_data;
    if (!ctx || !ctx->db || !ctx->agent_name) return tool_fail(is_error, NO_CTX);

    char *block = arg_str(ctx->db, arguments, "$.block");
    char *desc = arg_str(ctx->db, arguments, "$.description");
    char *placement = arg_str(ctx->db, arguments, "$.placement");
    const char *err = NULL;
    MemoryBlock *mb = NULL;
    if (!block) err = "error: missing or invalid 'block'";
    else if (!desc && !placement) err = "error: give 'description' and/or 'placement'";
    else if (!placement_ok(placement)) err = "error: placement must be 'system' or 'context'";
    else if ((mb = writable_block(ctx, block, &err)) != NULL &&
             memory_block_update(ctx->db, mb->id, desc, placement) != 0)
        err = "error: failed to update block";

    char buf[256];
    if (!err)
        snprintf(buf, sizeof(buf), "ok: memory block '%.63s' updated%s", block,
                 placement ? " (a placement change shows from the next turn; a move"
                             " to or from the system prompt from the next session)" : "");
    if (mb) memory_block_free(mb);
    free(block); free(desc); free(placement);
    return err ? tool_fail(is_error, "%s", err) : strdup(buf);
}

static char *tool_note_add_handler(const char *arguments, void *user_data, int *is_error) {
    ToolMemoryCtx *ctx = (ToolMemoryCtx *)user_data;
    if (!ctx || !ctx->db || !ctx->agent_name) return tool_fail(is_error, NO_CTX);

    char *block = arg_str(ctx->db, arguments, "$.block");
    char *text = arg_str(ctx->db, arguments, "$.text");
    const char *err = NULL;
    MemoryBlock *mb = NULL;
    int64_t id = 0;
    if (!block || !text) err = "error: 'block' and 'text' required";
    else if ((mb = writable_block(ctx, block, &err)) != NULL) {
        id = memory_note_add_guarded(ctx->db, mb->id, text);
        if (id < 0) err = "error: failed to add note";
    }

    char *out = NULL;
    if (!err && id == 0) {
        /* Full: hand back the block so the pruning can happen in the next
         * call, and say what is worth keeping. */
        out = with_summary(FULL_MSG, render_block(ctx->db, mb));
        *is_error = 1;
    } else if (!err) {
        char summary[64];
        snprintf(summary, sizeof(summary), "added note [%lld]", (long long)id);
        out = with_summary(summary, render_block(ctx->db, mb));
    }
    if (mb) memory_block_free(mb);
    free(block); free(text);
    return err ? tool_fail(is_error, "%s", err) : out;
}

/* Count of a top-level array argument, or -1 if it is not an array. */
static int arg_array_len(sqlite3 *db, const char *args, const char *path) {
    if (!arg_is_array(db, args, path)) return -1;
    sqlite3_stmt *st;
    int n = 0;
    if (sqlite3_prepare_v2(db, "SELECT json_array_length(?1,?2)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, args, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return n;
}

/* Append "[id]" to a comma list; only what fits whole (snprintf returns the
 * length it *wanted* — adding that unchecked walks past the array). */
static void list_append(char *buf, size_t cap, size_t *pos, long long id) {
    int w = snprintf(buf + *pos, cap - *pos, "%s[%lld]", *pos > 0 ? ", " : "", id);
    if (w > 0 && (size_t)w < cap - *pos) *pos += (size_t)w;
    else buf[*pos] = '\0';
}

/* Edit and delete share a shape: collect (id, text) pairs FIRST, then write.
 * json_each over a bound parameter pins no snapshot today, but collecting
 * keeps the loop independent of that. */
typedef struct { int64_t id; char *text; } NoteArg;

static int collect_notes(sqlite3 *db, const char *arguments, const char *sql,
                         NoteArg *out, int cap) {
    sqlite3_stmt *st;
    int n = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, arguments, -1, SQLITE_STATIC);
    while (n < cap && sqlite3_step(st) == SQLITE_ROW) {
        if (sqlite3_column_type(st, 0) != SQLITE_INTEGER) continue;
        out[n].id = sqlite3_column_int64(st, 0);
        const char *t = sqlite3_column_count(st) > 1
                            ? (const char *)sqlite3_column_text(st, 1) : NULL;
        out[n].text = t ? strdup(t) : NULL;
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

static char *tool_note_edit_handler(const char *arguments, void *user_data, int *is_error) {
    ToolMemoryCtx *ctx = (ToolMemoryCtx *)user_data;
    if (!ctx || !ctx->db || !ctx->agent_name) return tool_fail(is_error, NO_CTX);

    char *block = arg_str(ctx->db, arguments, "$.block");
    if (!block) return tool_fail(is_error, "error: missing or invalid 'block'");
    int n = arg_array_len(ctx->db, arguments, "$.notes");
    const char *err = NULL;
    MemoryBlock *mb = NULL;
    if (n < 0) err = "error: missing or invalid 'notes'";
    else if (n > MEM_MAX_NOTES) err = "error: too many notes in one call";
    else mb = writable_block(ctx, block, &err);
    free(block);
    if (err) return tool_fail(is_error, "%s", err);

    NoteArg args[MEM_MAX_NOTES];
    int got = collect_notes(ctx->db, arguments,
        "SELECT json_extract(value,'$.id'), json_extract(value,'$.text')"
        " FROM json_each(?1,'$.notes')", args, MEM_MAX_NOTES);

    int ok = 0, bad = 0, full = 0;
    char missing[256] = "", toobig[256] = "";
    size_t mpos = 0, tpos = 0;
    for (int i = 0; i < got; i++) {
        int rc = args[i].text
                     ? memory_note_set_guarded(ctx->db, mb->id, args[i].id, args[i].text) : -1;
        if (rc == 0) ok++;
        else if (rc == -2) { full++; list_append(toobig, sizeof(toobig), &tpos, (long long)args[i].id); }
        else { bad++; list_append(missing, sizeof(missing), &mpos, (long long)args[i].id); }
        free(args[i].text);
    }

    char summary[1024];
    int w = snprintf(summary, sizeof(summary), "edited %d note%s", ok, ok == 1 ? "" : "s");
    if (bad > 0 && (size_t)w < sizeof(summary))
        w += snprintf(summary + w, sizeof(summary) - (size_t)w,
                      "; no note %s in this block", missing);
    if (full > 0 && (size_t)w < sizeof(summary))
        snprintf(summary + w, sizeof(summary) - (size_t)w,
                 "; %s not changed — " FULL_MSG, toobig);
    char *out = with_summary(summary, render_block(ctx->db, mb));
    memory_block_free(mb);
    /* Nothing matched: that is a failed call, not a quiet success. */
    if (ok == 0) *is_error = 1;
    return out;
}

static char *tool_note_delete_handler(const char *arguments, void *user_data, int *is_error) {
    ToolMemoryCtx *ctx = (ToolMemoryCtx *)user_data;
    if (!ctx || !ctx->db || !ctx->agent_name) return tool_fail(is_error, NO_CTX);

    char *block = arg_str(ctx->db, arguments, "$.block");
    if (!block) return tool_fail(is_error, "error: missing or invalid 'block'");
    int n = arg_array_len(ctx->db, arguments, "$.ids");
    const char *err = NULL;
    MemoryBlock *mb = NULL;
    if (n < 0) err = "error: missing or invalid 'ids'";
    else if (n > MEM_MAX_NOTES) err = "error: too many ids in one call";
    else mb = writable_block(ctx, block, &err);
    free(block);
    if (err) return tool_fail(is_error, "%s", err);

    NoteArg args[MEM_MAX_NOTES];
    int got = collect_notes(ctx->db, arguments,
        "SELECT value FROM json_each(?1,'$.ids')", args, MEM_MAX_NOTES);

    int ok = 0, bad = 0;
    char missing[256] = "";
    size_t mpos = 0;
    for (int i = 0; i < got; i++) {
        if (memory_note_delete(ctx->db, mb->id, args[i].id) == 0) ok++;
        else { bad++; list_append(missing, sizeof(missing), &mpos, (long long)args[i].id); }
    }

    char summary[384];
    if (bad > 0)
        snprintf(summary, sizeof(summary), "deleted %d note%s; no note %s in this block",
                 ok, ok == 1 ? "" : "s", missing);
    else
        snprintf(summary, sizeof(summary), "deleted %d note%s", ok, ok == 1 ? "" : "s");
    char *out = with_summary(summary, render_block(ctx->db, mb));
    memory_block_free(mb);
    if (ok == 0) *is_error = 1;
    return out;
}

/* --- EXEC_THREAD shims: rebuild the ctx around the thread's own db handle --- */
#define THREAD_SHIM(fn, handler)                                              \
    static char *fn(sqlite3 *db, const char *agent_name, int64_t session_id,  \
                    const char *args, int *is_error) {                        \
        (void)session_id;                                                     \
        ToolMemoryCtx c = {.db = db, .agent_name = agent_name};               \
        return handler(args, &c, is_error);                                   \
    }
THREAD_SHIM(block_create_thread_run, tool_block_create_handler)
THREAD_SHIM(block_update_thread_run, tool_block_update_handler)
THREAD_SHIM(note_add_thread_run,     tool_note_add_handler)
THREAD_SHIM(note_edit_thread_run,    tool_note_edit_handler)
THREAD_SHIM(note_delete_thread_run,  tool_note_delete_handler)

/* --- Registration --- */

int tool_memory_register(ToolRegistry *reg, ToolMemoryCtx *ctx) {
    /* Descriptions: keep in sync with templates/seed.sql (write-once seed). */
    static const struct {
        const char *name, *desc; const char **params; ToolHandlerFn handler;
        char *(*run)(sqlite3 *, const char *, int64_t, const char *, int *);
    } T[] = {
        { "memory_block_create",
          "Create a memory block: a named container that holds notes. Args: label, description, placement.",
          &BLOCK_CREATE_PARAMS, tool_block_create_handler, block_create_thread_run },
        { "memory_block_update",
          "Change a memory block's description or placement. Args: block (label), description, placement.",
          &BLOCK_UPDATE_PARAMS, tool_block_update_handler, block_update_thread_run },
        { "memory_note_add",
          "Add a note to a memory block: a fact that changes what you do next time, not a record of what happened. Args: block (label), text. Returns the note's id.",
          &NOTE_ADD_PARAMS, tool_note_add_handler, note_add_thread_run },
        { "memory_note_edit",
          "Replace the text of notes, by note id — correct a note that has gone out of date rather than adding a new one. Args: block (label), notes (array of {id, text}).",
          &NOTE_EDIT_PARAMS, tool_note_edit_handler, note_edit_thread_run },
        { "memory_note_delete",
          "Delete notes, by note id. Other notes keep their ids. Args: block (label), ids (array of integers).",
          &NOTE_DELETE_PARAMS, tool_note_delete_handler, note_delete_thread_run },
    };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        if (tools_register(reg, T[i].name, T[i].desc, *T[i].params, T[i].handler, ctx) != 0)
            return -1;
        /* All DB-only — fire-and-forget threads with their own db handle. */
        tools_set_recipe(reg, T[i].name,
                         (ToolRecipe){.vehicle = EXEC_THREAD, .thread_run = T[i].run});
    }
    return 0;
}
