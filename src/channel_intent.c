#define _POSIX_C_SOURCE 200809L
#include "channel_intent.h"
#include "channel.h"
#include "approval.h"
#include "config_registry.h"
#include "db.h"
#include "log.h"
#include "resolve.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── 1. Surface interpretation ─────────────────────────────────────────── */

/* "/cmd" or "/cmd <arg…>" — returns the (leading-space-stripped) argument
 * tail, "" for the bare command, or NULL when text isn't this command. */
static const char *cmd_arg(const char *text, const char *cmd) {
    size_t n = strlen(cmd);
    if (strncmp(text, cmd, n) != 0) return NULL;
    if (text[n] == '\0') return text + n;
    if (text[n] != ' ') return NULL;
    const char *p = text + n + 1;
    while (*p == ' ') p++;
    return p;
}

ChannelIntent channel_intent_from_text(const char *text) {
    ChannelIntent it = {0};
    it.origin = CHANNEL_ORIGIN_CHAT;
    if (!text) return it;

    if (strcmp(text, "/new") == 0) {
        it.kind = CHANNEL_INTENT_NEW_SESSION;
        return it;
    }
    const char *list_arg = cmd_arg(text, "/sessions");
    if (list_arg) {
        int64_t pick = list_arg[0] ? strtoll(list_arg, NULL, 10) : 0;
        if (pick > 0) {
            it.kind = CHANNEL_INTENT_PIN_SESSION;
            it.session_id = pick;
        } else {
            it.kind = CHANNEL_INTENT_LIST_SESSIONS;
        }
        return it;
    }
    const char *approve_arg = cmd_arg(text, "/approve");
    const char *deny_arg = cmd_arg(text, "/deny");
    if (approve_arg || deny_arg) {
        const char *arg = approve_arg ? approve_arg : deny_arg;
        it.kind = CHANNEL_INTENT_DECIDE;
        it.decide = approve_arg ? CHANNEL_DECIDE_APPROVE : CHANNEL_DECIDE_DENY;
        char *end = NULL;
        long long id = arg[0] ? strtoll(arg, &end, 10) : 0;
        if (id <= 0 || (end && *end != '\0' && *end != ' ')) id = 0;
        it.approval_id = id;
        return it;
    }
    if (cmd_arg(text, "/status")) {
        it.kind = CHANNEL_INTENT_STATUS;
        return it;
    }
    return it;
}

int channel_intent_from_decision_event(sqlite3 *db, const char *payload,
                                       ChannelIntent *out) {
    memset(out, 0, sizeof(*out));
    out->origin = CHANNEL_ORIGIN_EVENT;

    int64_t approval_id = -1;
    char decision_str[16] = {0};
    sqlite3_stmt *js;
    if (sqlite3_prepare_v2(db,
            "SELECT json_extract(?1,'$.approval_id'), json_extract(?1,'$.decision'),"
            "       json_extract(?1,'$.sender');",
            -1, &js, NULL) == SQLITE_OK) {
        sqlite3_bind_text(js, 1, payload, -1, SQLITE_STATIC);
        if (sqlite3_step(js) == SQLITE_ROW) {
            approval_id = sqlite3_column_int64(js, 0);
            const char *dv = (const char *)sqlite3_column_text(js, 1);
            if (dv) snprintf(decision_str, sizeof(decision_str), "%s", dv);
            const char *sv = (const char *)sqlite3_column_text(js, 2);
            if (sv) snprintf(out->sender, sizeof(out->sender), "%s", sv);
        }
        sqlite3_finalize(js);
    }
    if (approval_id <= 0 || !decision_str[0])
        return -1;

    out->kind = CHANNEL_INTENT_DECIDE;
    out->approval_id = approval_id;
    if (strcmp(decision_str, "yes") == 0)
        out->decide = CHANNEL_DECIDE_APPROVE_ALWAYS;
    else if (strcmp(decision_str, "once") == 0)
        out->decide = CHANNEL_DECIDE_APPROVE_ONCE;
    else
        out->decide = CHANNEL_DECIDE_DENY;  /* "no" and anything unrecognized */
    return 0;
}

/* ── 2. Authority ──────────────────────────────────────────────────────── */

int channel_intent_allowed(sqlite3 *db, const char *ch_name,
                           const char *sender, const ChannelIntent *it) {
    if (it->kind == CHANNEL_INTENT_NONE) return 0;
    switch (it->origin) {
    case CHANNEL_ORIGIN_CHAT:
        /* Text commands: admin_ids senders only. Denial is not an error —
         * the caller forwards a non-admin's "/new" as ordinary chat text. */
        return sender != NULL && channel_id_is_admin(db, ch_name, sender);
    case CHANNEL_ORIGIN_EVENT:
        /* Structural events carry no sender: the extension is the gate
         * (channel_telegram.qjs checks isAdmin before emitting), and the
         * daemon trusts registered extensions — the runner is unsandboxed
         * code holding its own rw DB handle. Object scope is still checked
         * at execution. If that trust ever narrows (per-sender events,
         * third-party extensions), this is the line to change. */
        return 1;
    }
    return 0;
}

