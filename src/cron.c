#define _POSIX_C_SOURCE 200809L   /* localtime_r/mktime; timegm (GNU) is gone */
#include "cron.h"
#include "config_registry.h"
#include "db.h"
#include "log.h"
#include "tool_args.h"
#include "wake.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Generic pulse payload — no specific commitment. The heartbeat exists so an
 * agent is never permanently asleep; specific obligations are one-shot jobs. */
#define HEARTBEAT_PROMPT \
    "Read HEARTBEAT.md if present. Follow it. " \
    "If nothing needs attention, reply HEARTBEAT_OK."

/* Parse a single cron field. Supports: star, N, N-M, star/N, N-M/S, comma-separated.
 * Sets bits in out for values in [min, max]. Returns 0 on success. */
static int parse_field(const char *field, int min, int max, uint64_t *out) {
    *out = 0;
    const char *p = field;
    while (*p) {
        int start = min, end = max, step = 1;
        if (*p == '*') {
            p++;
            start = min; end = max;
        } else {
            /* Range-check the long before the int cast — on ERANGE strtol
             * clamps to LONG_MIN/MAX and a blind cast is truncation. */
            errno = 0;
            long v = strtol(p, (char **)&p, 10);
            if (errno == ERANGE || v < min || v > max) return -1;
            start = (int)v;
            if (*p == '-') {
                p++;
                errno = 0;
                v = strtol(p, (char **)&p, 10);
                if (errno == ERANGE || v < min || v > max) return -1;
                end = (int)v;
            } else {
                end = start;
            }
        }
        if (*p == '/') {
            p++;
            errno = 0;
            long v = strtol(p, (char **)&p, 10);
            if (errno == ERANGE || v <= 0 || v > max) return -1;
            step = (int)v;
        }
        for (int i = start; i <= end; i += step)
            *out |= (1ULL << i);
        if (*p == ',') p++;
        else if (*p != '\0') return -1;
    }
    return 0;
}

/* Local midnight of the date in tm, which may be out of range (mday 32,
 * mon 12) — mktime normalizes it. tm_isdst=-1 makes mktime re-resolve the
 * day's real UTC offset instead of trusting the one localtime_r left behind. */
static time_t local_midnight(struct tm *tm) {
    tm->tm_hour = 0;
    tm->tm_min = 0;
    tm->tm_sec = 0;
    tm->tm_isdst = -1;
    return mktime(tm);
}

