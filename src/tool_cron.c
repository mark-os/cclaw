#define _POSIX_C_SOURCE 200809L
#include "tool_cron.h"
#include "approval.h"
#include "config_registry.h"
#include "cron.h"
#include "db.h"
#include "tool_args.h"
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* One JSON object, every field individually optional: validity lives in the
 * cross-field rules below, not in a `required` list. Same-name = upsert. */
static const char *CRON_SET_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\",\"description\":\"Job name — its identity. Setting a name that already exists updates that job in place; fields you leave out keep their current value. Omit to have a name generated for you.\"},"
    "\"cron_expr\":{\"type\":\"string\",\"description\":\"5-field cron expression (M H D Mo DoW) for a recurring job, evaluated in the daemon's local timezone (so it follows DST) — the current local time and zone are in your context block. At most one of cron_expr / in_seconds.\"},"
    "\"in_seconds\":{\"type\":\"integer\",\"description\":\"Delay in seconds from now for a one-shot job, which fires once and is then removed. At most one of cron_expr / in_seconds.\"},"
    "\"prompt\":{\"type\":\"string\",\"description\":\"Message delivered when the job fires; it starts a turn. Pass \\\"\\\" to clear it.\"},"
    "\"script\":{\"type\":\"string\",\"description\":\"Workspace-relative path to a .qjs file (it must already exist), run sandboxed at fire time with its output posted to the session — no LLM turn unless prompt is set too. Pass \\\"\\\" to clear it.\"},"
    "\"session_id\":{\"type\":\"integer\",\"description\":\"Pin every fire to one existing session. Expert form — by default a fire follows this conversation.\"},"
    "\"session\":{\"type\":\"string\",\"enum\":[\"new\"],\"description\":\"\\\"new\\\" gives each fire a fresh session (fresh context). Required before agent / channel_name / chat_id.\"},"
    "\"agent\":{\"type\":\"string\",\"description\":\"Agent the fire runs as; session:\\\"new\\\" only, default yourself. Naming another agent needs human approval.\"},"
    "\"channel_name\":{\"type\":\"string\",\"description\":\"Deliver the fresh session's result to this chat; session:\\\"new\\\" only, and pass chat_id with it.\"},"
    "\"chat_id\":{\"type\":\"string\",\"description\":\"Chat id, paired with channel_name.\"}"
    "}}";

static const char *CRON_LIST_PARAMS =
    "{\"type\":\"object\",\"properties\":{}}";

static const char *CRON_REMOVE_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"id\":{\"type\":\"integer\",\"description\":\"Job id, as shown by cron_list\"}"
    "},\"required\":[\"id\"]}";

/* Every floor refusal ends with this: the behaviour the floor exists to stop
 * is an agent scheduling pokes at work that already reports back by itself. */
#define FLOOR_REDIRECT \
    " If you are waiting on a sub-agent, do not schedule checks — its result " \
    "will arrive on its own."

__attribute__((format(printf, 1, 2)))
static char *errf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return strdup(buf);
}

/* Number of jobs a session currently owns (per-session cap check). */
static int session_job_count(sqlite3 *db, int64_t session_id) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM cron_jobs WHERE session_id=?",
                           -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(st, 1, session_id);
    int n = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : -1;
    sqlite3_finalize(st);
    return n;
}

static int job_exists(sqlite3 *db, const char *agent, const char *name) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM cron_jobs WHERE agent_name=?1 AND name=?2",
            -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, agent, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, name, -1, SQLITE_STATIC);
    int found = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return found;
}

/* ── validation ──────────────────────────────────────────────────────── */

/* Typo-hostile: an unknown key is an error, never a silently ignored field. */
static char *check_keys(sqlite3 *db, const char *args) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT key FROM json_each(?1) WHERE key NOT IN"
            " ('name','cron_expr','in_seconds','prompt','script',"
            "  'session_id','session','agent','channel_name','chat_id')"
            " LIMIT 1", -1, &st, NULL) != SQLITE_OK)
        return errf("error: could not read the arguments");
    sqlite3_bind_text(st, 1, args, -1, SQLITE_STATIC);
    char *bad = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *k = (const char *)sqlite3_column_text(st, 0);
        bad = errf("error: unknown field '%s' (use name, cron_expr, in_seconds, "
                   "prompt, script, session_id, session, agent, channel_name, "
                   "chat_id)", k ? k : "?");
    }
    sqlite3_finalize(st);
    return bad;
}

