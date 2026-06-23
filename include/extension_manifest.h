#ifndef CCLAW_EXTENSION_MANIFEST_H
#define CCLAW_EXTENSION_MANIFEST_H

#include <sqlite3.h>

/* Declarative extension manifest loader + shared-store installer.
 *
 * An extension bundle is a directory whose root holds extension.json (the
 * manifest) plus the handler .qjs files it declares. The manifest carries
 * identity + declarations only — never tool code. Parsing is done with SQLite
 * JSON1 (the manifest is structured data destined for DB rows), not hand-rolled
 * C. See specs/extensions.md. */

/* Validate <bundle_dir>/extension.json: well-formed JSON, a safe name, and
 * every declared handler file present and ending in .qjs. Returns 0 if valid,
 * -1 otherwise. On failure, if err_out is non-NULL, *err_out is set to a
 * malloc'd message the caller must free (NULL on success). */
int extension_manifest_validate(const char *bundle_dir, char **err_out);

/* Install a bundle into the shared store and ingest its declarations.
 *
 * Steps (daemon-side apply):
 *   1. validate the manifest,
 *   2. copy bundle_dir -> <store>/<name>/ (store = <db_dir>/extensions),
 *   3. ingest extensions / tools / hooks / agent_extensions rows in single SQL
 *      passes via json_each, seed channels + cron for declared scripts.
 *
 * owner_agent is recorded as the promoter and gets the first agent_extensions
 * row (published=0). Returns 0 on success, -1 on error (with *err_out set when
 * non-NULL). Re-installing the same name refreshes its rows in place
 * (preserving the published flag). */
int extension_install(sqlite3 *db, const char *bundle_dir,
                      const char *owner_agent, char **err_out);

#endif /* CCLAW_EXTENSION_MANIFEST_H */