int64_t cron_next_run(const char *cron_expr, int64_t after) {
    if (!cron_expr) return -1;

    /* Parse 5 fields: min hour dom month dow */
    char buf[128];
    strncpy(buf, cron_expr, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *fields[5];
    char *saveptr = NULL;
    char *tok = strtok_r(buf, " \t", &saveptr);
    for (int i = 0; i < 5; i++) {
        if (!tok) return -1;
        fields[i] = tok;
        tok = strtok_r(NULL, " \t", &saveptr);
    }

    uint64_t minutes, hours, doms, months, dows;
    if (parse_field(fields[0], 0, 59, &minutes) != 0) return -1;
    if (parse_field(fields[1], 0, 23, &hours) != 0) return -1;
    if (parse_field(fields[2], 1, 31, &doms) != 0) return -1;
    if (parse_field(fields[3], 1, 12, &months) != 0) return -1;
    if (parse_field(fields[4], 0, 6, &dows) != 0) return -1;

    /* Start from after+60, rounded to minute boundary. The int64→time_t
     * narrowing assumes 64-bit time_t — true on every target incl. armel
     * (Debian Bookworm ships 64-bit time_t on 32-bit ARM). */
    time_t t = (time_t)(after + 60);
    t -= t % 60;

    /* Search up to 366 days ahead */
    time_t limit = t + 366 * 24 * 3600;
    struct tm tm;

    /* Fields are matched in the daemon's LOCAL timezone (localtime_r/mktime,
     * never gmtime_r/timegm), so "0 9 * * *" stays 9am across a DST shift. */
    while (t < limit) {
        time_t prev = t;
        localtime_r(&t, &tm);
        if (!(months & (1ULL << (tm.tm_mon + 1)))) {
            tm.tm_mon++;
            tm.tm_mday = 1;
            t = local_midnight(&tm);
        } else if (!(doms & (1ULL << tm.tm_mday)) || !(dows & (1ULL << tm.tm_wday))) {
            /* Next local midnight, not t+86400: a DST day is 23 or 25 hours. */
            tm.tm_mday++;
            t = local_midnight(&tm);
        } else if (!(hours & (1ULL << tm.tm_hour))) {
            /* Top of the next hour. An hour is always 3600 elapsed seconds —
             * the local hour it lands on is re-derived at the loop top, so a
             * spring-forward gap (02:xx never happens) is skipped naturally. */
            t += 3600 - (tm.tm_min * 60);
        } else if (!(minutes & (1ULL << tm.tm_min))) {
            t += 60;
        } else {
            return (int64_t)t;
        }
        /* Every branch must move t forward; a stall would spin the daemon
         * here. Also catches mktime failure ((time_t)-1). */
        if (t <= prev) return -1;
    }
    return -1;
}

/* ── cron_set documents ───────────────────────────────────────────────
 * A document is the tool's JSON arguments. Presence, not value, is what an
 * upsert merges on, so every accessor below distinguishes "absent" from
 * "empty" — passing "" is how the model clears a payload. */

__attribute__((format(printf, 1, 2)))
static char *cerrf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return strdup(buf);
}

/* 1 iff the document names this key (JSON null reads as absent). */
static int doc_has(sqlite3 *db, const char *doc, const char *key) {
    char path[64];
    snprintf(path, sizeof(path), "$.%s", key);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "SELECT 1 WHERE json_type(?1,?2) IS NOT NULL",
                           -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, doc, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, path, -1, SQLITE_TRANSIENT);
    int found = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return found;
}

int cron_doc_parse(sqlite3 *db, const char *doc, CronDoc *out) {
    memset(out, 0, sizeof(*out));
    if (!db || !doc || !tool_args_valid_object(db, doc)) return -1;
    out->name         = tool_args_str(db, doc, "name");
    out->cron_expr    = tool_args_str(db, doc, "cron_expr");
    out->prompt       = tool_args_str(db, doc, "prompt");
    out->script       = tool_args_str(db, doc, "script");
    out->session      = tool_args_str(db, doc, "session");
    out->agent        = tool_args_str(db, doc, "agent");
    out->channel_name = tool_args_str(db, doc, "channel_name");
    out->chat_id      = tool_args_str(db, doc, "chat_id");
    out->in_seconds   = tool_args_int(db, doc, "in_seconds", 0);
    out->session_id   = tool_args_int(db, doc, "session_id", 0);
    out->has_expr       = doc_has(db, doc, "cron_expr");
    out->has_seconds    = doc_has(db, doc, "in_seconds");
    out->has_prompt     = doc_has(db, doc, "prompt");
    out->has_script     = doc_has(db, doc, "script");
    out->has_session    = doc_has(db, doc, "session");
    out->has_session_id = doc_has(db, doc, "session_id");
    out->has_agent      = doc_has(db, doc, "agent");
    out->has_channel    = doc_has(db, doc, "channel_name");
    out->has_chat       = doc_has(db, doc, "chat_id");
    return 0;
}

void cron_doc_free(CronDoc *d) {
    if (!d) return;
    free(d->name); free(d->cron_expr); free(d->prompt); free(d->script);
    free(d->session); free(d->agent); free(d->channel_name); free(d->chat_id);
    memset(d, 0, sizeof(*d));
}

