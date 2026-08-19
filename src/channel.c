#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "channel.h"
#include "channel_intent.h"
#include "approval.h"
#include "log.h"
#include "db.h"
#include "llm_worker.h"
#include "secret_scan.h"
#include "wake.h"
#include "resolve.h"
#include "config_registry.h"
#include "util.h"           /* split_and_trim */
#include <signal.h>
#include <sys/prctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

static ChannelProc g_channels[CHANNEL_MAX];
static int g_count;

static ChannelProc *find_by_pid(pid_t pid) {
    for (int i = 0; i < g_count; i++)
        if (g_channels[i].pid == pid) return &g_channels[i];
    return NULL;
}

static ChannelProc *find_by_name(const char *name) {
    for (int i = 0; i < g_count; i++)
        if (strcmp(g_channels[i].name, name) == 0) return &g_channels[i];
    return NULL;
}

static void remove_channel(ChannelProc *c) {
    *c = g_channels[--g_count];
}

static pid_t do_fork(const char *name) {
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* Die with the daemon. A SIGKILLed/OOM-killed daemon otherwise leaves
         * runners holding their gateway connections, and the restarted daemon
         * spawns duplicates (double message delivery). PDEATHSIG survives
         * execve — the kernel clears it only across setuid/setgid execs — so
         * setting it pre-exec is correct. Race: if the parent died between
         * fork and prctl, the signal already fired for nobody; re-check. */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent) _exit(0);
        /* Re-exec self for a clean process image; the --channel branch runs the
         * channel loop in-process (channel_runner_main), so ps shows
         * `cclaw --channel <name>`. */
        execl("/proc/self/exe", "cclaw", "--channel", name, (char *)NULL);
        _exit(127);
    }
    return pid;
}

static void update_pid(sqlite3 *db, const char *name, pid_t pid) {
    const char *sql = "UPDATE channels SET pid=? WHERE name=?;";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int(s, 1, (int)pid);
        sqlite3_bind_text(s, 2, name, -1, SQLITE_STATIC);
        sqlite3_step(s); sqlite3_finalize(s);
    }
}


int channel_mark_validated(sqlite3 *db, const char *name) {
    const char *sql = "UPDATE channels SET status='validated'"
                      " WHERE name=? AND status IN ('draft','broken');";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    sqlite3_step(s);
    int changed = sqlite3_changes(db);
    sqlite3_finalize(s);
    return changed > 0 ? 0 : -1;
}

int channel_activate(sqlite3 *db, const char *name) {
    const char *sql = "UPDATE channels SET status='active'"
                      " WHERE name=? AND status='validated';";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    sqlite3_step(s);
    int changed = sqlite3_changes(db);
    sqlite3_finalize(s);
    return changed > 0 ? 0 : -1;
}

char *channel_prev_extension(sqlite3 *db, const char *name) {
    const char *sql = "SELECT prev_extension_name FROM channels WHERE name=?;";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    char *prev = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(s, 0);
        if (v && v[0]) prev = strdup(v);
    }
    sqlite3_finalize(s);
    return prev;
}

/* The destination of an admin notice is a *user* id, not a chat: the outbox
 * payload says so with to_user=1 and the handler resolves it however its
 * platform requires (Telegram: identical, a DM's chat id is the user id;
 * Discord: open a DM channel first). C learns no platform specifics. */
/* Queue a notice to every admin chat of a channel (channel_state admin_ids,
 * comma/space separated). Rows sit in channel_outbox until a process for the
 * channel delivers them — after an auto-revert that's the respawned, reverted
 * process, so the notice arrives through the channel it is about. */
void channel_notify_admins(sqlite3 *db, const char *channel_name, const char *text) {
    char *admins = channel_config_get(db, channel_name, "admin_ids");
    if (!admins) return;

    char *ids[CHANNEL_ADMIN_IDS_MAX];
    int n = split_and_trim(admins, ids, CHANNEL_ADMIN_IDS_MAX);

    const char *isql =
        "INSERT INTO channel_outbox(channel_name, session_id, payload)"
        " VALUES(?1, 0, json_object('chat_id', ?2, 'text', ?3, 'to_user', 1));";
    for (int i = 0; i < n; i++) {
        sqlite3_stmt *ins;
        if (sqlite3_prepare_v2(db, isql, -1, &ins, NULL) == SQLITE_OK) {
            sqlite3_bind_text(ins, 1, channel_name, -1, SQLITE_STATIC);
            sqlite3_bind_text(ins, 2, ids[i], -1, SQLITE_STATIC);
            sqlite3_bind_text(ins, 3, text, -1, SQLITE_STATIC);
            sqlite3_step(ins);
            sqlite3_finalize(ins);
        }
    }
    free(admins);
}

/* Is this id one of the channel's admin_ids? admin_ids are *user* ids: on
 * Telegram a DM's chat id and its sender id coincide, but on Discord a DM
 * channel snowflake is not the user's — so every caller must pass the
 * envelope's sender_id (see admin_id_of()). */
int channel_id_is_admin(sqlite3 *db, const char *channel_name, const char *id) {
    if (!id || !id[0]) return 0;
    char *admins = channel_config_get(db, channel_name, "admin_ids");
    if (!admins) return 0;
    char *ids[CHANNEL_ADMIN_IDS_MAX];
    int n = split_and_trim(admins, ids, CHANNEL_ADMIN_IDS_MAX);
    int hit = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(ids[i], id) == 0) { hit = 1; break; }
    }
    free(admins);
    return hit;
}

