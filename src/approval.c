#define _POSIX_C_SOURCE 200809L
#include "approval.h"
#include "buf.h"
#include "config_registry.h"
#include "db.h"
#include "host_match.h"
#include "log.h"
#include "tool_args.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static char *dup_or_null(const unsigned char *s) {
    return s ? strdup((const char *)s) : NULL;
}

static Approval *row_to_approval(sqlite3_stmt *stmt) {
    Approval *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->id = sqlite3_column_int64(stmt, 0);
    a->session_id = sqlite3_column_int64(stmt, 1);
    a->tool_call_id = dup_or_null(sqlite3_column_text(stmt, 2));
    a->tool_name = dup_or_null(sqlite3_column_text(stmt, 3));
    a->action = dup_or_null(sqlite3_column_text(stmt, 4));
    a->args_json = dup_or_null(sqlite3_column_text(stmt, 5));
    a->resolve = dup_or_null(sqlite3_column_text(stmt, 6));
    a->state = dup_or_null(sqlite3_column_text(stmt, 7));
    a->decided_via = dup_or_null(sqlite3_column_text(stmt, 8));
    a->requested_at = sqlite3_column_int64(stmt, 9);
    a->expires_at = sqlite3_column_int64(stmt, 10);
    return a;
}

void approval_free(Approval *a) {
    if (!a) return;
    free(a->tool_call_id);
    free(a->tool_name);
    free(a->action);
    free(a->args_json);
    free(a->resolve);
    free(a->state);
    free(a->decided_via);
    free(a);
}

/* approval_timeout_sec — the same deadline approval.c uses to park-expire;
 * reused by --auto-approve to bound its self-expiring grants. */
int approval_timeout_seconds(sqlite3 *db) {
    int timeout = config_get_int(db, "approval_timeout_sec");
    return timeout > 0 ? timeout : config_default_int("approval_timeout_sec");
}

/* approval_block_sec, clamped to approval_timeout_sec so the short block
 * never outlasts the final expiry deadline. */
int approval_block_seconds(sqlite3 *db) {
    int block = config_get_int(db, "approval_block_sec");
    if (block <= 0) block = config_default_int("approval_block_sec");
    int timeout = approval_timeout_seconds(db);
    if (block > timeout) block = timeout;
    return block;
}

int64_t approval_create(sqlite3 *db, int64_t session_id, const char *tool_call_id,
                        const char *tool_name, const char *action,
                        const char *args_json, const char *resolve) {
    if (!resolve) resolve = "rerun";

    /* Deadline: approval_timeout_sec from the config registry */
    int64_t expires_at = (int64_t)time(NULL) + approval_timeout_seconds(db);

    const char *sql =
        "INSERT INTO approvals(session_id, tool_call_id, tool_name, action, args_json, resolve, expires_at)"
        " VALUES(?,?,?,?,?,?,?)";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_text(stmt, 2, tool_call_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, tool_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, action, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, args_json, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, resolve, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 7, expires_at);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    int64_t id = sqlite3_last_insert_rowid(db);
    LOG_INFO_("approval create id=%lld session=%lld tool=%s",
             (long long)id, (long long)session_id, tool_name ? tool_name : "?");
    return id;
}

/* Ordered like approval_get_pending_subtree, deliberately: the CLI park
 * prompt (this) and the CLI's y/n reader (that) must name the same row, or
 * the user is shown one approval and decides another. The daemon prompt path
 * resolves the just-parked approval by tool_call_id instead
 * (handle_approval_park). */
Approval *approval_get_pending(sqlite3 *db, int64_t session_id) {
    const char *sql =
        "SELECT id, session_id, tool_call_id, tool_name, action,"
        " args_json, resolve, state, decided_via, requested_at, expires_at"
        " FROM approvals WHERE session_id=? AND state='pending'"
        " ORDER BY requested_at ASC, id ASC LIMIT 1";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(stmt, 1, session_id);
    Approval *a = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        a = row_to_approval(stmt);
    sqlite3_finalize(stmt);
    return a;
}

