#ifndef CCLAW_LLM_PROC_H
#define CCLAW_LLM_PROC_H

#include "sqlite3.h"
#include <curl/curl.h>
#include <stdint.h>

/* Core LLM request: one HTTP call, writes entry to DB.
 * Accepts persistent handles (worker mode) or NULL (creates/destroys).
 * recall: 1 = first req in turn (triggers auto-recall AND materializes the
 * session context block for the turn), 0 = subsequent tool-loop iterations
 * (reuse the materialized block verbatim — prompt-cache stability).
 * Returns 0 on success (entry written), -1 on error. */
int llm_req(sqlite3 *db, CURL *curl, int64_t session_id, int recall);

/* Compact session branch: estimate tokens, decide cut point, call LLM for summary.
 * Returns 0 on success (compaction entry inserted), -1 on error (no compaction).
 * On LLM failure, returns -1 without compacting (safe fallback). */
int llm_compaction(sqlite3 *db, CURL *curl, int64_t session_id, const char *agent_name);

#endif