/* Channel open-door policy: channels.default_agent (NULL = fail-closed).
 * *filter_out (optional) receives channels.default_tool_filter — the tool
 * scope for chats the gate accepts without a route. It is read independently
 * of default_agent: the admin fallback lands on the global default agent but
 * is still an unrouted chat, so it takes the same filter. */
char *channel_default_agent(sqlite3 *db, const char *channel_name,
                            char **filter_out) {
    sqlite3_stmt *s;
    char *agent = NULL;
    if (filter_out) *filter_out = NULL;
    if (sqlite3_prepare_v2(db, "SELECT default_agent, default_tool_filter"
                               " FROM channels WHERE name=?;",
                           -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, channel_name, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(s, 0);
            if (v && v[0]) agent = strdup(v);
            const char *f = (const char *)sqlite3_column_text(s, 1);
            if (filter_out && f && f[0]) *filter_out = strdup(f);
        }
        sqlite3_finalize(s);
    }
    return agent;
}

/* Queue a plain-text reply to one chat (command feedback — not agent output,
 * so it goes straight to the outbox like admin notices do). */
void channel_chat_reply(sqlite3 *db, const char *channel_name,
                        const char *chat_id, const char *text) {
    sqlite3_stmt *ins;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO channel_outbox(channel_name, session_id, payload)"
            " VALUES(?1, 0, json_object('chat_id', ?2, 'text', ?3));",
            -1, &ins, NULL) == SQLITE_OK) {
        sqlite3_bind_text(ins, 1, channel_name, -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 2, chat_id, -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 3, text, -1, SQLITE_STATIC);
        sqlite3_step(ins);
        sqlite3_finalize(ins);
    }
}

/* Re-point the chat's pin (upsert, preserving delivery_mode/tool_filter on
 * an existing row) and stamp the session's origin. The pin is the binding —
 * this is the one write that moves a chat between conversations. */
void channel_pin_session(sqlite3 *db, const char *channel_name,
                         const char *chat_id, int64_t sid) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO channel_routes(channel_name, chat_id, session_id)"
            " VALUES(?1,?2,?3)"
            " ON CONFLICT(channel_name, chat_id)"
            " DO UPDATE SET session_id=excluded.session_id;",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, channel_name, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, chat_id, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 3, sid);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(db,
            "UPDATE sessions SET channel_name=?1, chat_id=?2 WHERE id=?3;",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, channel_name, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, chat_id, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 3, sid);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
}


/* One-time admin notification for a dropped unrouted sender. The dedup set is
 * in-memory, per-process — ephemeral politeness state, not authority: a
 * re-notify after a daemon restart is acceptable and not worth a table. */
#define GATE_NOTIFIED_MAX 256
static char *g_gate_notified[GATE_NOTIFIED_MAX];
static int g_gate_notified_n;

static void gate_notify_unknown(sqlite3 *db, const char *ch_name,
                                const char *cid, const char *payload) {
    char key[192];
    snprintf(key, sizeof(key), "%s\x1f%s", ch_name, cid);
    for (int i = 0; i < g_gate_notified_n; i++)
        if (strcmp(g_gate_notified[i], key) == 0) return;
    if (g_gate_notified_n < GATE_NOTIFIED_MAX)
        g_gate_notified[g_gate_notified_n++] = strdup(key);

    /* Sender facts for the operator, rendered from the envelope in SQL. */
    char *note = NULL;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT 'unrouted message from '"
            " || COALESCE(NULLIF(json_extract(?1,'$.sender_name'),''),'(unknown)')"
            " || ' (sender id ' || COALESCE(json_extract(?1,'$.sender_id'),'?')"
            " || ', chat ' || ?2 || ') on channel ' || ?3"
            " || ' — dropped. To accept: cclaw route add ' || ?3 || ' ' || ?2"
            " || ' <Agent>'",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, payload, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, cid, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, ch_name, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(st, 0);
            if (v) note = strdup(v);
        }
        sqlite3_finalize(st);
    }
    if (note) {
        channel_notify_admins(db, ch_name, note);
        free(note);
    }
}

int channel_bounce(sqlite3 *db, const char *name) {
    const char *sql = "SELECT pid FROM channels WHERE name=?;";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    pid_t pid = 0;
    int found = 0;
    if (sqlite3_step(s) == SQLITE_ROW) {
        found = 1;
        pid = (pid_t)sqlite3_column_int(s, 0);
    }
    sqlite3_finalize(s);
    if (!found) return -1;
    /* SIGTERM only — the daemon's supervision reaps and respawns with a fresh
     * read of the channels row (new extension pointer included). A pid of 0
     * means not running; the reconcile in channel_tick launches it if active. */
    if (pid > 0) kill(pid, SIGTERM);
    return 0;
}

int channel_swap(sqlite3 *db, const char *name, const char *extension) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM extensions WHERE name=?;",
                           -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(s, 1, extension, -1, SQLITE_STATIC);
    int have_ext = sqlite3_step(s) == SQLITE_ROW;
    sqlite3_finalize(s);
    if (!have_ext) return -2;

    /* Record the current extension as the revert target — but only on a real
     * change, so re-swapping to the same extension (a restart) can't clobber
     * the rollback pointer with itself. */
    const char *sql =
        "UPDATE channels SET prev_extension_name=extension_name, extension_name=?2"
        " WHERE name=?1 AND extension_name != ?2;";
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, extension, -1, SQLITE_STATIC);
    sqlite3_step(s);
    sqlite3_finalize(s);

    return channel_bounce(db, name);
}