Approval *approval_get_pending_subtree(sqlite3 *db, int64_t root_session_id) {
    const char *sql =
        "WITH RECURSIVE subtree(id) AS ("
        "  SELECT ?1"
        "  UNION ALL"
        "  SELECT s.id FROM sessions s JOIN subtree t ON s.parent_session_id = t.id"
        ")"
        " SELECT a.id, a.session_id, a.tool_call_id, a.tool_name, a.action,"
        " a.args_json, a.resolve, a.state, a.decided_via, a.requested_at, a.expires_at"
        " FROM approvals a WHERE a.session_id IN (SELECT id FROM subtree)"
        " AND a.state='pending' ORDER BY a.requested_at ASC, a.id ASC LIMIT 1";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(stmt, 1, root_session_id);
    Approval *a = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        a = row_to_approval(stmt);
    sqlite3_finalize(stmt);
    return a;
}

Approval *approval_get_for_tool_call(sqlite3 *db, int64_t session_id,
                                     const char *tool_call_id) {
    if (!tool_call_id) return NULL;
    /* Scoped to (session_id, tool_call_id): tool_call_ids come from the model
     * and are not globally unique, so a bare tool_call_id match would let a
     * reused id collide with another session's approved row. */
    const char *sql =
        "SELECT id, session_id, tool_call_id, tool_name, action,"
        " args_json, resolve, state, decided_via, requested_at, expires_at"
        " FROM approvals WHERE session_id=? AND tool_call_id=? ORDER BY id DESC LIMIT 1";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_text(stmt, 2, tool_call_id, -1, SQLITE_STATIC);
    Approval *a = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        a = row_to_approval(stmt);
    sqlite3_finalize(stmt);
    return a;
}

int approval_consume(sqlite3 *db, int64_t id) {
    /* Terminal transition approved → consumed. Makes a "once" approval
     * single-use: the gate green-lights only state='approved', so once the
     * frozen call has run, a replayed tool_call_id can no longer match it. */
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
            "UPDATE approvals SET state='consumed' WHERE id=? AND state='approved'",
            -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE && sqlite3_changes(db) == 1) ? 0 : -1;
}

/* ── capability matching: dedupe + ticket transfer ─────────────────
 * Two calls ask for the same capability when one human decision honestly
 * covers both: byte-identical args. A 'sensitive' park is per-call by
 * trust.md rule 1, so only an exact re-issue of the frozen call may dedupe
 * against it, and it never transfers (the gate excludes it from tickets). */
static int capability_match(const char *action, const char *args_a, const char *args_b) {
    (void)action;
    if (!args_a || !args_b) return 0;
    return strcmp(args_a, args_b) == 0;
}

/* Scan this session's rerun approvals in `state` for a capability match.
 * unexpired_only guards the ticket path: an approved row past its original
 * park deadline is stale authority, not a ticket. Returns row id or 0. */
