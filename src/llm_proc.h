#ifndef CCLAW_LLM_PROC_H
#define CCLAW_LLM_PROC_H

/* The LLM request pipeline: builds the ranked model-candidate list from
 * models JOIN providers, then runs a request with retry, timeout, and
 * fallback across candidates. The transcribe path shares its candidate types.
 */

#include "sqlite3.h"
#include "types.h"
#include <curl/curl.h>
#include <stdint.h>

/* One routable model (models JOIN providers row), as loaded by the candidate
 * builders. Shared with the transcribe path and tests. */
typedef struct {
    char id[128];
    char model[128];
    char base_url[256];
    char api_key_env[64];
    int endpoint_type;
    int context_window;
    /* Set only on the synthetic candidate llm_req() builds when the models
     * table yields nothing — that one carries no api_key_env because its key
     * is already resolved in cfg->provider. A real DB row must NEVER borrow
     * cfg's key: config_load's availability scan may have put a different
     * provider there, and sending its key to this row's base_url leaks the
     * credential cross-provider. */
    int use_cfg_key;
} ModelCandidate;

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

/* Consecutive compaction failures before the operator is told (C4). */
#define COMPACTION_FAIL_NOTIFY 3

/* Record one compaction outcome on sessions.compaction_fail_count: reset on
 * success, bump on failure, and on the transition to COMPACTION_FAIL_NOTIFY
 * send ONE operator notice to the session's channel (never an entry — the
 * agent is not told). Called by llm_compaction; exposed for tests. */
void compaction_record_outcome(sqlite3 *db, const char *db_path,
                               int64_t session_id, int ok, int budget_tokens);

/* Healthy, non-degraded models whose capabilities JSON array contains cap
 * (e.g. "audio"), ordered by priority. Fills out (max slots), returns count. */
int model_pick_by_capability(sqlite3 *db, const char *cap, ModelCandidate *out, int max);

/* Build the transcription request body for a media_jobs row (audio inlined
 * from payload.media), shaped per endpoint type. Malloc'd; NULL on error.
 * Exposed for tests. */
char *transcribe_build_body(sqlite3 *db, EndpointType ep, const char *model,
                            int64_t job_id);

/* Run one media_jobs transcription: capability-pick an audio model, POST,
 * write the transcript as a text-only inbox row for the job's session, delete
 * the job. Terminal failure still writes an inbox row (so the agent can tell
 * the user) and deletes the job. Returns 0 if the job was resolved either
 * way, -1 only if the job row is gone/unreadable. */
int llm_transcribe(sqlite3 *db, CURL *curl, int64_t job_id);

#endif