/* ── 3. Execution ──────────────────────────────────────────────────────── */

/* Reply iff the intent's origin has a chat to answer into. */
static void reply(sqlite3 *db, const char *ch_name, const char *cid,
                  const char *text) {
    if (cid) channel_chat_reply(db, ch_name, cid, text);
}

/* Decide a parked approval. The transition itself is NOT reimplemented here:
 * this is a scope+state check in front of the one entry point every approver
 * uses (resolve_approval). Out of scope reads as "doesn't exist": only the
 * channel an approval's session is bound to may decide it, and a mismatch
 * must not confirm the id exists elsewhere.
 *
 * Who decided lands in approvals.decided_via — "channel:<ch>:<sender>" when
 * the surface knows the sender, "channel:<ch>" when it doesn't (structural
 * events) — the same audit column the dashboard and CLI write. */
static int intent_exec_decide(sqlite3 *db, const char *ch_name, const char *cid,
                              const char *sender, const ChannelIntent *it) {
    int deny = it->decide == CHANNEL_DECIDE_DENY;
    const char *verb = deny ? "deny" : "approve";
    char msg[320];
    if (it->approval_id <= 0) {
        snprintf(msg, sizeof(msg),
                 "usage: /%s <approval id> — see /approvals for pending ids", verb);
        reply(db, ch_name, cid, msg);
        return 1;
    }
    int64_t id = it->approval_id;

    char state[32] = {0}, tool[64] = {0}, action[64] = {0}, resolve[16] = {0};
    int found = 0, in_scope = 0;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT a.state, COALESCE(a.tool_name,'?'), COALESCE(a.action,'?'),"
            "       COALESCE(a.resolve,'rerun'), s.channel_name IS ?2"
            " FROM approvals a JOIN sessions s ON s.id = a.session_id"
            " WHERE a.id = ?1;", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, id);
        sqlite3_bind_text(s, 2, ch_name, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) {
            found = 1;
            snprintf(state, sizeof(state), "%s", sqlite3_column_text(s, 0));
            snprintf(tool, sizeof(tool), "%s", sqlite3_column_text(s, 1));
            snprintf(action, sizeof(action), "%s", sqlite3_column_text(s, 2));
            snprintf(resolve, sizeof(resolve), "%s", sqlite3_column_text(s, 3));
            in_scope = sqlite3_column_int(s, 4);
        }
        sqlite3_finalize(s);
    }

    if (!found || !in_scope) {
        snprintf(msg, sizeof(msg), "no approval #%lld on this channel",
                 (long long)id);
        reply(db, ch_name, cid, msg);
        if (it->origin == CHANNEL_ORIGIN_EVENT)
            LOG_WARN_("channel approval_decision out of scope ch=%s"
                      " approval=%lld — ignored", ch_name, (long long)id);
        else
            LOG_WARN_("channel /%s out of scope ch=%s sender=%s approval=%lld",
                      verb, ch_name, sender ? sender : "?", (long long)id);
        return 1;
    }
    if (strcmp(state, "pending") != 0) {
        snprintf(msg, sizeof(msg),
                 "approval #%lld is already %s — nothing to %s",
                 (long long)id, state, verb);
        reply(db, ch_name, cid, msg);
        return 1;
    }

    ApprovalDecision d = APPROVAL_DENY;
    switch (it->decide) {
    case CHANNEL_DECIDE_APPROVE:
        d = strcmp(resolve, "apply") == 0 ? APPROVAL_ALWAYS : APPROVAL_ONCE;
        break;
    case CHANNEL_DECIDE_APPROVE_ALWAYS: d = APPROVAL_ALWAYS; break;
    case CHANNEL_DECIDE_APPROVE_ONCE:   d = APPROVAL_ONCE;   break;
    case CHANNEL_DECIDE_DENY:           d = APPROVAL_DENY;   break;
    }
    char decided_via[128];
    if (sender)
        snprintf(decided_via, sizeof(decided_via), "channel:%s:%s",
                 ch_name, sender);
    else
        snprintf(decided_via, sizeof(decided_via), "channel:%s", ch_name);
    LOG_INFO_("channel %s ch=%s chat=%s sender=%s approval=%lld tool=%s action=%s",
              verb, ch_name, cid ? cid : "-", sender ? sender : "-",
              (long long)id, tool, action);
    resolve_approval(id, d, decided_via, 0);

    snprintf(msg, sizeof(msg), "%s #%lld (%s %s)%s",
             deny ? "denied" : "approved", (long long)id, tool, action,
             (!deny && d == APPROVAL_ONCE) ? " — once" : "");
    reply(db, ch_name, cid, msg);
    return 1;
}