int channel_revert(sqlite3 *db, const char *name) {
    const char *sql =
        "UPDATE channels SET extension_name=prev_extension_name, prev_extension_name=NULL"
        " WHERE name=?1 AND prev_extension_name IS NOT NULL;";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    sqlite3_step(s);
    int changed = sqlite3_changes(db);
    sqlite3_finalize(s);
    if (changed == 0) return -1;   /* nothing to revert to */
    return channel_bounce(db, name);
}

char *channel_get_status(sqlite3 *db, const char *name) {
    const char *sql = "SELECT status FROM channels WHERE name=?;";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    char *status = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(s, 0);
        if (v) status = strdup(v);
    }
    sqlite3_finalize(s);
    return status;
}

/* Snapshot the active-channel names. Collect first, finalize, then write:
 * forking or writing with the SELECT still open pins a read snapshot, and a
 * later write would fail with an immediate SQLITE_BUSY if another process
 * committed meanwhile (snapshot upgrades skip the busy handler). */
static int active_channel_names(sqlite3 *db, char names[][64], int max) {
    const char *sql = "SELECT c.name FROM channels c"
                      " JOIN extensions e ON c.extension_name=e.name"
                      " WHERE c.status='active';";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n < max) {
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        if (!name) continue;
        snprintf(names[n], 64, "%s", name);
        n++;
    }
    sqlite3_finalize(stmt);
    return n;
}

/* ── Deaf-channel watchdog ──────────────────────────────────────────
 *
 * A runner can be alive as a process and still have stopped receiving: a
 * wedged WebSocket, a long-poll loop that never completes again. Nothing else
 * notices — supervision only ever hears about a channel that *exits*.
 *
 * The mark is transport-level, not message-level. The runner stamps it on any
 * received frame (gateway heartbeat ACKs included — Discord's arrive every
 * ~41s) and on every successful poll cycle, so a quiet-but-healthy channel
 * keeps stamping while a wedged one stops within a minute or two. Keying off
 * user-visible messages instead would condemn every idle chat.
 *
 * Enrolment is by first traffic (see channel.h): only `seen` creates the row.
 */
#define CHANNEL_ACTIVITY_KEY "last_activity_at"