/* Rules that need only this one call's fields. */
static char *check_call(const CronDoc *d) {
    if (d->has_expr && d->has_seconds)
        return errf("error: give at most one of cron_expr (recurring) or "
                      "in_seconds (one-shot), not both");
    if (d->has_seconds && d->in_seconds <= 0)
        return errf("error: in_seconds=%lld — it must be a positive delay in "
                    "seconds from now", (long long)d->in_seconds);
    if (d->has_session && (!d->session || strcmp(d->session, "new") != 0))
        return errf("error: session accepts only \"new\" (a fresh session per "
                    "fire); to target an existing session use session_id");
    if (d->has_session && d->has_session_id)
        return errf("error: give at most one of session:\"new\" (fresh "
                      "session per fire) or session_id (pin one session)");
    if (d->has_channel != d->has_chat)
        return errf("error: channel_name and chat_id go together — give both "
                      "or neither");
    if (d->name && strlen(d->name) > 64)
        return errf("error: name must be 64 characters or fewer");
    return NULL;
}

/* Schedule refusals are split so the model learns which mistake it made, and
 * the floor refusal states the floor, echoes the offending value, and points
 * at the thing it should have waited for instead. */
static char *check_schedule(sqlite3 *db, const CronDoc *d) {
    if (!d->has_expr && !d->has_seconds) return NULL;   /* upsert may keep one */
    int64_t every = 0;
    CronSchedRc rc = cron_schedule_check(db, d->has_expr ? d->cron_expr : NULL,
                                         d->has_seconds ? d->in_seconds : 0,
                                         NULL, &every);
    int64_t floor = config_get_int(db, "cron_min_interval_seconds");
    switch (rc) {
    case CRON_SCHED_BAD_EXPR:
        return errf("error: cron_expr '%s' is not a 5-field cron expression "
                    "(minute hour day month weekday) — e.g. '0 9 * * *' is "
                    "9am daily, '*/10 * * * *' every ten minutes",
                    d->cron_expr ? d->cron_expr : "");
    case CRON_SCHED_FLOOR:
        if (d->has_seconds)
            return errf("error: schedule below the floor (in_seconds=%lld, "
                        "floor=%llds — config cron_min_interval_seconds)."
                        FLOOR_REDIRECT,
                        (long long)d->in_seconds, (long long)floor);
        return errf("error: schedule below the floor (cron_expr '%s' fires "
                    "every %llds, floor=%llds — config "
                    "cron_min_interval_seconds)." FLOOR_REDIRECT,
                    d->cron_expr ? d->cron_expr : "", (long long)every,
                    (long long)floor);
    default:
        return NULL;
    }
}

/* A pin names a session that has to exist now. It may still vanish before
 * the fire — that is the pin's accepted risk — but a typo is not. */
static char *check_session_pin(const ToolCronCtx *ctx, const CronDoc *d) {
    if (!d->has_session_id) return NULL;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(ctx->db, "SELECT 1 FROM sessions WHERE id=?1",
                           -1, &st, NULL) != SQLITE_OK)
        return errf("error: could not check the session");
    sqlite3_bind_int64(st, 1, d->session_id);
    int found = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    if (found) return NULL;
    return errf("error: session %lld does not exist — omit session_id to let "
                "the job follow this conversation",
                (long long)d->session_id);
}

/* Script paths are workspace-relative and must resolve to a file that exists
 * now: a job whose script is missing would fail unattended, hours later. */
