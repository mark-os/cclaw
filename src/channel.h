#ifndef CCLAW_CHANNEL_H
#define CCLAW_CHANNEL_H

/* Channel supervisor: spawns, monitors, and restarts the per-channel JS
 * runner processes, with flap detection. Owns the channels table and the
 * daemon side of the channel lifecycle.
 */

#include <sqlite3.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#define CHANNEL_MAX 16
#define CHANNEL_MAX_RESTARTS 3

/* Cap on a parsed admin_ids list — an operator-typed config value, not
 * user-controlled input; sized generously above any real admin roster. */
#define CHANNEL_ADMIN_IDS_MAX 32
#define CHANNEL_FLAP_WINDOW 300   /* 3 crashes in 5 min = flapping */
#define CHANNEL_MAX_BACKOFF 60

/* Deaf-channel watchdog (specs/channels.md): a runner that is alive but whose
 * transport has delivered nothing for this many seconds gets bounced.
 * Generous by design — slow transports and long-poll cycles must clear it.
 * Resolution: <ext>.deaf_timeout, then the global channel_deaf_timeout
 * (env CCLAW_CHANNEL_DEAF_TIMEOUT), then this. Either set to 0 = off. */
#define CHANNEL_DEAF_TIMEOUT 300

typedef struct {
    pid_t pid;
    char name[64];
    int restart_count;
    time_t first_crash;       /* timestamp of first crash in current window */
    time_t next_restart_at;   /* 0 = not pending restart */
    time_t started_at;        /* when process was last launched */
} ChannelProc;

/* Lifecycle */
int channel_launch_all(sqlite3 *db);
void channel_shutdown_all(void);

/* draft|broken → validated (on a passing --check). Returns 0 on success,
 * -1 if the channel doesn't exist or isn't in an allowed source state. */
int channel_mark_validated(sqlite3 *db, const char *name);

/* validated → active (on --activate). Returns 0 on success, -1 if the
 * channel doesn't exist or isn't 'validated'. */
int channel_activate(sqlite3 *db, const char *name);

/* Current lifecycle status ("draft"/"validated"/"active"/"broken"), or NULL
 * if the channel doesn't exist. Caller frees. */
char *channel_get_status(sqlite3 *db, const char *name);

/* Swap the extension behind a channel. Records the current extension as the
 * revert target (only on a real change), repoints, and bounces the process —
 * the daemon's supervision respawns it with the new pointer. Returns 0,
 * -1 unknown channel, -2 unknown extension. */
int channel_swap(sqlite3 *db, const char *name, const char *extension);

/* Swap back to the recorded previous extension (clears it) and bounce.
 * Returns 0, or -1 if there is nothing to revert to. */
int channel_revert(sqlite3 *db, const char *name);

/* SIGTERM the channel's process (pid from the channels table); supervision
 * respawns it, re-reading its extension pointer. 0 on success, -1 unknown. */
int channel_bounce(sqlite3 *db, const char *name);

/* prev_extension_name for a channel, or NULL. Caller frees. */
char *channel_prev_extension(sqlite3 *db, const char *name);

/* Queue text to every admin chat of a channel (registry key
 * <ext>.admin_ids, comma/space separated). */
void channel_notify_admins(sqlite3 *db, const char *channel_name, const char *text);

/* Is `id` in the channel's <ext>.admin_ids? admin_ids are user ids — pass the
 * envelope's sender_id, not the chat id (they differ on Discord). */
int channel_id_is_admin(sqlite3 *db, const char *channel_name, const char *id);

/* Queue a plain-text reply to one chat (command feedback — not agent output,
 * so it goes straight to the outbox like admin notices do). */
void channel_chat_reply(sqlite3 *db, const char *channel_name,
                        const char *chat_id, const char *text);

/* Re-point the chat's pin (upsert, preserving delivery_mode/tool_filter on
 * an existing row) and stamp the session's origin. The pin is the binding —
 * this is the one write that moves a chat between conversations. */
void channel_pin_session(sqlite3 *db, const char *channel_name,
                         const char *chat_id, int64_t sid);

/* Channel open-door policy: channels.default_agent (NULL = fail-closed).
 * *filter_out (optional) receives channels.default_tool_filter — the tool
 * scope for chats the gate accepts without a route. Caller frees both. */
char *channel_default_agent(sqlite3 *db, const char *channel_name,
                            char **filter_out);


/* Resolve a channel's registry config key <extension>.<key> through the
 * uniform resolution rule (specs/config.md). Heap or NULL; caller frees. */
char *channel_config_get(sqlite3 *db, const char *channel_name, const char *key);

/* Launch gate (specs/config.md): trust status 'active' AND <ext>.enabled
 * resolves truthy AND every required config key resolves non-empty.
 * Returns 1 to launch, 0 for a definite no, -1 if the gate could not be read
 * (a failed query — not a decision, so callers must not act on it as one).
 * Both non-1 results write the reason into why. */
int channel_should_launch(sqlite3 *db, const char *name, char *why, size_t cap);

/* Deaf-channel watchdog — transport liveness, kept in channel_state under
 * 'last_activity_at'. The runner and the supervisor are separate processes, so
 * the DB is the only place this can live.
 *
 * `seen` is the runner reporting *received* traffic and creates the mark;
 * `reset` only refreshes a mark that already exists. That asymmetry is the
 * enrolment rule: a channel with no self-driven transport (one the daemon
 * feeds over the request UDS — a webhook) never stamps, never gets a row, and
 * is never judged deaf. */
void channel_activity_seen(sqlite3 *db, const char *name);
void channel_activity_reset(sqlite3 *db, const char *name);

/* Seconds since the channel's last recorded activity; -1 if never recorded
 * (not enrolled — the watchdog has no opinion about it). */
time_t channel_activity_age(sqlite3 *db, const char *name);

/* Resolved deaf threshold in seconds for a channel; 0 = watchdog off. */
int channel_deaf_timeout(sqlite3 *db, const char *name);

/* Reap — returns 1 if pid belonged to a channel, 0 otherwise */
int channel_reap(pid_t pid, sqlite3 *db);

/* Tick — reconcile with the channels table (launch newly-active, stop
 * deactivated), restart crashed channels whose backoff expired. */
void channel_tick(sqlite3 *db);

/* Channel events — process pending channel_events rows */
void channel_consume_events(sqlite3 *db);

/* Earliest next_restart_at across pending channels (0 = none pending) */
time_t channel_next_deadline(void);

#endif