void channel_activity_seen(sqlite3 *db, const char *name) {
    if (!db || !name) return;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO channel_state(channel_name, key, value)"
            " VALUES(?1, '" CHANNEL_ACTIVITY_KEY "', unixepoch())"
            " ON CONFLICT(channel_name, key) DO UPDATE SET value=unixepoch();",
            -1, &s, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

void channel_activity_reset(sqlite3 *db, const char *name) {
    if (!db || !name) return;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "UPDATE channel_state SET value=unixepoch()"
            " WHERE channel_name=?1 AND key='" CHANNEL_ACTIVITY_KEY "';",
            -1, &s, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

/* MAX(0, …): a backward clock step must read as "just now", never as a huge
 * silence that bounces every enrolled channel at once. */
time_t channel_activity_age(sqlite3 *db, const char *name) {
    if (!db || !name) return -1;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT MAX(0, unixepoch() - CAST(value AS INTEGER))"
            " FROM channel_state"
            " WHERE channel_name=?1 AND key='" CHANNEL_ACTIVITY_KEY "';",
            -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    time_t age = -1;
    if (sqlite3_step(s) == SQLITE_ROW && sqlite3_column_type(s, 0) != SQLITE_NULL)
        age = (time_t)sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return age;
}

/* Ceiling keeps the narrowing to int well-defined on 32-bit targets, and a
 * transport watchdog set past a day is indistinguishable from "off" anyway. */
#define CHANNEL_DEAF_MAX 86400

/* Consumes v. Unset or unparsable is not an answer — fall through to the next
 * tier; a parsed value <= 0 means "watchdog off for this channel". */
static int deaf_seconds(char *v, int *out) {
    if (!v) return 0;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    int ok = v[0] != '\0' && end && *end == '\0';
    if (ok) *out = n <= 0 ? 0 : (n > CHANNEL_DEAF_MAX ? CHANNEL_DEAF_MAX : (int)n);
    free(v);
    return ok;
}

int channel_deaf_timeout(sqlite3 *db, const char *name) {
    int secs = CHANNEL_DEAF_TIMEOUT;
    if (deaf_seconds(channel_config_get(db, name, "deaf_timeout"), &secs))
        return secs;
    if (deaf_seconds(config_get(db, "channel_deaf_timeout"), &secs))
        return secs;
    return CHANNEL_DEAF_TIMEOUT;
}

/* Fork one channel process and start tracking it. */
static int start_channel(sqlite3 *db, const char *name) {
    if (g_count >= CHANNEL_MAX) return -1;
    pid_t pid = do_fork(name);
    if (pid <= 0) return -1;
    ChannelProc *c = &g_channels[g_count++];
    c->pid = pid;
    c->restart_count = 0;
    c->first_crash = 0;
    c->next_restart_at = 0;
    c->started_at = time(NULL);
    snprintf(c->name, sizeof(c->name), "%s", name);
    update_pid(db, name, pid);
    /* Give the successor a full threshold to connect — the mark it inherits
     * is the wedged predecessor's, and would otherwise be instantly fatal. */
    channel_activity_reset(db, name);
    return 0;
}

char *channel_config_get(sqlite3 *db, const char *channel_name, const char *key) {
    if (!db || !channel_name || !key) return NULL;
    sqlite3_stmt *s;
    char ext[128] = "";
    if (sqlite3_prepare_v2(db,
            "SELECT extension_name FROM channels WHERE name=?1",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, channel_name, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(s, 0);
            if (v) snprintf(ext, sizeof(ext), "%s", v);
        }
        sqlite3_finalize(s);
    }
    if (!ext[0]) return NULL;
    char full[192];
    snprintf(full, sizeof(full), "%s.%s", ext, key);
    return config_get(db, full);
}

/* Did the registry actually answer for `key`?
 *
 * config_get() returns NULL for "no such key" AND for "the query failed" —
 * fine for every other caller, fatal here: the gate's 0 makes channel_tick
 * SIGTERM a running channel, so a locked/corrupt-DB moment would take down a
 * healthy one. This mirrors config_get()'s primary read (same table, same
 * columns, same WHERE) purely to tell the two NULLs apart: a connection that
 * cannot answer that read is exactly the case the -1 invariant exists for.
 * Returns 1 = the read machinery worked, 0 = it did not. */
static int config_read_ok(sqlite3 *db, const char *key) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT COALESCE(value, default_value), COALESCE(secret,0)"
            " FROM config WHERE key=?1", -1, &s, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(s, 1, key, -1, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    return rc == SQLITE_ROW || rc == SQLITE_DONE;
}

/* Resolve one registry key for the gate. Returns the value (caller frees) when
 * it resolves non-empty; otherwise NULL with *unreadable set to 1 if the DB
 * refused to answer and 0 if the key is genuinely absent/empty. */
static char *gate_config_get(sqlite3 *db, const char *key, int *unreadable) {
    *unreadable = 0;
    char *v = config_get(db, key);
    if (v && v[0]) return v;
    free(v);
    if (!config_read_ok(db, key)) *unreadable = 1;
    return NULL;
}

int channel_should_launch(sqlite3 *db, const char *name, char *why, size_t cap) {
    if (why && cap) why[0] = '\0';

    /* Trust status and the owning extension come from the same row — one read.
     * A query that fails outright is NOT an answer: return -1 so the caller can
     * tell "the operator turned this off" from "the DB did not respond", and
     * not SIGTERM a healthy channel over a transient error. */
    char ext[128] = "";
    char status[32] = "";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT status, extension_name FROM channels WHERE name=?1",
            -1, &s, NULL) != SQLITE_OK) {
        if (why) snprintf(why, cap, "channel lookup failed: %s", sqlite3_errmsg(db));
        return -1;
    }
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    int rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) {
        const char *st = (const char *)sqlite3_column_text(s, 0);
        const char *ex = (const char *)sqlite3_column_text(s, 1);
        if (st) snprintf(status, sizeof(status), "%s", st);
        if (ex) snprintf(ext, sizeof(ext), "%s", ex);
    }
    sqlite3_finalize(s);
    if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
        if (why) snprintf(why, cap, "channel lookup failed: %s", sqlite3_errmsg(db));
        return -1;
    }
    if (strcmp(status, "active") != 0) {
        if (why) snprintf(why, cap, "trust status %s", status[0] ? status : "missing");
        return 0;
    }

    if (!ext[0]) {
        if (why) snprintf(why, cap, "no extension registered for this channel");
        return 0;
    }

    /* <ext>.enabled — built from the extension_name already in hand rather than
     * via channel_config_get, which would re-read the channels row (a fifth
     * query, and one more place to fail open). */
    char key[192];
    snprintf(key, sizeof(key), "%s.enabled", ext);
    int unreadable = 0;
    char *enabled = gate_config_get(db, key, &unreadable);
    if (unreadable) {
        if (why) snprintf(why, cap, "enabled lookup failed: %s", sqlite3_errmsg(db));
        return -1;
    }
    int on = enabled && strcmp(enabled, "0") != 0;
    free(enabled);
    if (!on) {
        if (why) snprintf(why, cap, "not enabled (set <ext>.enabled=1)");
        return 0;
    }

    /* Every required config key must resolve non-empty. Collect keys first,
     * then resolve — config_get reads the same table. */
    char keys[16][192];
    int nkeys = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT key FROM config WHERE required=1"
            " AND substr(key, 1, length(?1)+1) = ?1 || '.'",
            -1, &s, NULL) != SQLITE_OK) {
        if (why) snprintf(why, cap, "required-key lookup failed: %s", sqlite3_errmsg(db));
        return -1;
    }
    sqlite3_bind_text(s, 1, ext, -1, SQLITE_STATIC);
    int krc;
    while ((krc = sqlite3_step(s)) == SQLITE_ROW && nkeys < 16) {
        const char *k = (const char *)sqlite3_column_text(s, 0);
        if (k) snprintf(keys[nkeys++], sizeof(keys[0]), "%s", k);
    }
    sqlite3_finalize(s);
    /* A scan that died mid-walk yields a short list, and a short list reads as
     * "nothing required" — the same fail-open the prepare above guards. */
    if (krc != SQLITE_ROW && krc != SQLITE_DONE) {
        if (why) snprintf(why, cap, "required-key lookup failed: %s", sqlite3_errmsg(db));
        return -1;
    }
    for (int i = 0; i < nkeys; i++) {
        char *v = gate_config_get(db, keys[i], &unreadable);
        int have = (v != NULL);
        free(v);
        if (unreadable) {
            if (why) snprintf(why, cap, "config lookup failed for %s: %s",
                              keys[i], sqlite3_errmsg(db));
            return -1;
        }
        if (!have) {
            if (why) snprintf(why, cap, "required config %s unresolved", keys[i]);
            return 0;
        }
    }
    return 1;
}

