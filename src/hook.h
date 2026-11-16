#ifndef CCLAW_HOOK_H
#define CCLAW_HOOK_H

/* Script hooks, the database side: which scripts an agent runs for an event,
 * whether a finished turn is one they run for, and the count of runs that
 * failed. Nothing here forks or touches session state — settle.c does that.
 * See specs/hooks.md.
 */

#include "sqlite3.h"
#include "turn_edit.h"
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

/* The one event: a turn ended normally and its reply has been delivered. */
#define HOOK_EVENT_TURN_SETTLED "turnSettled"

/* Consecutive failed runs of one hook that send the operator a notice. A
 * policy number, the same on every machine. */
#define HOOK_FAIL_NOTIFY 3

typedef struct {
    char extension[128];
    char path[PATH_MAX];    /* handler file, absolute, in the extension store */
    int wait;               /* hooks.wait */
    TurnEditInputSpec input;/* hooks.input with the defaults filled in */
} HookRef;

/* The agent's next enabled hook for `event`, in (extension, path) order,
 * after `after` (NULL = the first). An agent runs a hook when it has the
 * extension attached and enabled and the extension is published or its own —
 * the rule its tools load by (tools.c). `wait` selects hooks.wait 0 or 1; -1
 * takes either. `input` is what the hook asked to be sent (hooks.input), each
 * option it did not name at its default. Read from the tables on every call: nothing is cached, so an
 * extension attached a moment ago is found. Returns 1 with *out filled, 0
 * when there is none, -1 on a database error. */
int hook_next(sqlite3 *db, const char *agent, const char *event, int wait,
              const HookRef *after, HookRef *out);

/* Does the turn that just ended on this session get its turnSettled hooks?
 * All of: its agent has such a hook; the session is top-level; the turn ended
 * with an assistant message that is not an error stop. Nothing about the
 * turn's size: whether a turn is worth its attention is the script's call
 * (it returns {} when it is not). Returns 1 or 0; `why` (optional) gets a
 * short reason for a 0. */
int hook_turn_settled_due(sqlite3 *db, int64_t session_id, const char *agent,
                          char *why, size_t why_len);

/* Count a finished run: a failure adds one to hooks.fail_count, anything
 * else zeroes it. Returns 1 when this failure is the HOOK_FAIL_NOTIFY-th in
 * a row — the caller tells the operator, once per streak — and 0 otherwise. */
int hook_record_outcome(sqlite3 *db, const HookRef *h, const char *event,
                        int failed);

#endif