CronSchedRc cron_schedule_check(sqlite3 *db, const char *cron_expr,
                                int64_t in_seconds, int64_t *next_out,
                                int64_t *every_out) {
    int has_expr = (cron_expr && cron_expr[0]);
    int has_secs = (in_seconds != 0);
    if (has_expr && has_secs) return CRON_SCHED_BOTH;
    if (!has_expr && !has_secs) return CRON_SCHED_NONE;

    int64_t now = (int64_t)time(NULL);
    int64_t floor = config_get_int(db, "cron_min_interval_seconds");
    if (has_expr) {
        int64_t next = cron_next_run(cron_expr, now);
        if (next < 0) return CRON_SCHED_BAD_EXPR;
        /* Floor measured fire-to-fire, not first-fire delta: cron_next_run
         * starts its search at after+60, so a first-fire delta is never a
         * meaningful rate signal (an hourly job created at :59 fires in ~60s
         * once, then hourly). Compare two consecutive fires. */
        int64_t n2 = cron_next_run(cron_expr, next);
        if (n2 < 0) return CRON_SCHED_BAD_EXPR;
        if (every_out) *every_out = n2 - next;
        if (n2 - next < floor) return CRON_SCHED_FLOOR;
        if (next_out) *next_out = next;
        return CRON_SCHED_OK;
    }
    if (every_out) *every_out = in_seconds;
    if (in_seconds < floor) return CRON_SCHED_FLOOR;
    if (next_out) *next_out = now + in_seconds;
    return CRON_SCHED_OK;
}

/* The stored job an upsert merges into. found=0 means "creating". */
typedef struct {
    int found;
    int64_t id, run_at, interval_s, session_id, next_run_at;
    char *expr, *task, *script, *target, *target_agent, *channel, *chat;
} CronRow;

static void row_free(CronRow *r) {
    free(r->expr); free(r->task); free(r->script);
    free(r->target); free(r->target_agent); free(r->channel); free(r->chat);
    memset(r, 0, sizeof(*r));
}

static char *col_dup(sqlite3_stmt *st, int i) {
    const char *v = (const char *)sqlite3_column_text(st, i);
    return v ? strdup(v) : NULL;
}

static void row_load(sqlite3 *db, const char *agent, const char *name,
                     CronRow *r) {
    memset(r, 0, sizeof(*r));
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT id, cron_expr, COALESCE(run_at,0), COALESCE(interval_s,0),"
            "       session_id, task, script, target, target_agent,"
            "       channel_name, chat_id, next_run_at"
            " FROM cron_jobs WHERE agent_name=?1 AND name=?2",
            -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, agent ? agent : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, name ? name : "", -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        r->found       = 1;
        r->id          = sqlite3_column_int64(st, 0);
        r->expr        = col_dup(st, 1);
        r->run_at      = sqlite3_column_int64(st, 2);
        r->interval_s  = sqlite3_column_int64(st, 3);
        r->session_id  = sqlite3_column_int64(st, 4);
        r->task        = col_dup(st, 5);
        r->script      = col_dup(st, 6);
        r->target      = col_dup(st, 7);
        r->target_agent= col_dup(st, 8);
        r->channel     = col_dup(st, 9);
        r->chat        = col_dup(st, 10);
        r->next_run_at = sqlite3_column_int64(st, 11);
    }
    sqlite3_finalize(st);
}

/* Target mode of the merged job: NULL (follow the conversation), "pin", or
 * "new". Refuses the combinations that would leave a job with two modes —
 * the check the upsert path owns, because only the stored row knows the
 * mode a partial document is landing on. */
static const char *merged_mode(const CronDoc *d, const CronRow *row,
                               char **err_out) {
    int was_pin = row->target && strcmp(row->target, "pin") == 0;
    int was_new = row->target && strcmp(row->target, "new") == 0;
    int wants_new = d->has_session;

    if (d->has_session_id && was_new) {
        *err_out = cerrf("error: job '%s' fires in a fresh session "
                         "(session:\"new\") — it cannot also pin session_id. "
                         "Remove the job and set it again to change modes.",
                         d->name ? d->name : "?");
        return NULL;
    }
    if (wants_new && was_pin) {
        *err_out = cerrf("error: job '%s' is pinned to session %lld — it "
                         "cannot also use session:\"new\". Remove the job and "
                         "set it again to change modes.",
                         d->name ? d->name : "?", (long long)row->session_id);
        return NULL;
    }
    if (d->has_session_id) return "pin";
    if (wants_new || was_new) return "new";
    if (was_pin) return "pin";
    return NULL;
}