int channel_launch_all(sqlite3 *db) {
    char names[CHANNEL_MAX][64];  /* matches ChannelProc.name */
    int n = active_channel_names(db, names, CHANNEL_MAX - g_count);
    int launched = 0;
    for (int i = 0; i < n; i++) {
        char why[192];
        int gate = channel_should_launch(db, names[i], why, sizeof(why));
        if (gate != 1) {
            /* A definite "no" is routine operator state; a gate that could not
             * be read is a fault, and the channel stays down until the next
             * tick re-asks — worth more than an INFO line. */
            if (gate < 0)
                LOG_WARN_("channel gate unreadable name=%s reason=\"%s\"", names[i], why);
            else
                LOG_INFO_("channel skipped name=%s reason=\"%s\"", names[i], why);
            continue;
        }
        if (start_channel(db, names[i]) == 0) launched++;
    }
    return launched;
}


void channel_shutdown_all(void) {
    for (int i = 0; i < g_count; i++)
        if (g_channels[i].pid > 0) kill(g_channels[i].pid, SIGTERM);
    for (int attempt = 0; attempt < 10 && g_count > 0; attempt++) {
        usleep(100000);
        int status; pid_t p;
        while ((p = waitpid(-1, &status, WNOHANG)) > 0) {
            ChannelProc *c = find_by_pid(p);
            if (c) remove_channel(c);
        }
    }
    for (int i = 0; i < g_count; i++)
        if (g_channels[i].pid > 0) kill(g_channels[i].pid, SIGKILL);
    g_count = 0;
}
int channel_reap(pid_t pid, sqlite3 *db) {
    ChannelProc *c = find_by_pid(pid);
    if (!c) return 0;

    time_t now = time(NULL);
    c->pid = -1;
    c->restart_count++;

    if (c->first_crash == 0) c->first_crash = now;

    /* Flap detection: 3+ crashes in 5 minutes → broken */
    if (c->restart_count >= CHANNEL_MAX_RESTARTS &&
        (now - c->first_crash) < CHANNEL_FLAP_WINDOW) {
        /* Auto-revert first: a swapped-in extension that flaps goes back to
         * its predecessor instead of taking the channel down — a broken
         * channel is exactly the one the operator can't be reached through.
         * prev is cleared by the revert, so a flap of the reverted code
         * (second pass through here) falls through to 'broken'. */
        char *prev = channel_prev_extension(db, c->name);
        if (prev) {
            const char *sql =
                "UPDATE channels SET extension_name=prev_extension_name,"
                " prev_extension_name=NULL WHERE name=?;";
            sqlite3_stmt *s;
            if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
                sqlite3_bind_text(s, 1, c->name, -1, SQLITE_STATIC);
                sqlite3_step(s); sqlite3_finalize(s);
            }
            LOG_ERROR_("channel flapping name=%s crashes=%d — reverted to extension=%s",
                       c->name, c->restart_count, prev);
            char note[256];
            snprintf(note, sizeof(note),
                     "⚠ channel %s crashed %d times and was reverted to extension "
                     "'%s'. The failed extension is still registered — fix and "
                     "re-swap when ready.", c->name, c->restart_count, prev);
            channel_notify_admins(db, c->name, note);
            free(prev);
            c->restart_count = 0;
            c->first_crash = 0;
            c->next_restart_at = now + 2;   /* respawn runs the reverted code */
            return 1;
        }

        LOG_ERROR_("channel flapping name=%s crashes=%d window=%lds status=broken",
                   c->name, c->restart_count, (long)(now - c->first_crash));
        const char *sql = "UPDATE channels SET status='broken' WHERE name=?;";
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, c->name, -1, SQLITE_STATIC);
            sqlite3_step(s); sqlite3_finalize(s);
        }
        remove_channel(c);
        return 1;
    }

    /* Exponential backoff: min(60, 1 << restart_count) */
    int delay = 1 << c->restart_count;
    if (delay > CHANNEL_MAX_BACKOFF) delay = CHANNEL_MAX_BACKOFF;
    c->next_restart_at = now + delay;
    LOG_INFO_("channel died name=%s pid=%d restart_count=%d delay=%ds",
              c->name, (int)pid, c->restart_count, delay);
    return 1;
}