static int64_t approval_match_state(sqlite3 *db, int64_t session_id,
                                    const char *state, int unexpired_only,
                                    const char *action, const char *tool_name,
                                    const char *args_json) {
    const char *sql =
        "SELECT id, args_json FROM approvals"
        " WHERE session_id=?1 AND state=?2 AND resolve='rerun'"
        "   AND action IS ?3 AND tool_name IS ?4"
        "   AND (?5 = 0 OR (expires_at IS NOT NULL AND expires_at > unixepoch()))"
        " ORDER BY id DESC LIMIT 8";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_text(stmt, 2, state, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, action, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, tool_name, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 5, unexpired_only);
    int64_t id = 0;
    while (id == 0 && sqlite3_step(stmt) == SQLITE_ROW) {
        const char *row_args = (const char *)sqlite3_column_text(stmt, 1);
        if (capability_match(action, row_args, args_json))
            id = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return id;
}

int64_t approval_find_pending_match(sqlite3 *db, int64_t session_id,
                                    const char *action, const char *tool_name,
                                    const char *args_json) {
    return approval_match_state(db, session_id, "pending", 0,
                                action, tool_name, args_json);
}

int64_t approval_take_ticket(sqlite3 *db, int64_t session_id,
                             const char *action, const char *tool_name,
                             const char *args_json) {
    int64_t id = approval_match_state(db, session_id, "approved", 1,
                                      action, tool_name, args_json);
    if (id <= 0) return 0;
    /* approval_consume is a CAS on state='approved' — a concurrent taker
     * loses cleanly and parks instead of double-spending the ticket. */
    return approval_consume(db, id) == 0 ? id : 0;
}

int approval_pending_ids(sqlite3 *db, int64_t session_id, char *buf, size_t cap) {
    if (buf && cap) buf[0] = '\0';
    const char *sql =
        "SELECT id FROM approvals WHERE session_id=? AND state='pending'"
        " ORDER BY id ASC";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int64(stmt, 1, session_id);
    int n = 0;
    size_t used = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(stmt, 0);
        if (buf && used < cap) {
            int w = snprintf(buf + used, cap - used, "%s#%lld",
                             n ? " " : "", (long long)id);
            if (w > 0 && (size_t)w < cap - used) used += (size_t)w;
            else buf[used] = '\0';   /* truncated: keep what fit */
        }
        n++;
    }
    sqlite3_finalize(stmt);
    return n;
}

