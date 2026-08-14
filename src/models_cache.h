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

/* Probe one provider and replace its cached rows. 0 on success; -1 with a
 * human-readable reason in err (nothing written on failure). */
int models_cache_refresh(sqlite3 *db, const char *provider_name,
                         char *err, size_t errlen);

/* Ingest an already-fetched catalog body ({"data":[...]}) for one provider.
 * The write half of refresh, separated so it can be exercised without a
 * network. Replaces that provider's rows in one transaction. */
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