void channel_tick(sqlite3 *db) {
    time_t now = time(NULL);

    /* Reconcile: the channels table is the desired state, g_channels the
     * running set. Launch newly-activated channels and stop deactivated ones,
     * so --activate / channel swap take effect on a live daemon instead of
     * waiting for a restart. */
    char desired[CHANNEL_MAX][64];
    int nd = active_channel_names(db, desired, CHANNEL_MAX);

    /* Trust-'active' says the code *may* run; the launch gate (enabled key +
     * required config) decides whether it does. Applying the gate here, to the
     * desired set itself, rather than only at the launch site below, is what
     * makes `<ext>.enabled=0` mean "off" for a channel that is already up:
     * the stop loop then SIGTERMs it and untracks it, which in turn keeps the
     * backoff-respawn loop (which only walks tracked channels) from bringing a
     * crashed-and-since-disabled channel back. */
    int keep = 0;
    for (int i = 0; i < nd; i++) {
        int gate = channel_should_launch(db, desired[i], NULL, 0);
        /* Indeterminate (-1): the gate could not be read, which is not the same
         * as a "no". Leave a running channel where it is rather than killing it
         * over a DB blip, but start nothing new on an answer we do not have. */
        if (gate == 0 || (gate < 0 && !find_by_name(desired[i]))) continue;
        if (keep != i) memcpy(desired[keep], desired[i], sizeof(desired[0]));
        keep++;
    }
    nd = keep;

    for (int i = 0; i < nd; i++) {
        if (find_by_name(desired[i])) continue;
        if (start_channel(db, desired[i]) == 0)
            LOG_INFO_("channel launch name=%s reason=activated", desired[i]);
    }

    for (int i = 0; i < g_count; ) {
        ChannelProc *c = &g_channels[i];
        int wanted = 0;
        for (int j = 0; j < nd; j++)
            if (strcmp(c->name, desired[j]) == 0) { wanted = 1; break; }
        if (wanted) { i++; continue; }
        /* Untrack before killing: channel_reap only reschedules processes it
         * finds in g_channels, so an intentional stop never respawns. */
        /* Dropped out of the desired set: trust-deactivated, or the launch
         * gate went false (enabled=0 / a required config key cleared). */
        LOG_INFO_("channel stop name=%s reason=unwanted", c->name);
        if (c->pid > 0) kill(c->pid, SIGTERM);
        update_pid(db, c->name, 0);
        remove_channel(c);   /* swaps the last entry into slot i — revisit i */
    }

    for (int i = 0; i < g_count; i++) {
        ChannelProc *c = &g_channels[i];

        /* Restart channels whose backoff expired */
        if (c->pid <= 0 && c->next_restart_at > 0 && now >= c->next_restart_at) {
            c->next_restart_at = 0;
            pid_t pid = do_fork(c->name);
            if (pid > 0) {
                c->pid = pid;
                c->started_at = now;
                update_pid(db, c->name, pid);
                channel_activity_reset(db, c->name);   /* grace, see start_channel */
                LOG_INFO_("channel respawn name=%s pid=%d",
                          c->name, (int)pid);
            }
        }

        /* Reset restart_count if healthy for 5 minutes */
        if (c->pid > 0 && c->restart_count > 0 &&
            c->started_at > 0 && (now - c->started_at) > CHANNEL_FLAP_WINDOW) {
            c->restart_count = 0;
            c->first_crash = 0;
        }

        /* Deaf watchdog: the process is up but its transport went silent.
         * SIGTERM only — the reap/backoff path above is what restarts it, so a
         * bounce here is indistinguishable from a crash to the rest of
         * supervision (flap detection included, which is what eventually marks
         * a channel that can never receive as broken). The mark is reset on
         * the way out so a child that ignores SIGTERM is re-signalled once per
         * threshold rather than once per tick. */
        if (c->pid > 0) {
            time_t age = channel_activity_age(db, c->name);
            int limit = age >= 0 ? channel_deaf_timeout(db, c->name) : 0;
            if (limit > 0 && age >= limit) {
                LOG_ERROR_("channel deaf name=%s pid=%d silent=%lds threshold=%ds"
                           " — restarting", c->name, (int)c->pid, (long)age, limit);
                kill(c->pid, SIGTERM);
                channel_activity_reset(db, c->name);
            }
        }
    }
}