Approval *approval_resolve(sqlite3 *db, int64_t id, int approved, const char *decided_via) {
    const char *new_state = approved ? "approved" : "denied";
    const char *sql =
        "UPDATE approvals SET state=?, decided_via=? WHERE id=? AND state='pending'";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(stmt, 1, new_state, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, decided_via, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE || sqlite3_changes(db) != 1)
        return NULL;
    LOG_INFO_("approval resolve id=%lld decision=%s",
             (long long)id, new_state);

    /* Return the resolved row */
    const char *sel =
        "SELECT id, session_id, tool_call_id, tool_name, action,"
        " args_json, resolve, state, decided_via, requested_at, expires_at"
        " FROM approvals WHERE id=?";
    if (sqlite3_prepare_v2(db, sel, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(stmt, 1, id);
    Approval *a = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        a = row_to_approval(stmt);
    sqlite3_finalize(stmt);
    return a;
}



static int64_t *drain_ids(sqlite3_stmt *stmt, int *out_count) {
    int cap = 0, n = 0;
    int64_t *ids = NULL;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (n >= cap) {
            cap = cap ? cap * 2 : 8;
            int64_t *tmp = realloc(ids, (size_t)cap * sizeof(*ids));
            if (!tmp) {
                free(ids);
                sqlite3_finalize(stmt);
                *out_count = 0;
                return NULL;
            }
            ids = tmp;
        }
        ids[n++] = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    *out_count = n;
    return ids;
}

/* Owner filter shared by the sweeps: the approval's session is owned by `me`,
 * unowned, or dead-owned (owner not in the live registry). With `me` bound NULL
 * the `= ?me` arm is never true, reducing to "unowned or dead-owned". */
static void bind_me(sqlite3_stmt *stmt, int idx, const char *me) {
    if (me && me[0]) sqlite3_bind_text(stmt, idx, me, -1, SQLITE_STATIC);
    else sqlite3_bind_null(stmt, idx);
}

int64_t *approval_list_expired(sqlite3 *db, const char *me, int *out_count) {
    *out_count = 0;
    const char *sql =
        "SELECT a.id FROM approvals a JOIN sessions s ON s.id = a.session_id"
        " WHERE a.state='pending'"
        "   AND a.expires_at IS NOT NULL AND a.expires_at < unixepoch()"
        "   AND (s.owner_instance IS NULL OR s.owner_instance = ?1"
        "        OR s.owner_instance NOT IN (SELECT instance_id FROM processes));";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    bind_me(stmt, 1, me);
    return drain_ids(stmt, out_count);
}

int64_t *approval_list_block_due(sqlite3 *db, int block_sec, const char *me, int *out_count) {
    *out_count = 0;
    const char *sql =
        "SELECT a.id FROM approvals a JOIN sessions s ON s.id = a.session_id"
        " WHERE a.state='pending' AND s.state='awaiting_approval'"
        "   AND a.requested_at + ?1 < unixepoch()"
        "   AND (s.owner_instance IS NULL OR s.owner_instance = ?2"
        "        OR s.owner_instance NOT IN (SELECT instance_id FROM processes));";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int(stmt, 1, block_sec);
    bind_me(stmt, 2, me);
    return drain_ids(stmt, out_count);
}

int64_t approval_deliver_postwindow(sqlite3 *db, const Approval *a, ApprovalPostWindow outcome) {
    if (!a) return -1;
    const char *what = a->action ? a->action : (a->tool_name ? a->tool_name : "tool");
    /* Denied/expired messages carry the next-step norm — the moment the model
     * reads the outcome is the moment the norm matters, and the skill that
     * documents it may never be loaded. */
    char buf[512];
    switch (outcome) {
    case APPROVAL_PW_RERUN_APPROVED:
        /* Sensitivity approvals are per-call (trust.md rule 1): the frozen
         * call was already answered, so nothing transfers — say so instead of
         * implying a silent retry. Everything else leaves a ticket the gate
         * transfers to the next matching call (approval_take_ticket). */
        if (a->action && strcmp(a->action, "sensitive") == 0)
            snprintf(buf, sizeof(buf),
                     "Approval #%lld for '%s' was approved after the wait "
                     "window, but a sensitivity approval covers only that one "
                     "frozen call, which was already answered — re-issue the "
                     "call if still needed and it will ask the operator again.",
                     (long long)a->id, what);
        else
            snprintf(buf, sizeof(buf),
                     "Approval #%lld for '%s' was approved — re-issue the call "
                     "now if it's still needed; the approval covers the next "
                     "matching call, so it will run without asking again.",
                     (long long)a->id, what);
        break;
    case APPROVAL_PW_RERUN_DENIED:
        snprintf(buf, sizeof(buf),
                 "Approval #%lld for '%s' was denied. Don't re-request the "
                 "same thing — adjust your approach, or explain what you were "
                 "trying to do and let the operator decide.",
                 (long long)a->id, what);
        break;
    case APPROVAL_PW_APPLY_GRANTED:
        snprintf(buf, sizeof(buf), "Approval #%lld granted: %s applied.",
                 (long long)a->id, what);
        break;
    case APPROVAL_PW_APPLY_DENIED:
        snprintf(buf, sizeof(buf),
                 "Approval #%lld denied: %s. Don't re-request the same change "
                 "— adjust, or explain what you were trying to do.",
                 (long long)a->id, what);
        break;
    case APPROVAL_PW_EXPIRED:
    default:
        snprintf(buf, sizeof(buf),
                 "Approval #%lld for '%s' expired without a decision — the "
                 "operator may not have seen it. Not a denial — re-request "
                 "only if the task still needs it, and mention that it "
                 "expired.",
                 (long long)a->id, what);
        break;
    }
    return inbox_insert(db, a->session_id, "approval", NULL, buf);
}

/* Append s, breaking any run of 3+ backticks with a space after each pair.
 * Inside a fenced block nothing can be escaped — the only way to keep a
 * crafted ``` in model-authored text from closing our fence (and forging
 * trailing prompt content, e.g. a fake "Decide here" line) is to never emit
 * one. A space is visible, so the approver can see the content was altered
 * (R1, plan/projects/approval-summary-two-axis.md). */
static void append_fence_escaped(Buf *out, const char *s) {
    int run = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '`') {
            if (run == 2) { buf_append_char(out, ' '); run = 0; }
            run++;
        } else {
            run = 0;
        }
        buf_append_char(out, *p);
    }
}