/* /status — one read-only query: what this chat is pinned to, which agent and
 * model answer it, and how much is outstanding/spent-ish. There is no cost
 * ledger (llm_responses carries no price), so "today" is request count. */
static int intent_exec_status(sqlite3 *db, const char *ch_name, const char *cid) {
    char msg[512];
    sqlite3_stmt *s;
    int have = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT s.id, s.agent_name, s.state,"
            "       COALESCE((SELECT model_id FROM agent_models am"
            "          WHERE am.agent_name=a.name ORDER BY pos LIMIT 1),"
            "        '(provider default)'),"
            "       (SELECT COUNT(*) FROM approvals ap"
            "          JOIN sessions s2 ON s2.id = ap.session_id"
            "         WHERE ap.state='pending' AND s2.channel_name = ?1),"
            "       (SELECT COUNT(*) FROM llm_responses"
            "         WHERE session_id = s.id"
            "           AND created_at >= unixepoch('now','start of day'))"
            " FROM channel_routes r JOIN sessions s ON s.id = r.session_id"
            " LEFT JOIN agents a ON a.name = s.agent_name"
            " WHERE r.channel_name = ?1 AND r.chat_id = ?2;",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, ch_name, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, cid, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) {
            have = 1;
            snprintf(msg, sizeof(msg),
                     "session #%lld (%s, %s)\nmodel: %s\nLLM calls today: %d\n"
                     "pending approvals on this channel: %d",
                     (long long)sqlite3_column_int64(s, 0),
                     sqlite3_column_text(s, 1), sqlite3_column_text(s, 2),
                     sqlite3_column_text(s, 3), sqlite3_column_int(s, 5),
                     sqlite3_column_int(s, 4));
        }
        sqlite3_finalize(s);
    }
    reply(db, ch_name, cid,
          have ? msg : "no session pinned to this chat (try /new)");
    return 1;
}

/* /new — re-point the chat's pin at a fresh session. Same agent as the
 * current pin; open-door default, then the global default_agent, when the
 * chat has never been routed. An unrouted chat's fresh session is still an
 * unrouted session: it takes the channel's default filter, so /new can't
 * launder the open door into full authority. */
static int intent_exec_new(sqlite3 *db, const char *ch_name, const char *cid) {
    char *agent = db_channel_binding_get(db, ch_name, cid);
    char *gate_filter = NULL;
    if (!agent) {
        agent = channel_default_agent(db, ch_name, &gate_filter);
        if (!agent) agent = config_get(db, "default_agent");
    }
    if (!agent) {
        free(gate_filter);
        reply(db, ch_name, cid, "no agent for this chat");
        return 1;
    }
    int64_t sid = session_create_filtered(db, ch_name, agent, -1, 0, gate_filter);
    free(gate_filter);
    if (sid > 0) {
        channel_pin_session(db, ch_name, cid, sid);
        char msg[96];
        snprintf(msg, sizeof(msg), "new session #%lld (%s)",
                 (long long)sid, agent);
        reply(db, ch_name, cid, msg);
        LOG_INFO_("channel /new ch=%s chat=%s sid=%lld agent=%s",
                  ch_name, cid ? cid : "-", (long long)sid, agent);
    } else {
        reply(db, ch_name, cid, "session create failed");
    }
    free(agent);
    return 1;
}

/* /sessions <id> — re-pin the chat to an existing session. */
static int intent_exec_pin(sqlite3 *db, const char *ch_name, const char *cid,
                           int64_t pick) {
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
        reply(db, ch_name, cid, msg);
        LOG_INFO_("channel /sessions attach ch=%s chat=%s sid=%lld",
                  ch_name, cid ? cid : "-", (long long)pick);
    } else {
        reply(db, ch_name, cid, "no such session");
    }
    return 1;
}

/* Bare /sessions — this chat's sessions newest-first, current pin starred. */
static int intent_exec_list(sqlite3 *db, const char *ch_name, const char *cid) {
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
    reply(db, ch_name, cid, listing ? listing : "no sessions for this chat");
    free(listing);
    return 1;
}

int channel_intent_execute(sqlite3 *db, const char *ch_name, const char *cid,
                           const char *sender, const ChannelIntent *it) {
    switch (it->kind) {
    case CHANNEL_INTENT_DECIDE:
        return intent_exec_decide(db, ch_name, cid, sender, it);
    case CHANNEL_INTENT_NEW_SESSION:
        return intent_exec_new(db, ch_name, cid);
    case CHANNEL_INTENT_PIN_SESSION:
        return intent_exec_pin(db, ch_name, cid, it->session_id);
    case CHANNEL_INTENT_LIST_SESSIONS:
        return intent_exec_list(db, ch_name, cid);
    case CHANNEL_INTENT_STATUS:
        return intent_exec_status(db, ch_name, cid);
    case CHANNEL_INTENT_NONE:
        break;
    }
    return 0;
}