void channel_consume_events(sqlite3 *db) {
    /* One event per pass, SELECT finalized before any write. Holding the
     * SELECT open across the loop body's writes (inbox, sessions, the delete
     * below) keeps a read snapshot open, and the write must then upgrade it.
     * A concurrent runner commit — telegram writes tg_offset right after
     * sending the wake byte — lands inside that snapshot and the upgrade
     * fails with an *immediate* SQLITE_BUSY (busy_timeout does not apply to
     * snapshot upgrades), parking the event until the next wake. The id
     * cursor also lets a failed event be skipped for this pass instead of
     * refetching it forever. */
    const char *sql = "SELECT id, channel_name, event_type, payload"
                      " FROM channel_events WHERE id > ? ORDER BY id ASC LIMIT 1;";
    int64_t cursor = 0;
    int processed = 0;

    for (;;) {
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) break;
        sqlite3_bind_int64(stmt, 1, cursor);
        if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); break; }
        int64_t eid = sqlite3_column_int64(stmt, 0);
        const char *v = (const char *)sqlite3_column_text(stmt, 1);
        char *ch_name = v ? strdup(v) : NULL;
        v = (const char *)sqlite3_column_text(stmt, 2);
        char *etype = v ? strdup(v) : NULL;
        v = (const char *)sqlite3_column_text(stmt, 3);
        char *payload = v ? strdup(v) : NULL;
        sqlite3_finalize(stmt);
        cursor = eid;

        if (!ch_name || !payload || !etype)
            goto del;

        /* Structural approval decision (inline-button tap, or any other
         * channel-specific UI for a yes/once/no answer). The extension
         * already interpreted its platform's surface and gated the sender
         * (see channel_telegram.qjs processCallback); here it becomes a
         * DECIDE intent like any other and runs the same allow → execute
         * pipeline the chat commands use. The approval carries its own
         * session_id, so this skips channel_routes/session lookup. */
        if (strcmp(etype, "approval_decision") == 0) {
            processed++;
            ChannelIntent it;
            if (channel_intent_from_decision_event(db, payload, &it) != 0) {
                LOG_ERROR_("channel approval_decision malformed ch=%s eid=%lld payload=%s",
                           ch_name, (long long)eid, payload);
                goto del;
            }
            const char *who = it.sender[0] ? it.sender : NULL;
            if (channel_intent_allowed(db, ch_name, who, &it))
                channel_intent_execute(db, ch_name, NULL, who, &it);
            goto del;
        }

        if (strcmp(etype, "message") != 0)
            goto del;

        /* Resolve the chat's pinned session; first contact creates + pins */
        {
            /* Extract chat_id from payload via SQLite json_extract */
            const char *cid = "0";
            char cid_buf[64] = {0};
            {
                const char *jsql = "SELECT CAST(json_extract(?, '$.chat_id') AS TEXT);";
                sqlite3_stmt *js;
                if (sqlite3_prepare_v2(db, jsql, -1, &js, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(js, 1, payload, -1, SQLITE_STATIC);
                    if (sqlite3_step(js) == SQLITE_ROW) {
                        const char *val = (const char *)sqlite3_column_text(js, 0);
                        if (val && val[0] && strlen(val) < sizeof(cid_buf)) {
                            memcpy(cid_buf, val, strlen(val) + 1);
                            cid = cid_buf;
                        }
                    }
                    sqlite3_finalize(js);
                }
            }

            /* Authority keys on the *sender*, never the chat: admin_ids are
             * user ids and a Discord DM channel id is not one. Channels that
             * emit no sender_id (custom handlers) fall back to the chat id,
             * which is what Telegram DMs report anyway. */
            char sender_buf[64] = {0};
            const char *sender = cid;
            {
                const char *jsql = "SELECT CAST(json_extract(?, '$.sender_id') AS TEXT);";
                sqlite3_stmt *js;
                if (sqlite3_prepare_v2(db, jsql, -1, &js, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(js, 1, payload, -1, SQLITE_STATIC);
                    if (sqlite3_step(js) == SQLITE_ROW) {
                        const char *val = (const char *)sqlite3_column_text(js, 0);
                        if (val && val[0] && strlen(val) < sizeof(sender_buf)) {
                            memcpy(sender_buf, val, strlen(val) + 1);
                            sender = sender_buf;
                        }
                    }
                    sqlite3_finalize(js);
                }
            }

            /* Admin intents are peeled off before any session dispatch, so
             * they work even when the pinned session is wedged. Parse →
             * allow → execute (channel_intent.c); a non-admin's recognized
             * command is NOT swallowed — it falls through as ordinary chat
             * text for the agent to answer. */
            {
                char *text = NULL;
                sqlite3_stmt *js;
                if (sqlite3_prepare_v2(db, "SELECT json_extract(?1,'$.text');",
                                       -1, &js, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(js, 1, payload, -1, SQLITE_STATIC);
                    if (sqlite3_step(js) == SQLITE_ROW) {
                        const char *val = (const char *)sqlite3_column_text(js, 0);
                        if (val) text = strdup(val);
                    }
                    sqlite3_finalize(js);
                }
                ChannelIntent it = channel_intent_from_text(text);
                free(text);
                if (it.kind != CHANNEL_INTENT_NONE &&
                    channel_intent_allowed(db, ch_name, sender, &it) &&
                    channel_intent_execute(db, ch_name, cid, sender, &it)) {
                    processed++;
                    goto del;
                }
            }

            /* The pin IS the binding: route → session, session → agent
             * (route-model unification, v33). No wildcard fallback — channel
             * policy lives on channels.default_agent. */
            int64_t sid = -1;
            char *agent = NULL;
            {
                sqlite3_stmt *ps;
                if (sqlite3_prepare_v2(db,
                        "SELECT r.session_id, s.agent_name FROM channel_routes r"
                        " JOIN sessions s ON s.id = r.session_id"
                        " WHERE r.channel_name=?1 AND r.chat_id=?2;",
                        -1, &ps, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(ps, 1, ch_name, -1, SQLITE_STATIC);
                    sqlite3_bind_text(ps, 2, cid, -1, SQLITE_STATIC);
                    if (sqlite3_step(ps) == SQLITE_ROW) {
                        sid = sqlite3_column_int64(ps, 0);
                        const char *a = (const char *)sqlite3_column_text(ps, 1);
                        if (a) agent = strdup(a);
                    }
                    sqlite3_finalize(ps);
                }
            }
            if (sid <= 0) {
                /* Unrouted chat — the gate (specs/channels.md). Chat
                 * membership is not authority: channels.default_agent set =
                 * open door; an admin chat falls back to the global
                 * default_agent; everyone else drops with a log line + a
                 * one-time admin notification. */
                int is_admin = channel_id_is_admin(db, ch_name, sender);
                char *gate_filter = NULL;
                agent = channel_default_agent(db, ch_name, &gate_filter);
                if (!agent && is_admin)
                    agent = config_get(db, "default_agent");
                if (!agent) {
                    free(gate_filter);
                    if (is_admin) {
                        LOG_WARN_("channel admin chat but no default agent"
                                  " ch=%s chat=%s — dropping", ch_name, cid);
                    } else {
                        LOG_WARN_("channel unrouted chat dropped ch=%s chat=%s",
                                  ch_name, cid);
                        gate_notify_unknown(db, ch_name, cid, payload);
                    }
                    goto del;
                }
                /* First contact: create the session and write the pin back,
                 * so the exact-route invariant holds from here on. Sessions
                 * born from channel policy carry the channel's unrouted
                 * filter (channels.default_tool_filter), not a route's —
                 * admins included: ambient reach through the open door is
                 * still unrouted, real admin power comes from a routed chat. */
                sid = session_create_filtered(db, ch_name, agent, -1, 0, gate_filter);
                free(gate_filter);
                if (sid > 0) {
                    channel_pin_session(db, ch_name, cid, sid);
                    LOG_INFO_("channel new_session ch=%s chat=%s sid=%lld agent=%s",
                              ch_name, cid, (long long)sid, agent);
                }
            }
            if (sid <= 0) { free(agent); goto del; }
            /* Display name of the chat, as the runner observed it. Bots
             * deserve to know where they are chatting — the model reads it in
             * the system prompt; every send still addresses by chat_id. The
             * IS NOT guard makes the common case (name unchanged, or absent)
             * a no-op write. */
            {
                sqlite3_stmt *ts;
                if (sqlite3_prepare_v2(db,
                        "UPDATE sessions SET chat_title=json_extract(?2,'$.chat_title')"
                        " WHERE id=?1 AND json_extract(?2,'$.chat_title') IS NOT NULL"
                        "   AND chat_title IS NOT json_extract(?2,'$.chat_title')",
                        -1, &ts, NULL) == SQLITE_OK) {
                    sqlite3_bind_int64(ts, 1, sid);
                    sqlite3_bind_text(ts, 2, payload, -1, SQLITE_STATIC);
                    sqlite3_step(ts);
                    sqlite3_finalize(ts);
                }
            }
            if (sid > 0) {
                LOG_INFO_("channel event ch=%s sid=%lld type=%s",
                          ch_name, (long long)sid, etype);
                processed++;
                /* Media-bearing message (voice, photo, …): the chat model
                 * can't take it — park the envelope in media_jobs (the bytes
                 * stay in the channel's media spool file it references) and
                 * hand it to a capability-matched preprocessor (transcription
                 * / description). Only its text output ever reaches
                 * inbox/entries; the spool file is deleted on resolve. */
                int has_media = 0;
                {
                    sqlite3_stmt *ms;
                    if (sqlite3_prepare_v2(db,
                            "SELECT json_extract(?1,'$.media') IS NOT NULL",
                            -1, &ms, NULL) == SQLITE_OK) {
                        sqlite3_bind_text(ms, 1, payload, -1, SQLITE_STATIC);
                        if (sqlite3_step(ms) == SQLITE_ROW)
                            has_media = sqlite3_column_int(ms, 0);
                        sqlite3_finalize(ms);
                    }
                }
                if (has_media) {
                    sqlite3_stmt *mj;
                    if (sqlite3_prepare_v2(db,
                            "INSERT INTO media_jobs(session_id, source, payload)"
                            " VALUES(?,?,?);", -1, &mj, NULL) == SQLITE_OK) {
                        sqlite3_bind_int64(mj, 1, sid);
                        sqlite3_bind_text(mj, 2, ch_name, -1, SQLITE_STATIC);
                        sqlite3_bind_text(mj, 3, payload, -1, SQLITE_STATIC);
                        int mrc = sqlite3_step(mj);
                        int64_t jid = sqlite3_last_insert_rowid(db);
                        sqlite3_finalize(mj);
                        if (mrc == SQLITE_DONE) {
                            /* Submit failure isn't fatal: the row survives and
                             * a daemon restart resubmits it. */
                            if (llm_worker_submit_transcribe(db, jid, sid, agent) != 0)
                                LOG_ERROR_("channel media job submit failed"
                                           " ch=%s sid=%lld job=%lld",
                                           ch_name, (long long)sid, (long long)jid);
                        } else {
                            LOG_ERROR_("channel media job insert failed ch=%s sid=%lld",
                                       ch_name, (long long)sid);
                        }
                    }
                    free(agent);
                    goto del;
                }
                /* Everything that wasn't an allowed admin intent (peeled off
                 * above) is ordinary conversation, forwarded as-is. Free
                 * text is never an implicit decision: a pending approval
                 * stays pending until a structural DECIDE intent resolves it
                 * (or it expires). */
                /* The raw envelope never reaches entries (F19): hand the inbox
                 * extracted plain text — bare text for DMs, sender-prefixed
                 * for groups (attribution is load-bearing in explicit-mode
                 * groups). A payload with no $.text (custom channel emitting
                 * its own shape) passes through unchanged. */
                char *content = NULL;
                {
                    const char *fsql =
                        "SELECT CASE"
                        " WHEN json_extract(?1,'$.text') IS NULL THEN ?1"
                        " WHEN json_extract(?1,'$.chat_type')='group'"
                        "  AND COALESCE(json_extract(?1,'$.sender_name'),'')<>''"
                        " THEN json_extract(?1,'$.sender_name')||': '||json_extract(?1,'$.text')"
                        " ELSE json_extract(?1,'$.text') END;";
                    sqlite3_stmt *fs;
                    if (sqlite3_prepare_v2(db, fsql, -1, &fs, NULL) == SQLITE_OK) {
                        sqlite3_bind_text(fs, 1, payload, -1, SQLITE_STATIC);
                        if (sqlite3_step(fs) == SQLITE_ROW) {
                            const char *v = (const char *)sqlite3_column_text(fs, 0);
                            if (v) content = strdup(v);
                        }
                        sqlite3_finalize(fs);
                    }
                }
                int64_t irc = inbox_insert_scanned(db, sid, ch_name, NULL,
                                                   content ? content : payload);
                free(content);
                if (irc < 0) {
                    /* Leave the row in channel_events (skip the del: below)
                     * so the next consume pass retries it — but log it, since
                     * a silent retry-forever with no trace is undebuggable. */
                    LOG_ERROR_("channel inbox_insert failed ch=%s sid=%lld eid=%lld, will retry",
                               ch_name, (long long)sid, (long long)eid);
                    free(agent);
                    free(ch_name); free(etype); free(payload);
                    continue;
                }
                wake_session(sid);
            }
            free(agent);
        }
del:;
        free(ch_name); free(etype); free(payload);
        const char *dsql = "DELETE FROM channel_events WHERE id=?;";
        sqlite3_stmt *ds;
        if (sqlite3_prepare_v2(db, dsql, -1, &ds, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(ds, 1, eid);
            sqlite3_step(ds); sqlite3_finalize(ds);
        }
    }
    if (processed > 0)
        LOG_INFO_("channel consume_done count=%d", processed);
}

time_t channel_next_deadline(void) {
    time_t earliest = 0;
    for (int i = 0; i < g_count; i++) {
        time_t t = g_channels[i].next_restart_at;
        if (t > 0 && (earliest == 0 || t < earliest))
            earliest = t;
    }
    return earliest;
}