/* Append `header` plus a fenced block of the rows yielded by sql (single
 * text column, one text bind) — or nothing when the query yields no rows.
 * The fence renders as monospace <pre> on chat channels so columns stay
 * aligned; in a terminal it reads fine as-is. Values are model-authored,
 * so they get the fence escape. */
static void append_enum_block(sqlite3 *db, Buf *out, const char *header,
                              const char *sql, const char *bind) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, bind, -1, SQLITE_STATIC);
    int n = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (!v) continue;
        if (n++ == 0) buf_appendf(out, "%s\n```\n", header);
        append_fence_escaped(out, v);
        buf_append_char(out, '\n');
    }
    if (n) buf_append_str(out, "```\n");
    sqlite3_finalize(st);
}

/* request_changes enumerations, grouped by scope: the approver must see
 * whether a line changes only the asking agent or the whole system. Every
 * requested VALUE is listed (which hosts, which paths) — never counts. */
static const char *SQL_CHANGES_AGENT_SCOPED =
    "SELECT format('%-7s%s','tool',atom) FROM json_each(?1,'$.changes.grants.tools')"
    " UNION ALL SELECT format('%-7s%s','host',atom) FROM json_each(?1,'$.changes.grants.hosts')"
    " UNION ALL SELECT format('%-7s%s','read',atom) FROM json_each(?1,'$.changes.grants.read_paths')"
    " UNION ALL SELECT format('%-7s%s','write',atom) FROM json_each(?1,'$.changes.grants.write_paths')"
    " UNION ALL SELECT format('%-7s%s','route',atom) FROM json_each(?1,'$.changes.routes')"
    " UNION ALL SELECT format('%s = %s',key,atom) FROM json_each(?1,'$.changes.agent')";

static const char *SQL_CHANGES_SYSTEM_WIDE =
    "SELECT format('%s = %s',key,atom) FROM json_each(?1,'$.changes.config')"
    /* One line per (secret, host) pair — the pair IS the decision: after
     * approval this credential may be submitted to that host, durably. */
    " UNION ALL SELECT format('secret %s -> %s', s.key, h.atom)"
    "   FROM json_each(?1,'$.changes.secret_bindings') s, json_each(s.value) h"
    /* All four keys validate_provider accepts, model included: the model is
     * what actually answers after the swap, and it is upserted from this
     * document — a prompt naming only the endpoint and the paying secret
     * describes the plumbing and omits the substance. */
    " UNION ALL SELECT format('provider %s -> %s, model %s (key from secret %s)',"
    "   json_extract(?1,'$.changes.provider.provider'),"
    "   json_extract(?1,'$.changes.provider.base_url'),"
    "   COALESCE(json_extract(?1,'$.changes.provider.model'),"
    "            '(provider default)'),"
    "   json_extract(?1,'$.changes.provider.api_key_env'))"
    " WHERE json_extract(?1,'$.changes.provider.provider') IS NOT NULL";

/* create_agent enumeration over the parked definition — values, not counts:
 * this is the approval that mints a new autonomous actor, so it must be the
 * most transparent one, not the least.
 *
 * The hand-listed arms cover the structured keys; the scalar tail at the end
 * covers everything else apply_fields_and_grants writes (system_prompt,
 * description, secondary_model, max_iterations, shell_timeout), so a field
 * agent_define learns to apply cannot go unrendered by omission. Keep the
 * tail's NOT IN list in step with the headline — those keys are already on
 * the line above this block. */
