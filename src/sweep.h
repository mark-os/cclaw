#ifndef CCLAW_SWEEP_H
#define CCLAW_SWEEP_H

/*
 * The periodic tick: everything the process does on a timer rather than on an
 * event. Edge wakes (wake pipe, worker completion, SIGCHLD) drive the normal
 * path; this module is the backstop that converges whatever an edge missed —
 * a peer's inbox row on a now-idle session, an approval nobody answered, a
 * delivery whose cursor lags its leaf — plus the plain housekeeping (process
 * heartbeat/GC, stale-session recovery, inbox/outbox pruning, WAL checkpoint,
 * disk-floor watch, cron).
 *
 * Everything here is owner-scoped and idempotent by construction: CLI and
 * daemon peers tick the same shared DB concurrently, so a sweep must never
 * assume it is the only one running. Latency is a tick (POLL_DB_INTERVAL), not
 * a guarantee — nothing may depend on a sweep for correctness of a live turn.
 *
 * Lib-resident: no archive module calls in here today, but the sweeps call
 * out to loop.c/child.c/advance.c, and the reorg keeps every non-main.c
 * module in libcclaw.a (the Makefile filters only main.o out).
 */

/* Recurring DB housekeeping. Called from both poll loops when the
 * POLL_DB_INTERVAL timer is due. */
void db_periodic(void);

/* Deny timed-out pending approvals. Part of the db_periodic tick; also called
 * directly at CLI shutdown, where a short -p run can exit before the first
 * tick. */
void approval_sweep_expired(void);

#endif