/* Merged-row rules: a job needs a schedule, and agent/channel targeting is
 * meaningful only for a fresh-session job. */
static char *doc_check_merged(const CronDoc *d, const CronRow *row) {
    char *err = NULL;
    const char *mode = merged_mode(d, row, &err);
    if (err) return err;

    int has_schedule = d->has_expr || d->has_seconds ||
                       (row->found && ((row->expr && row->expr[0]) ||
                                       row->run_at > 0 || row->interval_s > 0));
    if (!has_schedule)
        return cerrf("error: this job has no schedule — give it cron_expr "
                     "(recurring) or in_seconds (one-shot)");

    int is_new = (mode && strcmp(mode, "new") == 0);
    if (d->has_agent && !is_new)
        return cerrf("error: 'agent' only applies to a fresh-session job — "
                     "pass session:\"new\" with it");
    if ((d->has_channel || d->has_chat) && !is_new)
        return cerrf("error: 'channel_name'/'chat_id' only apply to a "
                     "fresh-session job — pass session:\"new\" with them");
    return NULL;
}

int cron_doc_check(sqlite3 *db, const char *agent_name, const char *doc,
                   char **err_out) {
    CronDoc d;
    if (cron_doc_parse(db, doc, &d) != 0) {
        if (err_out) *err_out = strdup("error: cron job document is not valid JSON");
        return -1;
    }
    int rc = 0;
    if (!d.name || !d.name[0]) {
        if (err_out) *err_out = strdup("error: cron job needs a name");
        rc = -1;
    } else {
        CronRow row;
        row_load(db, agent_name, d.name, &row);
        char *err = doc_check_merged(&d, &row);
        row_free(&row);
        if (err) {
            if (err_out) *err_out = err; else free(err);
            rc = -1;
        }
    }
    cron_doc_free(&d);
    return rc;
}

/* The calling session's chat binding — the follow-the-conversation stamp.
 * Both stay NULL for CLI and sub-agent callers, which is fine: such a fire
 * falls back to the stamped session id. */
static void caller_chat(sqlite3 *db, int64_t sid, char **chan, char **chat) {
    *chan = NULL; *chat = NULL;
    if (sid <= 0) return;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT channel_name, chat_id FROM sessions WHERE id=?1",
            -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(st, 1, sid);
    if (sqlite3_step(st) == SQLITE_ROW) {
        *chan = col_dup(st, 0);
        *chat = col_dup(st, 1);
    }
    sqlite3_finalize(st);
}

/* One merged job, ready to write. Strings borrow from the document, the
 * stored row, or the caller stamp — all outlive the INSERT. */
typedef struct {
    const char *expr, *task, *script, *target, *target_agent, *chan, *chat;
    int64_t run_at, interval_s, next_run_at, session_id;
} CronMerged;

static int merge_schedule(sqlite3 *db, const CronDoc *d, const CronRow *row,
                          CronMerged *m, char **err_out) {
    if (!d->has_expr && !d->has_seconds) {   /* keep what the job has */
        m->expr        = row->expr ? row->expr : "";
        m->run_at      = row->run_at;
        m->interval_s  = row->interval_s;
        m->next_run_at = row->next_run_at;
        return 0;
    }
    int64_t next = 0;
    CronSchedRc rc = cron_schedule_check(db, d->has_expr ? d->cron_expr : NULL,
                                         d->has_seconds ? d->in_seconds : 0,
                                         &next, NULL);
    if (rc != CRON_SCHED_OK) {
        /* The tool phrases these precisely before it ever gets here; this is
         * the structural backstop (and the apply-after-approval path). */
        if (err_out) *err_out = strdup("error: invalid schedule");
        return -1;
    }
    m->expr        = d->has_expr ? d->cron_expr : "";
    m->run_at      = d->has_seconds ? next : 0;
    m->interval_s  = 0;
    m->next_run_at = next;
    return 0;
}