static const char *SQL_CREATE_AGENT_LINES =
    "SELECT format('%-7s%s','tool',atom) FROM json_each(?1,'$.grants.tools')"
    " UNION ALL SELECT format('%-7s%s','host',atom) FROM json_each(?1,'$.grants.hosts')"
    " UNION ALL SELECT format('%-7s%s','read',atom) FROM json_each(?1,'$.grants.read_paths')"
    " UNION ALL SELECT format('%-7s%s','write',atom) FROM json_each(?1,'$.grants.write_paths')"
    " UNION ALL SELECT format('%-7s%s','ext',atom) FROM json_each(?1,'$.extensions')"
    " UNION ALL SELECT format('%-7s%s','model',json_extract(?1,'$.primary_model'))"
    "   WHERE json_extract(?1,'$.primary_model') IS NOT NULL"
    /* Block contents, not a count: a seeded memory block is standing context
     * the new agent boots with — indistinguishable in effect from part of its
     * system prompt, and 'N block(s)' says nothing about what it carries. */
    " UNION ALL SELECT format('%-7s%s = %s','memory',"
    "     COALESCE(json_extract(value,'$.label'),'(unlabeled)'),"
    "     CASE WHEN length(json_extract(value,'$.value')) > 200"
    "          THEN substr(json_extract(value,'$.value'),1,200)||'...'"
    "          ELSE COALESCE(json_extract(value,'$.value'),'(empty)') END)"
    "   FROM json_each(?1,'$.memory_blocks')"
    " UNION ALL SELECT format('%s = %s', key,"
    "     CASE WHEN length(atom) > 200 THEN substr(atom,1,200)||'...' ELSE atom END)"
    "   FROM json_each(?1)"
    "   WHERE type NOT IN ('object','array')"
    "     AND key NOT IN ('name','sandbox_profile','clone_from','primary_model')";

/* update_agent enumeration — grants add, scalar fields overwrite; long text
 * (system_prompt) is clipped per line, the whole-summary truncation below is
 * the backstop. */
static const char *SQL_UPDATE_AGENT_LINES =
    "SELECT format('%-7s%s','tool',atom) FROM json_each(?1,'$.grants.tools')"
    " UNION ALL SELECT format('%-7s%s','host',atom) FROM json_each(?1,'$.grants.hosts')"
    " UNION ALL SELECT format('%-7s%s','read',atom) FROM json_each(?1,'$.grants.read_paths')"
    " UNION ALL SELECT format('%-7s%s','write',atom) FROM json_each(?1,'$.grants.write_paths')"
    " UNION ALL SELECT format('%s = %s', key,"
    "     CASE WHEN length(atom) > 200 THEN substr(atom,1,200)||'...' ELSE atom END)"
    "   FROM json_each(?1)"
    "   WHERE key NOT IN ('name','grants','reason') AND type!='object'";

/* cron_set enumeration — every field the job was asked to carry. The
 * escalation being approved is *which agent* fires it, so the agent and the
 * chat it would report to must both be visible, never summarized away. */
static const char *SQL_CRON_SET_LINES =
    "SELECT format('%-13s%s',key,atom) FROM json_each(?1) WHERE key!='name'";

/* Generic argument enumeration for every tool without a bespoke branch —
 * all scalar args, per-value clipped like SQL_UPDATE_AGENT_LINES; the
 * whole-summary truncation is the backstop. Objects/arrays are skipped,
 * not flattened: a tool whose nested payload matters that much deserves a
 * bespoke branch. Declared salience (a summary_fields list) was considered
 * and deferred — D10. */
static const char *SQL_ARGS_LINES =
    "SELECT format('%s: %s', key,"
    "     CASE WHEN length(atom) > 200 THEN substr(atom,1,200)||'...' ELSE atom END)"
    " FROM json_each(?1)"
    " WHERE type NOT IN ('object','array') AND key NOT IN ('code','command')";

/* Axis-2 overlay for a sensitivity park: re-derive the matched target the
 * same way the gate did (raw-args scan, then the decoded url host second
 * chance) — the approval row records only the reason, and the label list has
 * one home (sensitive_targets), so there is no cached match to drift. */