static char *check_script(const ToolCronCtx *ctx, const CronDoc *d) {
    if (!d->has_script || !d->script || !d->script[0]) return NULL;  /* "" clears */
    if (d->script[0] == '/')
        return errf("error: script '%s' must be a workspace-relative path, not "
                    "an absolute one", d->script);
    if (strstr(d->script, ".."))
        return errf("error: script '%s' must stay inside your workspace (no "
                    "'..' path segments)", d->script);
    size_t sl = strlen(d->script);
    if (sl < 5 || strcmp(d->script + sl - 4, ".qjs") != 0)
        return errf("error: script '%s' must end in .qjs — the JS tier refuses "
                    "anything else at fire time", d->script);
    if (!ctx->workspace || !ctx->workspace[0])
        return errf("error: no workspace configured — script jobs need one");

    char full[PATH_MAX];
    int n = snprintf(full, sizeof(full), "%s/%s", ctx->workspace, d->script);
    if (n <= 0 || (size_t)n >= sizeof(full))
        return errf("error: script path is too long");
    struct stat sb;
    if (stat(full, &sb) != 0 || !S_ISREG(sb.st_mode))
        return errf("error: script file '%s' does not exist — write it first, "
                    "then schedule it", full);
    return NULL;
}

/* An explicit chat target must already route to the agent that will run the
 * fire; the fire re-checks, but a job that can never deliver is set-time
 * garbage. Mirrors channel_send's allowlist (routes are the authority). */
static char *check_channel(const ToolCronCtx *ctx, const CronDoc *d) {
    if (!d->has_channel || !d->channel_name || !d->chat_id) return NULL;
    const char *target = (d->has_agent && d->agent && d->agent[0])
                       ? d->agent : ctx->agent_name;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(ctx->db,
            "SELECT 1 FROM channel_routes r JOIN sessions s ON s.id=r.session_id"
            " WHERE r.channel_name=?1 AND r.chat_id=?2 AND s.agent_name=?3",
            -1, &st, NULL) != SQLITE_OK)
        return errf("error: could not check channel routes");
    sqlite3_bind_text(st, 1, d->channel_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, d->chat_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, target, -1, SQLITE_STATIC);
    int ok = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    if (ok) return NULL;
    return errf("error: no route sends (%s, %s) to agent %s — an operator route "
                "(cclaw route add) is what authorizes that chat",
                d->channel_name, d->chat_id, target);
}

static char *check_cap(const ToolCronCtx *ctx, const char *name) {
    int cap = config_get_int(ctx->db, "cron_max_jobs_per_session");
    if (cap <= 0) return NULL;
    /* Replacing a job the session already owns is not a new job. */
    if (job_exists(ctx->db, ctx->agent_name, name)) return NULL;
    int n = session_job_count(ctx->db, ctx->session_id);
    if (n < 0 || n < cap) return NULL;
    return errf("error: session cron job limit reached (max %d) — remove one "
                "with cron_remove, or reuse a job's name to replace it", cap);
}

/* ── document assembly ───────────────────────────────────────────────── */

/* Short, collision-checked job name. Names are the logical identity, so an
 * accidental collision would silently overwrite somebody else's job. */
static char *generate_name(sqlite3 *db, const char *agent) {
    for (int attempt = 0; attempt < 8; attempt++) {
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db, "SELECT 'job-' || lower(hex(randomblob(3)))",
                               -1, &st, NULL) != SQLITE_OK) return NULL;
        char *name = NULL;
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(st, 0);
            if (v) name = strdup(v);
        }
        sqlite3_finalize(st);
        if (!name) return NULL;
        if (!job_exists(db, agent, name)) return name;
        free(name);
    }
    return NULL;
}

/* The canonical document is the call's own arguments with `name` pinned — so
 * a parked approval carries exactly what the model asked for, plus the name
 * the result already told it. */
static char *doc_with_name(sqlite3 *db, const char *args, const char *name) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "SELECT json_set(json(?1),'$.name',?2)",
                           -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(st, 1, args, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, name, -1, SQLITE_STATIC);
    char *doc = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) doc = strdup(v);
    }
    sqlite3_finalize(st);
    return doc;
}

/* The other agent this job would run as, or NULL when it stays with the
 * caller. Keyed on the target session's owner, not on which field named it. */
static char *other_agent(const ToolCronCtx *ctx, const CronDoc *d) {
    char *owner = NULL;
    if (d->has_agent && d->agent && d->agent[0]) {
        owner = strdup(d->agent);
    } else if (d->has_session_id) {
        owner = session_get_agent_name(ctx->db, d->session_id);
    }
    if (owner && strcmp(owner, ctx->agent_name) == 0) {
        free(owner);
        owner = NULL;
    }
    return owner;
}