static void merge_target(const CronDoc *d, const CronRow *row, const char *mode,
                         int64_t anchor, const char *cc, const char *ct,
                         CronMerged *m) {
    m->target = mode;
    m->target_agent = NULL;
    m->chan = NULL;
    m->chat = NULL;
    m->session_id = anchor;

    if (mode && strcmp(mode, "pin") == 0) {
        m->session_id = d->has_session_id ? d->session_id : row->session_id;
        return;
    }
    if (mode && strcmp(mode, "new") == 0) {
        m->target_agent = d->has_agent ? d->agent : row->target_agent;
        if (m->target_agent && !m->target_agent[0]) m->target_agent = NULL;
        if (d->has_channel || d->has_chat) {
            m->chan = d->channel_name;
            m->chat = d->chat_id;
        } else {
            m->chan = row->channel;
            m->chat = row->chat;
        }
        return;
    }
    /* Default: follow the conversation. The chat is the durable identity, so
     * re-stamp it from whoever is setting the job now — unless that caller
     * has no chat (CLI, sub-agent), which must not silently strip the
     * delivery target off a job set from a real conversation. */
    if (cc || ct) {
        m->chan = cc;
        m->chat = ct;
    } else {
        m->chan = row->channel;
        m->chat = row->chat;
    }
}

static int64_t merged_write(sqlite3 *db, const char *agent, const char *name,
                            const CronMerged *m) {
    const char *sql =
        "INSERT INTO cron_jobs (agent_name, name, cron_expr, run_at, interval_s,"
        "                       kind, session_id, task, script, channel_name,"
        "                       chat_id, target, target_agent, enabled, next_run_at)"
        " VALUES (?1,?2,?3,?4,?5,'task',?6,?7,?8,?9,?10,?11,?12,1,?13)"
        " ON CONFLICT(agent_name,name) DO UPDATE SET"
        "   cron_expr=excluded.cron_expr, run_at=excluded.run_at,"
        "   interval_s=excluded.interval_s, session_id=excluded.session_id,"
        "   task=excluded.task, script=excluded.script,"
        "   channel_name=excluded.channel_name, chat_id=excluded.chat_id,"
        "   target=excluded.target, target_agent=excluded.target_agent,"
        /* An upsert is also how a paused job comes back. */
        "   enabled=1, next_run_at=excluded.next_run_at"
        " RETURNING id;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, agent ? agent : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, m->expr ? m->expr : "", -1, SQLITE_STATIC);
    if (m->run_at > 0) sqlite3_bind_int64(st, 4, m->run_at); else sqlite3_bind_null(st, 4);
    if (m->interval_s > 0) sqlite3_bind_int64(st, 5, m->interval_s); else sqlite3_bind_null(st, 5);
    sqlite3_bind_int64(st, 6, m->session_id);
    sqlite3_bind_text(st, 7, m->task ? m->task : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 8, m->script, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 9, m->chan, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 10, m->chat, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 11, m->target, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 12, m->target_agent, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 13, m->next_run_at);
    int64_t id = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int64(st, 0) : -1;
    sqlite3_finalize(st);
    return id;
}