static void overlay_sensitive(sqlite3 *db, Buf *out, const char *args) {
    char host[254] = "";
    int n = 0;
    char **sens = db_sensitive_hosts(db, &n);
    if (sens) {
        if (!host_in_text(sens, (size_t)n, args, host, sizeof(host))) {
            char uh[254];
            if (web_args_url_host(db, args, uh, sizeof(uh))) {
                url_host_normalize(uh, sizeof(uh));
                if (host_covered(sens, (size_t)n, uh))
                    snprintf(host, sizeof(host), "%s", uh);
            }
        }
        for (int i = 0; i < n; i++) free(sens[i]);
        free(sens);
    }
    buf_appendf(out, "Sensitive target: %s\n"
                     "No standing grant covers a sensitive target — 'always' "
                     "still passes only this one call.\n",
                host[0] ? host : "(no label matches now — list changed since park?)");
}

/* The full command/code the frozen call would run, fenced, LAST in the
 * summary: model-authored bulk is the untrusted part, so the bottom-up
 * truncation backstop eats it before the DB-derived facts above it (R1).
 *
 * Both keys render when both are present: SQL_ARGS_LINES excludes the pair
 * unconditionally, so first-match-wins would drop the loser from the summary
 * entirely — invisible on a self-augmented tool, whose param names are
 * whatever its manifest author chose. */
static void append_body_block(sqlite3 *db, Buf *out, const char *args) {
    static const struct { const char *key, *label; } bodies[] = {
        { "command", "Command:" }, { "code", "Code:" },
    };
    for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); i++) {
        char *body = tool_args_str(db, args, bodies[i].key);
        if (!body) continue;
        buf_appendf(out, "%s\n```\n", bodies[i].label);
        append_fence_escaped(out, body);
        buf_append_str(out, "\n```\n");
        free(body);
    }
}

/* Render a human-readable markdown summary of an approval — never the raw
 * args_json blob. Grant-shaped approvals enumerate every requested value in
 * fenced blocks grouped by scope; any other tool's call arguments (shape
 * unknown in general) surface their most common salient field. Returns a
 * heap string, caller frees. */
