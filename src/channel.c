#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "channel.h"
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
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
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

/* Channel open-door policy: channels.default_agent (NULL = fail-closed). */
static char *channel_default_agent(sqlite3 *db, const char *channel_name) {
    sqlite3_stmt *s;
    char *agent = NULL;
    if (sqlite3_prepare_v2(db, "SELECT default_agent FROM channels WHERE name=?;",
                           -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, channel_name, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(s, 0);
            if (v && v[0]) agent = strdup(v);
        }
        sqlite3_finalize(s);
    }
    return agent;
}

/* Queue a plain-text reply to one chat (command feedback — not agent output,
 * so it goes straight to the outbox like admin notices do). */
static void channel_chat_reply(sqlite3 *db, const char *channel_name,
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
static void channel_pin_session(sqlite3 *db, const char *channel_name,
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

/* Chat commands — /new and /sessions rebind the chat, so they are authority
 * actions: admin_ids senders only, handled in C before any session dispatch
 * (they must work even when the pinned session is wedged). A non-admin's
 * /new is NOT swallowed — it falls through as an ordinary message for the
 * agent to answer. Returns 1 when the event was consumed as a command.
 * `cid` is the chat the command rebinds; `sender` is who is allowed to. */
static int channel_chat_command(sqlite3 *db, const char *ch_name,
                                const char *cid, const char *sender,
                                const char *payload) {
    char *text = NULL;
    sqlite3_stmt *js;
    if (sqlite3_prepare_v2(db, "SELECT json_extract(?1,'$.text');",
                           -1, &js, NULL) == SQLITE_OK) {
        sqlite3_bind_text(js, 1, payload, -1, SQLITE_STATIC);
        if (sqlite3_step(js) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(js, 0);
            if (v) text = strdup(v);
        }
        sqlite3_finalize(js);
    }
    if (!text) return 0;
    int is_new = strcmp(text, "/new") == 0;
    int is_list = strncmp(text, "/sessions", 9) == 0 &&
                  (text[9] == '\0' || text[9] == ' ');
    if ((!is_new && !is_list) || !channel_id_is_admin(db, ch_name, sender)) {
        free(text);
        return 0;
    }

    if (is_new) {
        /* Same agent as the current pin; open-door default, then the global
         * default_agent, when the chat has never been routed. */
        char *agent = db_channel_binding_get(db, ch_name, cid);
        if (!agent) agent = channel_default_agent(db, ch_name);
        if (!agent) agent = config_get(db, "default_agent");
        if (!agent) {
            channel_chat_reply(db, ch_name, cid, "no agent for this chat");
            free(text);
            return 1;
        }
        int64_t sid = session_create_filtered(db, ch_name, agent, -1, 0, NULL);
        if (sid > 0) {
            channel_pin_session(db, ch_name, cid, sid);
            char msg[96];
            snprintf(msg, sizeof(msg), "new session #%lld (%s)",
                     (long long)sid, agent);
            channel_chat_reply(db, ch_name, cid, msg);
            LOG_INFO_("channel /new ch=%s chat=%s sid=%lld agent=%s",
                      ch_name, cid, (long long)sid, agent);
        } else {
            channel_chat_reply(db, ch_name, cid, "session create failed");
        }
        free(agent);
        free(text);
        return 1;
    }

    /* "/sessions <id>" re-pins; bare "/sessions" lists this chat's history. */
    int64_t pick = text[9] == ' ' ? strtoll(text + 10, NULL, 10) : 0;
    if (pick > 0) {
        sqlite3_stmt *chk;
        int found = 0;
        if (sqlite3_prepare_v2(db, "SELECT 1 FROM sessions WHERE id=?;",
                               -1, &chk, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(chk, 1, pick);
            if (sqlite3_step(chk) == SQLITE_ROW) found = 1;
            sqlite3_finalize(chk);
        }
        if (found) {
            channel_pin_session(db, ch_name, cid, pick);
            char msg[64];
            snprintf(msg, sizeof(msg), "attached session #%lld", (long long)pick);
            channel_chat_reply(db, ch_name, cid, msg);
            LOG_INFO_("channel /sessions attach ch=%s chat=%s sid=%lld",
                      ch_name, cid, (long long)pick);
        } else {
            channel_chat_reply(db, ch_name, cid, "no such session");
        }
        free(text);
        return 1;
    }

    /* List: this chat's sessions newest-first, current pin starred. */
    char *listing = NULL;
    sqlite3_stmt *ls;
    if (sqlite3_prepare_v2(db,
            "SELECT COALESCE(group_concat("
            "  CASE WHEN s.id = (SELECT session_id FROM channel_routes"
            "                    WHERE channel_name=?1 AND chat_id=?2)"
            "       THEN '* ' ELSE '  ' END"
            "  || '#' || s.id || ' ' || s.agent_name || ' ' || s.state,"
            "  char(10)), 'no sessions for this chat')"
            " FROM (SELECT id, agent_name, state FROM sessions"
            "       WHERE channel_name=?1 AND chat_id=?2"
            "       ORDER BY id DESC LIMIT 10) s;",
            -1, &ls, NULL) == SQLITE_OK) {
        sqlite3_bind_text(ls, 1, ch_name, -1, SQLITE_STATIC);
        sqlite3_bind_text(ls, 2, cid, -1, SQLITE_STATIC);
        if (sqlite3_step(ls) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(ls, 0);
            if (v) listing = strdup(v);
        }
        sqlite3_finalize(ls);
    }
    channel_chat_reply(db, ch_name, cid,
                       listing ? listing : "no sessions for this chat");
    free(listing);
    free(text);
    return 1;
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

    char *enabled = channel_config_get(db, name, "enabled");
    int on = enabled && enabled[0] && strcmp(enabled, "0") != 0;
    free(enabled);
    if (!on) {
        if (why) snprintf(why, cap, "not enabled (set <ext>.enabled=1)");
        return 0;
    }

    /* Every required config key must resolve non-empty. Collect keys first,
     * then resolve — config_get reads the same table. */
    char keys[16][192];
    int nkeys = 0;
    if (ext[0]) {
        if (sqlite3_prepare_v2(db,
                "SELECT key FROM config WHERE required=1"
                " AND substr(key, 1, length(?1)+1) = ?1 || '.'",
                -1, &s, NULL) != SQLITE_OK) {
            if (why) snprintf(why, cap, "required-key lookup failed: %s", sqlite3_errmsg(db));
            return -1;
        }
        sqlite3_bind_text(s, 1, ext, -1, SQLITE_STATIC);
        while (sqlite3_step(s) == SQLITE_ROW && nkeys < 16) {
            const char *k = (const char *)sqlite3_column_text(s, 0);
            if (k) snprintf(keys[nkeys++], sizeof(keys[0]), "%s", k);
        }
        sqlite3_finalize(s);
    }
    for (int i = 0; i < nkeys; i++) {
        char *v = config_get(db, keys[i]);
        int ok = v && v[0];
        free(v);
        if (!ok) {
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
        if (channel_should_launch(db, names[i], why, sizeof(why)) != 1) {
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
         * channel-specific UI for a yes/once/no answer). Channel-agnostic:
         * the daemon never interprets raw chat text as a decision — that's
         * entirely the channel extension's concern (see channel_telegram.qjs
         * processCallback). The approval carries its own session_id, so this
         * skips channel_routes/session lookup entirely. */
        if (strcmp(etype, "approval_decision") == 0) {
            processed++;
            int64_t approval_id = -1;
            char decision_str[16] = {0};
            const char *jsql =
                "SELECT json_extract(?1,'$.approval_id'), json_extract(?1,'$.decision');";
            sqlite3_stmt *js;
            if (sqlite3_prepare_v2(db, jsql, -1, &js, NULL) == SQLITE_OK) {
                sqlite3_bind_text(js, 1, payload, -1, SQLITE_STATIC);
                if (sqlite3_step(js) == SQLITE_ROW) {
                    approval_id = sqlite3_column_int64(js, 0);
                    const char *dv = (const char *)sqlite3_column_text(js, 1);
                    if (dv) snprintf(decision_str, sizeof(decision_str), "%s", dv);
                }
                sqlite3_finalize(js);
            }
            if (approval_id > 0 && decision_str[0]) {
                /* Scope check: only the channel the approval's session is
                 * bound to may decide it — otherwise any channel could
                 * approve another channel's (or the CLI's) pending action
                 * by forging an approval_id. Mismatch or unbound session →
                 * ignore the event, approval stays pending. */
                int in_scope = 0;
                sqlite3_stmt *sc;
                if (sqlite3_prepare_v2(db,
                        "SELECT 1 FROM approvals a JOIN sessions s ON s.id=a.session_id"
                        " WHERE a.id=?1 AND s.channel_name=?2",
                        -1, &sc, NULL) == SQLITE_OK) {
                    sqlite3_bind_int64(sc, 1, approval_id);
                    sqlite3_bind_text(sc, 2, ch_name, -1, SQLITE_STATIC);
                    if (sqlite3_step(sc) == SQLITE_ROW) in_scope = 1;
                    sqlite3_finalize(sc);
                }
                if (!in_scope) {
                    LOG_WARN_("channel approval_decision out of scope ch=%s"
                              " approval=%lld — ignored",
                              ch_name, (long long)approval_id);
                    goto del;
                }
                ApprovalDecision d = APPROVAL_DENY;
                if (strcmp(decision_str, "yes") == 0) d = APPROVAL_ALWAYS;
                else if (strcmp(decision_str, "once") == 0) d = APPROVAL_ONCE;
                char decided[128];
                snprintf(decided, sizeof(decided), "channel:%s", ch_name);
                resolve_approval(approval_id, d, decided, 0);
            } else {
                LOG_ERROR_("channel approval_decision malformed ch=%s eid=%lld payload=%s",
                           ch_name, (long long)eid, payload);
            }
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

            if (channel_chat_command(db, ch_name, cid, sender, payload)) {
                processed++;
                goto del;
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
                agent = channel_default_agent(db, ch_name);
                if (!agent && is_admin)
                    agent = config_get(db, "default_agent");
                if (!agent) {
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
                 * born from channel policy carry no route tool_filter. */
                sid = session_create_filtered(db, ch_name, agent, -1, 0, NULL);
                if (sid > 0) {
                    channel_pin_session(db, ch_name, cid, sid);
                    LOG_INFO_("channel new_session ch=%s chat=%s sid=%lld agent=%s",
                              ch_name, cid, (long long)sid, agent);
                }
            }
            if (sid <= 0) { free(agent); goto del; }
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
                /* Every inbound chat message is forwarded as-is — approval
                 * decisions arrive as their own structural event type
                 * (approval_decision, above), never by interpreting chat
                 * text. A pending approval simply stays pending until a
                 * decision event resolves it (or it expires). */
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
                int64_t irc = inbox_insert_scanned(db, sid, ch_name,
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
