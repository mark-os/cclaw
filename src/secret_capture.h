#ifndef CCLAW_SECRET_CAPTURE_H
#define CCLAW_SECRET_CAPTURE_H

#include "sqlite3.h"

/* Explicit, agent-named secret capture (specs/security.md).
 *
 * A tool call that expects a credential in its result names it up front:
 *   save_secret       — secret name (^[A-Z][A-Z0-9_]*$), required to capture
 *   save_secret_path  — optional JSON path ("$.token") into the raw result;
 *                       omitted = whole result, whitespace-trimmed
 *
 * secret_capture_apply parses tool_args for those keys and, when present,
 * extracts the value from the RAW result (before any masking), stores it
 * encrypted (source='captured', zero host bindings — first use parks), and
 * returns a replacement result with every occurrence of the value masked to
 * {{SECRET:name}} plus a confirmation note. On a capture error the original
 * result is returned with an error note appended (nothing stored).
 *
 * `is_error` is the call's explicit failure status (see tools.h): a failed
 * call's result carries no credential, so capture is skipped and the result
 * comes back with a note. The status is passed in, never guessed from the
 * result text.
 *
 * Returns NULL when args carry no save_secret — caller keeps its result. */
char *secret_capture_apply(sqlite3 *db, const char *tool_args,
                           const char *raw_result, int is_error);

#endif /* CCLAW_SECRET_CAPTURE_H */