char *approval_format_summary(sqlite3 *db, const Approval *a) {
    const char *args = a->args_json ? a->args_json : "{}";
    Buf out = {0};
    /* Collected extracted fields — freed in one sweep at the end. */
    char *f[3] = {0};
    if (a->tool_name && strcmp(a->tool_name, "request_config") == 0 &&
        a->action && strcmp(a->action, "request_changes") == 0) {
        buf_append_str(&out, "requests these changes:\n");
        append_enum_block(db, &out, "Agent-scoped (this agent only):",
                          SQL_CHANGES_AGENT_SCOPED, args);
        append_enum_block(db, &out, "System-wide:", SQL_CHANGES_SYSTEM_WIDE, args);
        f[0] = tool_args_str(db, args, "reason");
        if (f[0] && f[0][0]) buf_appendf(&out, "Reason: %s\n", f[0]);
    } else if (a->tool_name && strcmp(a->tool_name, "request_config") == 0 &&
               a->action && strcmp(a->action, "rename_agent") == 0) {
        f[0] = tool_args_str(db, args, "name");
        f[1] = tool_args_str(db, args, "reason");
        buf_appendf(&out, "rename_agent: %s", f[0] ? f[0] : "?");
        if (f[1] && f[1][0]) buf_appendf(&out, "\nReason: %s", f[1]);
    } else if (a->tool_name && strcmp(a->tool_name, "request_config") == 0) {
        buf_append_str(&out, a->action ? a->action : "?");
    } else if (a->tool_name && strcmp(a->tool_name, "extension_promote") == 0) {
        f[0] = tool_args_str(db, args, "name");
        f[1] = tool_args_str(db, args, "summary");
        buf_appendf(&out, "promote '%s': %s", f[0] ? f[0] : "?",
                    f[1] ? f[1] : "(no summary)");
    } else if (a->tool_name && strcmp(a->tool_name, "create_agent") == 0) {
        f[0] = tool_args_str(db, args, "name");
        f[1] = tool_args_str(db, args, "sandbox_profile");
        f[2] = tool_args_str(db, args, "clone_from");
        buf_appendf(&out, "wants to create agent **%s** (profile %s%s%s):\n",
                    f[0] ? f[0] : "?", f[1] ? f[1] : "default",
                    f[2] ? ", clone of " : "", f[2] ? f[2] : "");
        append_enum_block(db, &out, "Capabilities (all within the creator's own):",
                          SQL_CREATE_AGENT_LINES, args);
    } else if (a->tool_name && strcmp(a->tool_name, "update_agent") == 0) {
        f[0] = tool_args_str(db, args, "name");
        f[1] = tool_args_str(db, args, "reason");
        buf_appendf(&out, "wants to update agent **%s**:\n", f[0] ? f[0] : "?");
        append_enum_block(db, &out, "Changes (grants add; fields overwrite):",
                          SQL_UPDATE_AGENT_LINES, args);
        if (f[1] && f[1][0]) buf_appendf(&out, "Reason: %s\n", f[1]);
    } else if (a->tool_name && strcmp(a->tool_name, "cron_set") == 0) {
        f[0] = tool_args_str(db, args, "name");
        buf_appendf(&out, "wants to schedule job **%s**, running as another "
                          "agent (its grants and model, every fire):\n",
                    f[0] ? f[0] : "?");
        append_enum_block(db, &out, "Job:", SQL_CRON_SET_LINES, args);
    } else {
        /* Generic tail — two-axis: the tool + its args (axis 1), and why the
         * gate parked it (axis 2, rendered as an overlay). No guessed salient
         * field: every scalar arg is enumerated, so a tool the probes never
         * anticipated (js_eval's `code`) can't degrade to a blind
         * "tool (action)" line. Ordering is load-bearing (R1): DB-derived
         * facts first, model-authored args and code last, so the bottom-up
         * truncation backstop eats the code before the facts. */
        const char *tn = a->tool_name ? a->tool_name : "?";
        int is_sens = a->action && strcmp(a->action, "sensitive") == 0;
        if (is_sens)
            buf_appendf(&out, "%s — parked: targets a sensitive-labeled host\n", tn);
        else if (a->action && strcmp(a->action, tn) != 0)
            buf_appendf(&out, "%s (%s)\n", tn, a->action);
        else
            buf_appendf(&out, "%s\n", tn);
        if (is_sens) overlay_sensitive(db, &out, args);
        append_enum_block(db, &out, "Args:", SQL_ARGS_LINES, args);
        append_body_block(db, &out, args);
    }
    for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++) free(f[i]);

    char *s = buf_take(&out);
    if (!s) return strdup("?");
    /* Telegram counts parsed text against its 4096 limit and buttoned
     * prompts are never chunked — truncate a huge document at a line break.
     * The appended fence closes a cut-open block; if the cut landed outside
     * a block the stray fence stays literal (unbalanced markers pass
     * through the renderer untouched), which is safe. */
    if (strlen(s) > 3500) {
        size_t cut = 3400;
        while (cut > 0 && s[cut] != '\n') cut--;
        if (cut == 0) {
            /* No line break at all (e.g. one huge single-line field) — hard
             * cut rather than emptying the summary; keep UTF-8 sequences
             * whole so the platform doesn't reject the text. */
            cut = 3400;
            while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) cut--;
        }
        s[cut] = '\0';
        Buf t = {0};
        buf_appendf(&t, "%s\n```\n... (truncated)", s);
        free(s);
        s = buf_take(&t);
        if (!s) return strdup("?");
    }
    return s;
}