/* ── result text ─────────────────────────────────────────────────────── */

static void describe_target(char *out, size_t cap, const char *target,
                            const char *tagent, const char *chan,
                            const char *chat, int64_t session_id) {
    if (target && strcmp(target, "pin") == 0) {
        snprintf(out, cap, "session %lld", (long long)session_id);
    } else if (target && strcmp(target, "new") == 0) {
        char who[96] = "", where[192] = "";
        if (tagent) snprintf(who, sizeof(who), " as %s", tagent);
        if (chan && chat) snprintf(where, sizeof(where), ", reporting to %s:%s",
                                   chan, chat);
        snprintf(out, cap, "a fresh session%s%s", who, where);
    } else if (chan && chat) {
        snprintf(out, cap, "this conversation (%s:%s)", chan, chat);
    } else {
        snprintf(out, cap, "this conversation");
    }
}

/* "created/updated <name> (id=N): <schedule>; payload <kinds>; fires in <target>" */
static char *describe_job(sqlite3 *db, int64_t id, int updated) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT name, cron_expr, COALESCE(run_at,0), next_run_at, task,"
            "       script, target, target_agent, channel_name, chat_id,"
            "       session_id FROM cron_jobs WHERE id=?1",
            -1, &st, NULL) != SQLITE_OK)
        return errf("%s cron job id=%lld", updated ? "updated" : "created",
                    (long long)id);
    sqlite3_bind_int64(st, 1, id);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(st, 0);
        const char *expr = (const char *)sqlite3_column_text(st, 1);
        int64_t run_at   = sqlite3_column_int64(st, 2);
        int64_t next     = sqlite3_column_int64(st, 3);
        const char *task = (const char *)sqlite3_column_text(st, 4);
        const char *scr  = (const char *)sqlite3_column_text(st, 5);
        const char *tgt  = (const char *)sqlite3_column_text(st, 6);
        const char *tag  = (const char *)sqlite3_column_text(st, 7);
        const char *chan = (const char *)sqlite3_column_text(st, 8);
        const char *chat = (const char *)sqlite3_column_text(st, 9);
        int64_t sid      = sqlite3_column_int64(st, 10);

        char when[48] = "";
        time_t t = (time_t)next;
        struct tm tm;
        if (next > 0 && localtime_r(&t, &tm))
            strftime(when, sizeof(when), "%Y-%m-%d %H:%M %Z", &tm);

        char sched[128];
        if (expr && expr[0])
            snprintf(sched, sizeof(sched), "recurring '%s', next fire %s",
                     expr, when);
        else if (run_at > 0)
            snprintf(sched, sizeof(sched), "one-shot, fires %s", when);
        else
            snprintf(sched, sizeof(sched), "next fire %s", when);

        int has_prompt = (task && task[0]), has_script = (scr && scr[0]);
        const char *payload = has_prompt && has_script ? "prompt + script"
                            : has_prompt ? "prompt"
                            : has_script ? "script"
                            : "bare wake (no prompt, no script)";
        char where[256];
        describe_target(where, sizeof(where), tgt, tag, chan, chat, sid);

        out = errf("%s cron job \"%s\" (id=%lld): %s; payload: %s; fires in %s",
                   updated ? "updated" : "created", name ? name : "?",
                   (long long)id, sched, payload, where);
    }
    sqlite3_finalize(st);
    return out ? out : errf("%s cron job id=%lld", updated ? "updated" : "created",
                            (long long)id);
}

/* ── cross-agent approval ────────────────────────────────────────────── */

/* A job that runs as another agent runs with that agent's grants and model —
 * an escalation, approved once as a standing job rather than per fire. */
