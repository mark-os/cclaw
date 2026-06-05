#ifndef CCLAW_REQUEST_STREAM_H
#define CCLAW_REQUEST_STREAM_H

#include "context.h"
#include "llm.h"
#include "types.h"
#include "sqlite3.h"
#include <stddef.h>

/* V41: Streaming request builder phases */
typedef enum {
    RS_PHASE_PREAMBLE,   /* {"model":"...","messages":[ */
    RS_PHASE_ENTRIES,    /* cursor step + reshape per-entry JSON */
    RS_PHASE_TOOLS,      /* ],"tools":[...] */
    RS_PHASE_CLOSE,      /* } */
    RS_PHASE_DONE
} RsPhase;

/* V41: RequestStreamer state machine.
 * Streams LLM request JSON from SQLite cursor via CURLOPT_READFUNCTION.
 * Memory footprint: internal buffer + cursor (not session-proportional). */
typedef struct {
    /* Config */
    sqlite3 *db;
    int64_t session_id;
    const Config *cfg;
    const ToolSchema *tools;
    size_t tool_count;
    const ContextPlan *plan;

    /* State */
    RsPhase phase;
    sqlite3_stmt *cursor;       /* entry data cursor */
    int entry_idx;              /* index into plan->entries (from cut) */
    int first_entry;            /* flag: suppress leading comma */

    /* Internal buffer (partially consumed output) */
    char *buf;
    size_t buf_len;
    size_t buf_pos;
    size_t buf_cap;

    /* T289: index into plan->entries of last system msg (for per-block cache_control) */
    int last_sys_idx;           /* -1 = none */

    /* T269: auto-recall text (system message injected after preamble) */
    const char *recall_text;    /* NULL = no recall; caller-owned, must outlive streamer */
} RequestStreamer;

/* Initialize streamer. Does NOT prepare cursor yet (deferred to first read).
 * Returns 0 on success, -1 on error. */
int rs_init(RequestStreamer *rs, sqlite3 *db, int64_t session_id,
            const Config *cfg, const ContextPlan *plan,
            const ToolSchema *tools, size_t tool_count);

/* CURLOPT_READFUNCTION-compatible callback.
 * userdata must be a RequestStreamer*. */
size_t rs_read_cb(char *dest, size_t size, size_t nmemb, void *userdata);

/* Clean up streamer (finalize cursor, free buffer). */
void rs_cleanup(RequestStreamer *rs);

/* Reset streamer for retry (re-read from beginning). */
void rs_reset(RequestStreamer *rs);

/* Get total content length hint (0 = unknown/chunked). */
size_t rs_content_length(RequestStreamer *rs);

#endif
