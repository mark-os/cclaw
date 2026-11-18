#ifndef CCLAW_HOOK_H
#define CCLAW_HOOK_H

/* Script hooks, the database side: which events exist, which scripts an
 * agent runs for one, whether a finished turn gets its Stop hooks, and the
 * count of runs that failed. Nothing here forks or touches session state —
 * settle.c does that. See specs/hooks.md.
 */

#include "sqlite3.h"
#include "turn_edit.h"
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

/* The events this build fires. Names are the mainstream (Claude Code) ones
 * where the moment is the same; the context events are cclaw's own. */
#define HOOK_EVENT_STOP              "Stop"             /* a top-level turn ended with an answer */
#define HOOK_EVENT_SUBAGENT_STOP     "SubagentStop"     /* the same, in a sub-agent session */
#define HOOK_EVENT_CONTEXT_THRESHOLD "ContextThreshold" /* an iteration ended over trim_threshold */

/* Is `event` a name a manifest may register? NULL when it is; otherwise
 * the reason it is refused, for the promote error. A name this build knows
 * of but does not fire yet is refused by name, not as unknown. */
const char *hook_event_refusal(const char *event);

/* Consecutive failed runs of one hook (crash, timeout, refused reply) that
 * disable it. A policy number, the same on every machine. */
#define HOOK_FAIL_LIMIT 3

/* The most a manifest's "timeout" may say, in seconds. A hook holds its
 * session for as long as it runs, so this bounds how long a message can
 * wait behind one. Generous enough for a slow box and a slow provider. */
#define HOOK_TIMEOUT_MAX 3600

typedef struct {
    char extension[128];
    char path[PATH_MAX];    /* handler file, absolute, in the extension store */
    char event[32];         /* hooks.event */
    int pos;                /* hooks.pos: its place in the manifest */
    int wait;               /* 1 = the session waits (hooks.async = 0) */
    int timeout;            /* hooks.timeout in seconds; 0 = the configured default */
    TurnEditInputSpec input;/* hooks.input with the defaults filled in */
} HookRef;

/* The agent's next enabled hook for `event`, in (extension, manifest
 * position, path) order, after `after` (NULL = the first). An agent runs a
 * hook when it has the extension attached and enabled and the extension is
 * published or its own — the rule its tools load by (tools.c) — and the hook
 * is neither switched off nor disabled by its failures. `wait` 1 selects
 * the awaited hooks (hooks.async = 0), 0 the async ones; -1 takes either.
 * `input` is what the hook asked to be sent (hooks.input), each option it
 * did not name at its default. Read from
 * the tables on every call: nothing is cached, so an extension attached a
 * moment ago is found. Returns 1 with *out filled, 0 when there is none, -1
 * on a database error. */
int hook_next(sqlite3 *db, const char *agent, const char *event, int wait,
              const HookRef *after, HookRef *out);

/* Has the agent any hook for `event` it would run, awaited or not? */
int hook_any(sqlite3 *db, const char *agent, const char *event);

/* The stop event a session's turns fire: SubagentStop for a sub-agent's
 * session, Stop otherwise. */
const char *hook_stop_event(sqlite3 *db, int64_t session_id);

/* Does the turn that just ended on this session get its stop hooks
 * (hook_stop_event)? Both of: its agent has such a hook; the turn ended with
 * an assistant message that is not an error stop. Nothing about the turn's
 * size: whether a turn is worth its attention is the script's call (it
 * returns {} when it is not). Returns 1 or 0; `why` (optional) gets a short
 * reason for a 0. */
int hook_stop_due(sqlite3 *db, int64_t session_id, const char *agent,
                  char *why, size_t why_len);

/* Count a finished run: a failure adds one to hooks.fail_count, anything
 * else zeroes it. The HOOK_FAIL_LIMIT-th failure in a row disables the hook
 * (hooks.disabled = 1) until the extension is promoted again or an operator
 * clears the flag. Returns 1 when this call is the one that disabled it —
 * the caller tells the operator, once — and 0 otherwise. */
int hook_record_outcome(sqlite3 *db, const HookRef *h, int failed);

#endif