int64_t cron_upsert(sqlite3 *db, const char *agent_name,
                    int64_t caller_session_id, const char *doc,
                    int *updated_out, char **err_out) {
    if (err_out) *err_out = NULL;
    if (updated_out) *updated_out = 0;

    CronDoc d;
    if (cron_doc_parse(db, doc, &d) != 0) {
        if (err_out) *err_out = strdup("error: cron job document is not valid JSON");
        return -1;
    }
    if (!d.name || !d.name[0]) {
        cron_doc_free(&d);
        if (err_out) *err_out = strdup("error: cron job needs a name");
        return -1;
    }

    CronRow row;
    row_load(db, agent_name, d.name, &row);

    int64_t id = -1;
    char *err = doc_check_merged(&d, &row);
    CronMerged m = {0};
    if (!err && merge_schedule(db, &d, &row, &m, &err) == 0) {
        m.task = d.has_prompt ? d.prompt : (row.task ? row.task : "");
        m.script = d.has_script ? (d.script && d.script[0] ? d.script : NULL)
                                : row.script;
        char *cc = NULL, *ct = NULL;
        caller_chat(db, caller_session_id, &cc, &ct);
        int64_t anchor = (caller_session_id > 0) ? caller_session_id
                                                 : row.session_id;
        /* doc_check_merged already accepted the mode, so this cannot fail. */
        char *ignored = NULL;
        merge_target(&d, &row, merged_mode(&d, &row, &ignored), anchor, cc, ct, &m);
        free(ignored);
        id = merged_write(db, agent_name, d.name, &m);
        free(cc); free(ct);
        if (id < 0) err = strdup("error: could not save the cron job");
    }
    if (err) {
        if (err_out) *err_out = err; else free(err);
    }
    if (id > 0 && updated_out) *updated_out = row.found;
    row_free(&row);
    cron_doc_free(&d);
    return id;
}

CronJob *cron_list(sqlite3 *db, const char *agent_name, int *count) {
    *count = 0;
    const char *sql =
        "SELECT id, name, cron_expr, session_id, task, enabled, next_run_at,"
        "       last_run_at, COALESCE(run_at,0), COALESCE(interval_s,0), kind"
        " FROM cron_jobs WHERE agent_name=? ORDER BY id;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(stmt, 1, agent_name ? agent_name : "", -1, SQLITE_STATIC);

    int cap = 8;
    CronJob *jobs = malloc((size_t)cap * sizeof(CronJob));
    if (!jobs) { sqlite3_finalize(stmt); return NULL; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (*count >= cap) {
            cap *= 2;
            CronJob *tmp = realloc(jobs, (size_t)cap * sizeof(CronJob));
            if (!tmp) {
                cron_list_free(jobs, *count);
                *count = 0;
                sqlite3_finalize(stmt);
                return NULL;
            }
            jobs = tmp;
        }
        CronJob *j = &jobs[*count];
        j->id = sqlite3_column_int64(stmt, 0);
        const char *n = (const char *)sqlite3_column_text(stmt, 1);
        j->name = n ? strdup(n) : NULL;
        const char *ce = (const char *)sqlite3_column_text(stmt, 2);
        j->cron_expr = ce ? strdup(ce) : NULL;
        j->session_id = sqlite3_column_int64(stmt, 3);
        const char *t = (const char *)sqlite3_column_text(stmt, 4);
        j->task = t ? strdup(t) : NULL;
        j->enabled = sqlite3_column_int(stmt, 5);
        j->next_run_at = sqlite3_column_int64(stmt, 6);
        j->last_run_at = sqlite3_column_int64(stmt, 7);
        j->run_at = sqlite3_column_int64(stmt, 8);
        j->interval_s = sqlite3_column_int64(stmt, 9);
        const char *k = (const char *)sqlite3_column_text(stmt, 10);
        j->kind = k ? strdup(k) : strdup("task");
        (*count)++;
    }
    sqlite3_finalize(stmt);
    if (*count == 0) { free(jobs); return NULL; }
    return jobs;
}

