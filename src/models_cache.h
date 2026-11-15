#ifndef CCLAW_MODELS_CACHE_H
#define CCLAW_MODELS_CACHE_H

#include "sqlite3.h"
#include <stddef.h>

/* Catalog probe (config-ax Phase 3): what a provider's /models endpoint LISTS,
 * as opposed to what is registered in `models` for routing. A listing is not a
 * promise — the id may still be unroutable for this key. GET <base_url>/models,
 * kept in the models_cache table on a slow TTL.
 *
 * Size is the whole design constraint — an aggregator's catalog is hundreds of
 * models and ~1MB of JSON. So: a hard 4MB body cap, a slim column set (never
 * the description/provider blobs), a query-filtered agent surface, and a row
 * cap with an explicit "(N more)" marker. Never probed at startup, never per
 * turn — only from search_config's models topic and `cclaw models`. */

#define MODELS_CACHE_TTL_S      43200   /* 12h — catalogs move slowly */
#define MODELS_CACHE_LIST_MAX   50      /* above this a search term is required */

/* A catalog fetch waits at least this long; raising llm_probe_timeout (the
 * knob for every "is this provider answering" call) raises it too, for slow
 * boxes and links. */
#define MODELS_CACHE_FETCH_MIN_S 30

/* Seconds a catalog fetch may take: max(MODELS_CACHE_FETCH_MIN_S,
 * llm_probe_timeout). */
int models_cache_timeout(sqlite3 *db);

/* The key a provider's api_key_env names, resolved like the request path:
 * environment variable, else the system secret. Heap or NULL; the caller
 * zeroes and frees it. */
char *models_cache_key(sqlite3 *db, const char *api_key_env);

/* GET <base_url>/models with the key, authenticated the way the wire format
 * (endpoint_type: openai | gemini | anthropic) authenticates requests. Touches
 * no database, so a caller can make it without holding a lock or an open
 * transaction. 0 with *body_out (heap) on a 2xx; -1 with a sentence in err. */
int models_cache_fetch(const char *base_url, const char *endpoint_type,
                       const char *key, int timeout_s, char **body_out,
                       char *err, size_t errlen);

/* Probe one provider and replace its cached rows. 0 on success; -1 with a
 * human-readable reason in err (nothing written on failure). */
int models_cache_refresh(sqlite3 *db, const char *provider_name,
                         char *err, size_t errlen);

/* Ingest an already-fetched catalog body ({"data":[...]}, or Gemini's
 * {"models":[...]}) for one provider. The write half of refresh, separated so
 * it can be exercised without a network. Replaces that provider's rows in one
 * transaction (a savepoint when the caller already holds one). */
int models_cache_ingest(sqlite3 *db, const char *provider_name,
                        const char *body, char *err, size_t errlen);

/* Render matching models as text. provider NULL = every configured provider,
 * query NULL/"" = no substring filter, page is 1-based (plain OFFSET — a
 * refresh between pages can shift rows, which is fine for a lookup aid).
 * Refreshes any in-scope provider whose cache is empty or older than the TTL
 * (force=1 refreshes regardless).
 * *out is malloc'd on success (caller frees); -1 with err set on failure —
 * including the "catalog too big, give me a search term" refusal. */
int models_cache_query(sqlite3 *db, const char *provider, const char *query,
                       int limit, int page, int force, char **out,
                       char *err, size_t errlen);

#endif
