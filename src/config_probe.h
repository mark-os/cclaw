#ifndef CCLAW_CONFIG_PROBE_H
#define CCLAW_CONFIG_PROBE_H

#include "sqlite3.h"
#include <stdint.h>

/* Probe-at-apply for request_changes documents that move routing (config-ax
 * Phase 2A). "Applied" is not "working": the 2026-08-10 incident wrote a
 * perfectly valid provider row that silently knocked every request onto a
 * different model for 2.5h. So a document that changes WHICH model serves
 * requests is verified against the real request path before it is allowed to
 * stand, and reverted to the rows it replaced when it isn't.
 *
 * Snapshot the affected rows BEFORE the write; after the write commits, probe;
 * on failure restore the snapshot. The probe runs outside the write
 * transaction on purpose — a 15s HTTP call must not hold the WAL write lock. */

/* Rows this document is about to move (agent primary/secondary, the named
 * provider, the named models), as JSON. NULL on error. Caller frees. */
char *config_probe_snapshot(sqlite3 *db, const char *agent, const char *args_json);

/* 1 if the applied document changes which model would serve this agent's next
 * request; 0 for grants/config-value-only documents (nothing to probe). Call
 * AFTER the write — it reads the new state. */
int config_probe_needed(sqlite3 *db, const char *agent, const char *args_json);

/* Put back what the snapshot holds: rows that existed are restored field for
 * field, rows the document created are deleted. Returns 0 on success. */
int config_probe_restore(sqlite3 *db, const char *agent, const char *args_json,
                         const char *snap_json);

/* The whole apply-time verification: needed? → probe → commit or revert.
 * Returns 0 if the change stands (verdict = "probed OK ..." when it was
 * probed, NULL when it didn't need one), -1 if it was reverted (verdict =
 * "probe failed: ... — reverted to ..."). *verdict is malloc'd; caller frees.
 * session_id 0 means no probe at all — the operator paths (dashboard/CLI
 * grant-from-history) have no session to bill the request to and no tool
 * result to read it; A8's degrade-on-drop covers them instead. */
int config_probe_verify(sqlite3 *db, const char *agent, const char *args_json,
                        const char *snap_json, int64_t session_id, char **verdict);

#endif