int cron_seed_heartbeat(sqlite3 *db, const char *agent_name) {
    if (!db || !agent_name) return -1;
    /* Idempotent: insert only if this agent has no heartbeat row yet.
     * Seeded disabled (enabled=0) — heartbeats cost an LLM call per fire, so
     * turning the pulse on stays a deliberate operator/tool act. */
    const char *sql =
        "INSERT INTO cron_jobs (agent_name, name, kind, cron_expr, interval_s,"
        "                       session_id, task, enabled, next_run_at)"
        " SELECT ?1, 'heartbeat', 'heartbeat', '', 1800, 0, '', 0, 0"
        " WHERE NOT EXISTS (SELECT 1 FROM cron_jobs"
        "                   WHERE agent_name=?1 AND kind='heartbeat');";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, agent_name, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int cron_remove(sqlite3 *db, int64_t job_id, const char *agent_name) {
    /* Agent-scoped: an agent can only delete its own jobs (review-2 F2). */
    const char *sql = "DELETE FROM cron_jobs WHERE id=? AND agent_name=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, job_id);
    sqlite3_bind_text(stmt, 2, agent_name ? agent_name : "", -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE && sqlite3_changes(db) > 0) ? 0 : -1;
}

void cron_list_free(CronJob *jobs, int count) {
    if (!jobs) return;
    for (int i = 0; i < count; i++) {
        free(jobs[i].name);
        free(jobs[i].cron_expr);
        free(jobs[i].task);
        free(jobs[i].kind);
    }
    free(jobs);
}

/* Resolve an agent's most recently active session at fire time. idle_only
 * restricts to state='idle' (heartbeat targeting). Returns 0 if none. */
static int64_t recent_session(sqlite3 *db, const char *agent_name, int idle_only) {
    const char *sql = idle_only
        ? "SELECT id FROM sessions WHERE agent_name=? AND state='idle'"
          " ORDER BY updated_at DESC LIMIT 1;"
        : "SELECT id FROM sessions WHERE agent_name=?"
          " ORDER BY updated_at DESC LIMIT 1;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, agent_name ? agent_name : "", -1, SQLITE_STATIC);
    int64_t sid = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int64(st, 0) : 0;
    sqlite3_finalize(st);
    return sid;
}

/* True if the session already carries an unconsumed heartbeat pulse — never
 * stack a second one on top. */
static int has_pending_heartbeat(sqlite3 *db, int64_t session_id) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM inbox WHERE session_id=? AND source='heartbeat'"
            " AND consumed=0 LIMIT 1;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, session_id);
    int found = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return found;
}

/* One due row's payload and schedule, collected before any write (an open
 * SELECT pins a WAL read snapshot; a write that needs a snapshot upgrade
 * would get an immediate SQLITE_BUSY). */
typedef struct {
    int64_t id, session_id, run_at, interval_s;
    char *expr, *task, *agent_name, *kind;
} DueJob;

/* Inject a due job into the resolved session and wake the daemon. Heartbeat
 * kind resolves to the agent's most recent idle session (skips silently if
 * none, or if a pulse is already pending); task kind resolves session_id=0 to
 * the most recent session (WARN-skips if the agent has none). */
static void fire_due(sqlite3 *db, const DueJob *j) {
    int heartbeat = (j->kind && strcmp(j->kind, "heartbeat") == 0);
    if (heartbeat) {
        int64_t sid = recent_session(db, j->agent_name, 1);
        if (sid == 0) return;                       /* busy/no idle session — no pulse needed */
        if (has_pending_heartbeat(db, sid)) return; /* never stack pulses */
        inbox_insert(db, sid, "heartbeat", NULL, HEARTBEAT_PROMPT);
        LOG_INFO_("heartbeat fire job=%lld agent=%s session=%lld",
                  (long long)j->id, j->agent_name ? j->agent_name : "", (long long)sid);
        wake_session(sid);
        return;
    }

    int64_t sid = j->session_id;
    if (sid == 0) {
        sid = recent_session(db, j->agent_name, 0);
        if (sid == 0) {
            LOG_WARN_("cron skip job=%lld agent=%s: no session to resolve",
                      (long long)j->id, j->agent_name ? j->agent_name : "");
            return;
        }
    }
    if (!j->task) return;
    inbox_insert_scanned(db, sid, "cron", NULL, j->task);
    LOG_INFO_("cron fire job=%lld agent=%s session=%lld",
              (long long)j->id, j->agent_name ? j->agent_name : "", (long long)sid);
    wake_session(sid);
}

