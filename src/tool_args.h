#ifndef CCLAW_TOOL_ARGS_H
#define CCLAW_TOOL_ARGS_H

/*
 * Tool-call argument extraction via SQLite JSON1 — for code running in the
 * trusted parent with a live db handle (EXEC_INLINE / EXEC_THREAD handlers,
 * the dispatch gate, approval apply paths).
 *
 * This replaces the jsmn-based tool_parse.h: one parser (SQLite), no token
 * caps, fail-closed on malformed JSON (extractors return NULL/default).
 * Sandboxed --run-tool children have no db handle — they receive
 * pre-extracted values over the flat wire and never parse JSON.
 *
 * Ownership: string results are malloc'd; the caller frees.
 */

#include <stddef.h>

#include "sqlite3.h"

/* 1 iff args is a valid JSON object ("{...}"). The dispatch gate calls this
 * once per tool call; malformed args become an error tool-result, never an
 * empty param set. */
int tool_args_valid_object(sqlite3 *db, const char *args);

/* Top-level string field, or NULL if absent / wrong type / malformed JSON.
 * key is a C identifier-ish literal (no quotes or brackets). */
char *tool_args_str(sqlite3 *db, const char *args, const char *key);

/* Integer field with default (also accepts integral reals). */
int tool_args_int(sqlite3 *db, const char *args, const char *key, int def);

/* Boolean field with default. */
int tool_args_bool(sqlite3 *db, const char *args, const char *key, int def);

/* Object/array field re-serialized as minified JSON text, or NULL.
 * For variable-shape sub-blobs (e.g. file_edit's edits array). */
char *tool_args_json(sqlite3 *db, const char *args, const char *key);

/* Host of a call's "url" argument: scheme-strip + authority slice, mirroring
 * what the proxy/http layer accepts — deliberately not a full URL parser.
 * Writes at most cap bytes (NUL-terminated) to out and returns 1; returns 0
 * and leaves out untouched when there's no usable url. Shared by the dispatch
 * gate and the approval bind path. */
int web_args_url_host(sqlite3 *db, const char *args, char *out, size_t cap);

/* ── Wire extraction for sandboxed --run-tool children ──────────────────
 * The child has no db handle and parses no JSON: the parent extracts every
 * parameter the tool's schema declares and ships (key, kind, value) over
 * the flat wire. Scalars become text (bools as "1"/"0", numbers as their
 * decimal text); objects/arrays ship as opaque JSON text only where the
 * consumer parses JSON natively (js_eval's args → QuickJS); file_edit's
 * edits array is flattened to an oldText/newText string list so the file
 * tier never parses JSON at all. */

enum { TOOL_ARG_TEXT = 0, TOOL_ARG_JSON = 1, TOOL_ARG_LIST = 2 };

typedef struct {
    char *key;
    int kind;              /* TOOL_ARG_* */
    char *value;           /* TEXT / JSON */
    char **list;           /* LIST */
    size_t list_n;
} ToolWireArg;

/* Extract the params declared in schema_json ('$.properties' keys) that are
 * present in args (NULL schema_json = ship every top-level key — tests and
 * schemaless tools). tool_name selects the per-tool flattening shim (file_edit).
 * Returns 0 and a malloc'd array (may be empty) on success; -1 on malformed
 * args/schema — the caller must fail the call, never dispatch without params. */
int tool_args_extract(sqlite3 *db, const char *tool_name,
                      const char *schema_json, const char *args,
                      ToolWireArg **out, size_t *out_n);

void tool_wire_args_free(ToolWireArg *a, size_t n);

/* As tool_wire_args_free, but zeroes every value first — for params that
 * carried interpolated {{SECRET:name}} plaintext to the wire. */
void tool_wire_args_wipe_free(ToolWireArg *a, size_t n);

#endif
