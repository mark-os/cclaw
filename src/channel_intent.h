#ifndef CCLAW_CHANNEL_INTENT_H
#define CCLAW_CHANNEL_INTENT_H

/* Channel administration, as three separated concerns:
 *
 *   1. Surface interpretation — platform input (chat text today; buttons,
 *      native slash commands, reactions, transcripts tomorrow) becomes a
 *      structural ChannelIntent. Pure recognition: no authority, no side
 *      effects.
 *   2. Authority — may this sender issue this intent here?
 *      channel_intent_allowed() is the single policy point.
 *   3. Execution — channel_intent_execute() validates the object (scope,
 *      state) and performs the read or transition. Transitions go through
 *      the same entry points every approver/binder uses: resolve_approval()
 *      and the channel_routes pin.
 *
 * The event consumer (channel.c) wires them: parse → allow → execute.
 * Anything that isn't an allowed intent falls through as ordinary
 * conversation for the session's agent.
 */

#include <sqlite3.h>
#include <stdint.h>

typedef enum {
    CHANNEL_INTENT_NONE = 0,
    CHANNEL_INTENT_DECIDE,        /* decide a parked approval */
    CHANNEL_INTENT_NEW_SESSION,   /* /new — fresh session, re-point the pin */
    CHANNEL_INTENT_PIN_SESSION,   /* /sessions <id> — attach an existing one */
    CHANNEL_INTENT_LIST_SESSIONS, /* /sessions — this chat's history */
    CHANNEL_INTENT_STATUS,        /* /status — read-only pin/model/spend */
} ChannelIntentKind;

/* What a DECIDE means, as the surface expressed it. Chat's bare /approve
 * carries no strength — the executor picks it from approvals.resolve (chat
 * is the low-ceremony surface: "rerun" gets ONCE so text never mints
 * standing authority, "apply" gets ALWAYS because ONCE is incoherent for
 * it). Button taps say exactly what they mean. */
typedef enum {
    CHANNEL_DECIDE_APPROVE = 1,   /* strength chosen by the executor */
    CHANNEL_DECIDE_APPROVE_ALWAYS,
    CHANNEL_DECIDE_APPROVE_ONCE,
    CHANNEL_DECIDE_DENY,
} ChannelDecide;

typedef enum {
    CHANNEL_ORIGIN_CHAT = 0,  /* parsed from message text by the daemon */
    CHANNEL_ORIGIN_EVENT,     /* structural event authored by the extension */
} ChannelIntentOrigin;

typedef struct {
    ChannelIntentKind kind;
    ChannelIntentOrigin origin;
    ChannelDecide decide;     /* DECIDE only */
    int64_t approval_id;      /* DECIDE; <= 0 = unusable arg (usage reply) */
    int64_t session_id;       /* PIN_SESSION */
} ChannelIntent;

/* Chat text → intent. kind=NONE for anything that isn't an admin command.
 * Recognition is exact: "/approvals" is not "/approve", "/new x" is not
 * "/new". Pure — no DB, no allocation. */
ChannelIntent channel_intent_from_text(const char *text);

/* approval_decision payload {approval_id, decision:"yes"|"once"|other} →
 * DECIDE intent. `db` is used as the JSON parser only (jsmn stays banned
 * where a handle exists). Returns 0, or -1 for a malformed payload. */
int channel_intent_from_decision_event(sqlite3 *db, const char *payload,
                                       ChannelIntent *out);

/* Authority: may `sender` issue `it` on this channel? The one place the
 * policy lives — see the function body for the per-origin rules. */
int channel_intent_allowed(sqlite3 *db, const char *ch_name,
                           const char *sender, const ChannelIntent *it);

/* Execute an allowed intent. `cid` is the reply surface — NULL for origins
 * with no chat to answer into (structural events), which mutes replies but
 * changes nothing else. `sender` may be NULL; when present it is recorded
 * in approvals.decided_via. Returns 1 = consumed. */
int channel_intent_execute(sqlite3 *db, const char *ch_name, const char *cid,
                           const char *sender, const ChannelIntent *it);

#endif