/* Reschedule (or delete) a fired row. One-shot self-cleans; interval refires
 * every interval_s; recurring advances by cron_next_run. Replaces (not
 * follows) the old unconditional UPDATE — an empty-expr job must never fall
 * back to firing hourly forever. */
static void reschedule_due(sqlite3 *db, const DueJob *j, int64_t now) {
    if (j->run_at > 0) {
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db, "DELETE FROM cron_jobs WHERE id=?;",
                               -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, j->id);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
        return;
    }
    int64_t next;
    if (j->interval_s > 0) {
        next = now + j->interval_s;
    } else {
        next = cron_next_run(j->expr, now);
        if (next < 0) {
            /* Unreachable for a validated recurring job; disable rather than
             * hot-loop if a malformed expr ever slips in. */
            LOG_ERROR_("cron job=%lld bad expr, disabling", (long long)j->id);
            sqlite3_stmt *st;
            if (sqlite3_prepare_v2(db, "UPDATE cron_jobs SET enabled=0 WHERE id=?;",
                                   -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(st, 1, j->id);
                sqlite3_step(st);
                sqlite3_finalize(st);
            }
            return;
        }
    }
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "UPDATE cron_jobs SET last_run_at=?, next_run_at=? WHERE id=?;",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, now);
        sqlite3_bind_int64(st, 2, next);
        sqlite3_bind_int64(st, 3, j->id);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
}

static void due_free(DueJob *due, int count) {
    for (int i = 0; i < count; i++) {
        free(due[i].expr);
        free(due[i].task);
        free(due[i].agent_name);
        free(due[i].kind);
    }
    free(due);
}

/* Check and execute due cron jobs */
static void run_due_jobs(sqlite3 *db) {
    int64_t now = (int64_t)time(NULL);
    const char *sql =
        "SELECT id, cron_expr, session_id, task, agent_name, kind,"
        "       COALESCE(run_at,0), COALESCE(interval_s,0) FROM cron_jobs"
        " WHERE enabled=1 AND next_run_at <= ?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(stmt, 1, now);

    /* Collect due jobs first (avoid holding stmt open during inbox_insert) */
    int cap = 4, count = 0;
    DueJob *due = malloc((size_t)cap * sizeof(DueJob));
    if (!due) { sqlite3_finalize(stmt); return; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            DueJob *tmp = realloc(due, (size_t)cap * sizeof(DueJob));
            if (!tmp) { due_free(due, count); sqlite3_finalize(stmt); return; }
            due = tmp;
        }
        due[count].id = sqlite3_column_int64(stmt, 0);
        const char *e = (const char *)sqlite3_column_text(stmt, 1);
        due[count].expr = e ? strdup(e) : NULL;
        due[count].session_id = sqlite3_column_int64(stmt, 2);
        const char *t = (const char *)sqlite3_column_text(stmt, 3);
        due[count].task = t ? strdup(t) : NULL;
        const char *a = (const char *)sqlite3_column_text(stmt, 4);
        due[count].agent_name = a ? strdup(a) : NULL;
        const char *k = (const char *)sqlite3_column_text(stmt, 5);
        due[count].kind = k ? strdup(k) : strdup("task");
        due[count].run_at = sqlite3_column_int64(stmt, 6);
        due[count].interval_s = sqlite3_column_int64(stmt, 7);
        count++;
    }
    sqlite3_finalize(stmt);

    /* Fire and reschedule each due job. Both phases only run after the reader
     * above is finalized. */
    for (int i = 0; i < count; i++) {
        fire_due(db, &due[i]);
        reschedule_due(db, &due[i], now);
    }
    due_free(due, count);
}

void cron_run_due(sqlite3 *db) {
    run_due_jobs(db);
}
