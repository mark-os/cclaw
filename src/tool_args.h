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

#endif