static char *park_cross_agent(ToolCronCtx *ctx, const char *doc,
                              const char *name) {
    sqlite3_stmt *chk;
    if (sqlite3_prepare_v2(ctx->db,
            "SELECT 1 FROM approvals WHERE session_id=?1 AND tool_name='cron_set'"
            " AND state='pending' AND json_extract(args_json,'$.name')=?2",
            -1, &chk, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(chk, 1, ctx->session_id);
        sqlite3_bind_text(chk, 2, name, -1, SQLITE_STATIC);
        int dup = (sqlite3_step(chk) == SQLITE_ROW);
        sqlite3_finalize(chk);
        if (dup)
            return errf("error: this job was already sent for approval and is "
                          "still awaiting the user's yes/no reply — do not "
                          "re-request; wait");
    }

    int64_t aid = approval_create(ctx->db, ctx->session_id,
                                  ctx->current_tool_call_id, "cron_set",
                                  APPROVAL_PARK_REQUIRED, doc, "apply");
    if (aid < 0) return errf("error: failed to create approval");
    session_set_state(ctx->db, ctx->session_id, "awaiting_approval");
    return NULL; /* park */
}

/* ── handlers ────────────────────────────────────────────────────────── */

char *tool_cron_set_handler(const char *arguments, void *user_data, int *is_error) {
    ToolCronCtx *ctx = (ToolCronCtx *)user_data;
    if (!ctx || !ctx->db) return tool_fail(is_error, "error: cron_set unavailable");
    if (!tool_args_valid_object(ctx->db, arguments))
        return tool_fail(is_error, "error: arguments must be a JSON object");

    char *err = check_keys(ctx->db, arguments);
    if (err) { *is_error = 1; return err; }

    CronDoc d;
    if (cron_doc_parse(ctx->db, arguments, &d) != 0)
        return tool_fail(is_error, "error: could not read the arguments");

    char *name = NULL, *doc = NULL, *result = NULL, *cross = NULL;
    if ((err = check_call(&d)) ||
        (err = check_schedule(ctx->db, &d)) ||
        (err = check_session_pin(ctx, &d)) ||
        (err = check_script(ctx, &d)))
        goto done;

    name = (d.name && d.name[0]) ? strdup(d.name)
                                 : generate_name(ctx->db, ctx->agent_name);
    if (!name) { err = tool_fail(is_error, "error: could not name the job"); goto done; }

    if ((err = check_cap(ctx, name))) goto done;

    doc = doc_with_name(ctx->db, arguments, name);
    if (!doc) { err = tool_fail(is_error, "error: could not build the job"); goto done; }

    /* Merged-row rules (a partial upsert must still land on a valid job)
     * before the route check: "channel fields need session:new" is the more
     * useful thing to hear than "that chat does not route to you". */
    if (cron_doc_check(ctx->db, ctx->agent_name, doc, &err) != 0) goto done;
    if ((err = check_channel(ctx, &d))) goto done;

    cross = other_agent(ctx, &d);
    if (cross) {
        /* Parks and returns NULL; the job is written by the apply path. */
        result = park_cross_agent(ctx, doc, name);
        goto done;
    }

    int updated = 0;
    int64_t id = cron_upsert(ctx->db, ctx->agent_name, ctx->session_id, doc,
                             &updated, &err);
    if (id > 0) result = describe_job(ctx->db, id, updated);

done:
    /* Every `err` path above is a validation refusal — one explicit flag at
     * the single place they converge, so no failure depends on its wording. */
    if (err) { free(result); result = err; *is_error = 1; }
    free(cross); free(doc); free(name);
    cron_doc_free(&d);
    return result;
}

char *tool_cron_list_handler(const char *arguments, void *user_data, int *is_error) {
    (void)arguments;
    ToolCronCtx *ctx = (ToolCronCtx *)user_data;

    int count = 0;
    CronJob *jobs = cron_list(ctx->db, ctx->agent_name, &count);
    if (count == 0) return strdup("no cron jobs");

    size_t cap = 128 * (size_t)count + 64;
    char *buf = malloc(cap);
    if (!buf) { cron_list_free(jobs, count); return tool_fail(is_error, "error: OOM"); }
    size_t pos = 0;

    /* Header. A blank cron_expr with a future next_run_at reads as a one-shot;
     * both payload columns blank is a bare wake. */
    pos += (size_t)snprintf(buf + pos, cap - pos,
        "id|name|cron_expr|prompt|script|enabled|next_run_at\n");

    for (int i = 0; i < count; i++) {
        int n = snprintf(buf + pos, cap - pos, "%lld|%s|%s|%s|%s|%s|%lld\n",
            (long long)jobs[i].id,
            jobs[i].name ? jobs[i].name : "",
            jobs[i].cron_expr ? jobs[i].cron_expr : "",
            jobs[i].task ? jobs[i].task : "",
            jobs[i].script ? jobs[i].script : "",
            jobs[i].enabled ? "true" : "false",
            (long long)jobs[i].next_run_at);
        if (n > 0) pos += (size_t)n;
    }
    cron_list_free(jobs, count);
    return buf;
}

char *tool_cron_remove_handler(const char *arguments, void *user_data, int *is_error) {
    ToolCronCtx *ctx = (ToolCronCtx *)user_data;

    int id = tool_args_int(ctx->db, arguments, "id", -1);

    if (id < 0)
        return tool_fail(is_error, "error: missing required field 'id'");

    if (cron_remove(ctx->db, (int64_t)id, ctx->agent_name) != 0)
        return tool_fail(is_error, "error: job not found or DB error");

    char *result = malloc(64);
    if (!result) return tool_fail(is_error, "error: OOM");
    snprintf(result, 64, "removed cron job id=%d", id);
    return result;
}

/* EXEC_THREAD shims: rebuild ToolCronCtx around the thread's own db. cron_set
 * is not among them — it runs inline, because it can park an approval. */
static char *cron_list_thread_run(sqlite3 *db, const char *agent_name,
                                  int64_t session_id, const char *args,
                                  int *is_error) {
    ToolCronCtx c = {.db = db, .session_id = session_id};
    snprintf(c.agent_name, sizeof(c.agent_name), "%s", agent_name ? agent_name : "");
    return tool_cron_list_handler(args, &c, is_error);
}
static char *cron_remove_thread_run(sqlite3 *db, const char *agent_name,
                                    int64_t session_id, const char *args,
                                    int *is_error) {
    ToolCronCtx c = {.db = db, .session_id = session_id};
    snprintf(c.agent_name, sizeof(c.agent_name), "%s", agent_name ? agent_name : "");
    return tool_cron_remove_handler(args, &c, is_error);
}

int tool_cron_register(ToolRegistry *reg, ToolCronCtx *ctx) {
    if (tools_register(reg, "cron_set",
                       "Schedule work for later: recurring (cron_expr) or "
                       "one-shot (in_seconds). The payload is a prompt (a "
                       "message that starts a turn when it fires), a script (a "
                       "workspace JS file run sandboxed, its output posted "
                       "back), both, or neither (a bare wake). By default a "
                       "fire lands in this conversation; session:\"new\" gives "
                       "each fire a fresh session, optionally under another "
                       "agent or reporting to a chat. Reusing a job's name "
                       "updates that job in place. Schedule promises about the "
                       "future — \"remind me Friday\", \"check the feed at "
                       "9am\". Never schedule checks on sub-agents you have "
                       "launched: they report back on their own.",
                       CRON_SET_PARAMS, tool_cron_set_handler, ctx) != 0)
        return -1;
    if (tools_register(reg, "cron_list",
                       "List this agent's cron jobs: id, schedule, payload "
                       "(prompt and/or script — both blank is a bare wake), "
                       "and whether each is enabled. A job that auto-paused "
                       "after repeated failed fires shows enabled=false; "
                       "cron_set the same name to re-enable it.",
                       CRON_LIST_PARAMS, tool_cron_list_handler, ctx) != 0)
        return -1;
    if (tools_register(reg, "cron_remove",
                       "Delete one of this agent's cron jobs by id (the id "
                       "cron_list shows). To change a job instead of deleting "
                       "it, cron_set the same name.",
                       CRON_REMOVE_PARAMS, tool_cron_remove_handler, ctx) != 0)
        return -1;
    tools_set_recipe(reg, "cron_set",    (ToolRecipe){EXEC_INLINE, SBX_NONE, NULL});
    tools_set_recipe(reg, "cron_list",   (ToolRecipe){EXEC_THREAD, SBX_NONE, cron_list_thread_run});
    tools_set_recipe(reg, "cron_remove", (ToolRecipe){EXEC_THREAD, SBX_NONE, cron_remove_thread_run});
    return 0;
}
