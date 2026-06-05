#ifndef CCLAW_GEMINI_CACHE_H
#define CCLAW_GEMINI_CACHE_H

#include "types.h"
#include "context.h"
#include "sqlite3.h"
#include <stdint.h>

/* T291: Gemini context caching.
 * Creates/reuses/deletes cached content for sessions > 1024 tokens.
 * Cache name stored in agent.db kv as gemini_cache_<session_id>.
 * TTL 300s. Reuse if not expired; recreate otherwise. */

/* Get or create a Gemini cached content for this session.
 * Returns heap-allocated cache name (caller frees) or NULL if caching
 * not applicable (< 1024 tokens, non-gemini, API error). */
char *gemini_cache_get_or_create(sqlite3 *db, int64_t session_id,
                                 const Config *cfg, const ContextPlan *plan);

/* Delete the Gemini cached content for a session (cleanup). */
void gemini_cache_delete(sqlite3 *db, int64_t session_id, const Config *cfg);

#endif
