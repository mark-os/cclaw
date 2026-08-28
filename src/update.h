#ifndef CCLAW_UPDATE_H
#define CCLAW_UPDATE_H

/* `cclaw update` — replace this binary with a newer tagged release.
 *
 * The whole reason this is a verb and not a shell script is the compatibility
 * handshake: schema patches are forward-only, so once a new build opens the
 * database and migrates it, swapping the old binary back is *not* recovery —
 * the old build refuses a database stamped newer than itself. So the candidate
 * is interrogated with `--schema-range` before it is installed, and an update
 * that would strand this database is declined rather than attempted. The
 * database snapshot below stays as a backstop for the other failure (a binary
 * that migrates fine and then dies for an unrelated reason), but it is no
 * longer the primary safety mechanism.
 *
 * Restart rides the supervisor rather than fighting it: a process cannot
 * cleanly re-exec itself mid-turn, so the file is swapped and the daemon is
 * asked to exit — whatever started it (the init respawn loop) brings up the
 * new one.
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "sqlite3.h"

int update_main(int argc, char *argv[]);

/* The compatibility decision, split out from the fork/exec around it so it can
 * be tested directly. `range` is a candidate's `--schema-range` line
 * ("min=40 current=51"); `db_version` is the live database's user_version.
 * Returns 1 if that candidate can safely take over this database, else 0 with
 * a human-readable reason in `why`. A NULL/garbage range is a refusal: a
 * binary that cannot state its range is one we cannot reason about. */
int update_schema_ok(const char *range, int db_version, char *why, size_t cap);

/* Periodic "is there a newer release?" check, called from the daemon's poll.
 * Off unless update.check_interval_hours is set. When a new tag appears it
 * queues a note in the default agent's inbox rather than doing anything about
 * it: the agent tells the operator, and installing stays an operator act.
 *
 * Delivery is the inbox specifically, not an entry write — the mid-turn
 * invariant says only LLM output and this turn's tool results reach `entries`
 * mid-turn, and everything else lands at a turn boundary as a user entry.
 * Cheap to call often; it tracks its own due time and no-ops until then. */
void update_check_tick(sqlite3 *db);

/* Wait for a daemon that is not the one identified by (old_pid,
 * old_started_at) to register itself in `processes`. Returns 0 once one
 * appears, -1 on timeout. Exposed for the lifecycle test: this is the exact
 * function `cclaw update` decides a restart by, and two of its bugs were only
 * observable against a real daemon — a connection opened before the restart
 * can sit on a WAL snapshot that hides the new row forever. Identity is
 * instance_id, a fresh token per registration — pid is not identity, since a
 * re-exec keeps it. Takes a db *path*, not a handle, precisely because it
 * must reopen. */
int update_await_restart(const char *db_path, const char *old_instance_id,
                         int timeout_s);

/* Post-update crash-loop guard — the failure update_await_restart cannot see
 * (a build that starts, then keeps dying after the updater exited). Same
 * numbers as channel flap detection. */
#define UPDATE_VERIFY_WINDOW     300  /* seconds */
#define UPDATE_VERIFY_MAX_STARTS 3    /* starts inside the window = crash loop */

/* Arm the marker (config `update.verify`) — called by update_install after
 * the binary swap. Exposed for tests. */
void update_verify_arm(sqlite3 *db, const char *tag);

/* Count this daemon start against an armed marker. Returns 1 when the start
 * limit was hit and the previous binary was restored (schema-checked; the
 * revert stays passive when the old build could not open the migrated DB) —
 * the caller must then re-exec so the restored build actually runs. 0
 * otherwise. Daemon-only. */
int update_verify_startup(sqlite3 *db);

/* Clear the marker after a healthy UPDATE_VERIFY_WINDOW of uptime. Cheap;
 * called from the daemon's periodic tick. */
void update_verify_tick(sqlite3 *db);

#endif
