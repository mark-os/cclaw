#ifndef CCLAW_LLM_PROC_H
#define CCLAW_LLM_PROC_H

#include "sqlite3.h"
#include <curl/curl.h>
#include <stdint.h>

/* Legacy exit codes — kept for fork mode (child exit status).
 * Parent should prefer turn_complete() for dispatch decisions. */
#define LLM_EXIT_STOP      0
#define LLM_EXIT_ERROR     1
#define LLM_EXIT_TOOLCALL 10

/* Core LLM request: one HTTP call, writes entry to DB.
 * Accepts persistent handles (worker mode) or NULL (fork mode creates/destroys).
 * Returns 0 on success (entry written), -1 on error. */
int llm_req(sqlite3 *db, CURL *curl, int64_t session_id);

/* Fork-mode entry point: opens DB, calls llm_req, closes, exits.
 * Reads config from CCLAW_* env vars. Returns LLM_EXIT_* code. */
int llm_proc_main(int64_t session_id);

/* Query DB state after LLM completion to decide next action.
 * Returns: 1 = has pending tool_calls, 0 = final stop, -1 = error */
int turn_complete(sqlite3 *db, int64_t session_id);

#endif
