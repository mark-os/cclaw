#define _GNU_SOURCE
#include "approval.h"
#include "config_registry.h"
#include "cron.h"
#include "db.h"
#include "test_util.h"
#include "tool_cron.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define WS "/tmp/test_tool_cron_ws"

/* Every agent name this file writes into cron_jobs/sessions rows. */
static sqlite3 *open_seeded(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    test_seed_agent(db, "test_agent");
    test_seed_agent(db, "A");
    test_seed_agent(db, "agent_a");
    test_seed_agent(db, "agent_b");
    return db;
}

static ToolCronCtx mkctx(sqlite3 *db, int64_t sid, const char *agent) {
    ToolCronCtx c = {.db = db, .session_id = sid, .workspace = WS,
                     .current_tool_call_id = "call_1"};
    snprintf(c.agent_name, sizeof(c.agent_name), "%s", agent);
    return c;
}

/* Single text column of a one-row query over cron_jobs, or NULL. */
static char *job_text(sqlite3 *db, const char *col, const char *name) {
    char sql[192];
    snprintf(sql, sizeof(sql), "SELECT %s FROM cron_jobs WHERE name=?1", col);
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) out = test_strdup_(v);
    }
    sqlite3_finalize(st);
    return out;
}

static int64_t job_int(sqlite3 *db, const char *col, const char *name) {
    char sql[192];
    snprintf(sql, sizeof(sql), "SELECT COALESCE(%s,0) FROM cron_jobs WHERE name=?1", col);
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    int64_t v = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int64(st, 0) : -1;
    sqlite3_finalize(st);
    return v;
}

static int64_t job_total(sqlite3 *db) {
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM cron_jobs", -1, &st, NULL)
           == SQLITE_OK);
    sqlite3_step(st);
    int64_t n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

/* ── the basics ──────────────────────────────────────────────────────── */

static void test_set_recurring(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "test", "test_agent", -1, 0);
    assert(sid > 0);

    ToolCronCtx ctx = mkctx(db, sid, "test_agent");
    char *r = tool_cron_set_handler(
        "{\"name\":\"daily\",\"cron_expr\":\"0 9 * * *\",\"prompt\":\"good morning\"}",
        &ctx);
    assert(r);
    assert(strstr(r, "created cron job"));
    assert(strstr(r, "daily"));
    assert(strstr(r, "payload: prompt"));
    free(r);

    char *task = job_text(db, "task", "daily");
    assert(task && strcmp(task, "good morning") == 0);
    free(task);
    assert(job_int(db, "session_id", "daily") == sid);
    assert(job_text(db, "target", "daily") == NULL);   /* follow the conversation */

    db_close(db);
    printf("  PASS: recurring prompt job\n");
}

/* Name absent → generated, returned in the result, and the row is there. */
static void test_set_auto_name(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "test", "A", -1, 0);

    ToolCronCtx ctx = mkctx(db, sid, "A");
    char *r = tool_cron_set_handler(
        "{\"in_seconds\":600,\"prompt\":\"check the build\"}", &ctx);
    assert(r && strstr(r, "created cron job"));

    /* The generated name is echoed — pull it back out of the result text. */
    const char *q1 = strchr(r, '"');
    assert(q1);
    const char *q2 = strchr(q1 + 1, '"');
    assert(q2);
    char name[64] = {0};
    memcpy(name, q1 + 1, (size_t)(q2 - q1 - 1));
    assert(strncmp(name, "job-", 4) == 0);
    free(r);

    char *task = job_text(db, "task", name);
    assert(task && strcmp(task, "check the build") == 0);
    free(task);
    assert(job_int(db, "run_at", name) >= (int64_t)time(NULL) + 598);

    db_close(db);
    printf("  PASS: auto-generated name returned and stored\n");
}

static void test_set_bare_wake(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "test", "A", -1, 0);
    ToolCronCtx ctx = mkctx(db, sid, "A");

    char *r = tool_cron_set_handler("{\"name\":\"wake\",\"in_seconds\":300}", &ctx);
    assert(r && strstr(r, "created cron job"));
    assert(strstr(r, "bare wake"));
    free(r);

    char *task = job_text(db, "task", "wake");
    assert(task && task[0] == '\0');
    free(task);
    assert(job_text(db, "script", "wake") == NULL);

    db_close(db);
    printf("  PASS: bare wake (no prompt, no script)\n");
}

/* ── upsert ──────────────────────────────────────────────────────────── */

static void test_upsert_keeps_unnamed_fields(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "test", "A", -1, 0);
    ToolCronCtx ctx = mkctx(db, sid, "A");

    char *r = tool_cron_set_handler(
        "{\"name\":\"j\",\"cron_expr\":\"0 9 * * *\",\"prompt\":\"morning\"}", &ctx);
    assert(r && strstr(r, "created")); free(r);
    int64_t id = job_int(db, "id", "j");

    /* New schedule, prompt untouched. */
    r = tool_cron_set_handler("{\"name\":\"j\",\"cron_expr\":\"0 17 * * *\"}", &ctx);
    assert(r && strstr(r, "updated cron job"));
    assert(strstr(r, "0 17 * * *"));
    free(r);
    char *v = job_text(db, "cron_expr", "j");
    assert(v && strcmp(v, "0 17 * * *") == 0); free(v);
    v = job_text(db, "task", "j");
    assert(v && strcmp(v, "morning") == 0); free(v);

    /* New prompt, schedule untouched. */
    r = tool_cron_set_handler("{\"name\":\"j\",\"prompt\":\"evening\"}", &ctx);
    assert(r && strstr(r, "updated cron job")); free(r);
    v = job_text(db, "cron_expr", "j");
    assert(v && strcmp(v, "0 17 * * *") == 0); free(v);
    v = job_text(db, "task", "j");
    assert(v && strcmp(v, "evening") == 0); free(v);

    /* Same row throughout — the name is the identity. */
    assert(job_int(db, "id", "j") == id);
    assert(job_total(db) == 1);

    db_close(db);
    printf("  PASS: upsert replaces named fields, keeps the rest\n");
}

static void test_upsert_clears_payload(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "test", "A", -1, 0);
    ToolCronCtx ctx = mkctx(db, sid, "A");

    char *r = tool_cron_set_handler(
        "{\"name\":\"j\",\"in_seconds\":600,\"prompt\":\"noisy\"}", &ctx);
    assert(r && strstr(r, "created")); free(r);

    r = tool_cron_set_handler("{\"name\":\"j\",\"prompt\":\"\"}", &ctx);
    assert(r && strstr(r, "updated"));
    assert(strstr(r, "bare wake"));
    free(r);
    char *v = job_text(db, "task", "j");
    assert(v && v[0] == '\0'); free(v);

    db_close(db);
    printf("  PASS: empty prompt clears the payload\n");
}

/* Validity is judged on the merged row, not on the call alone. */
static void test_upsert_merged_mode_conflict(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "test", "A", -1, 0);
    ToolCronCtx ctx = mkctx(db, sid, "A");

    char args[160];
    snprintf(args, sizeof(args),
             "{\"name\":\"pinned\",\"in_seconds\":600,\"prompt\":\"p\","
             "\"session_id\":%lld}", (long long)sid);
    char *r = tool_cron_set_handler(args, &ctx);
    assert(r && strstr(r, "created"));
    free(r);
    char *v = job_text(db, "target", "pinned");
    assert(v && strcmp(v, "pin") == 0); free(v);

    /* The same call would be legal on a fresh name; on this job it is not. */
    r = tool_cron_set_handler("{\"name\":\"pinned\",\"session\":\"new\"}", &ctx);
    assert(r && strstr(r, "error"));
    assert(strstr(r, "pinned to session"));
    free(r);
    v = job_text(db, "target", "pinned");
    assert(v && strcmp(v, "pin") == 0); free(v);   /* unchanged */

    db_close(db);
    printf("  PASS: merged-row mode conflict rejected\n");
}

static void test_upsert_no_schedule(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "test", "A", -1, 0);
    ToolCronCtx ctx = mkctx(db, sid, "A");

    char *r = tool_cron_set_handler("{\"name\":\"j\",\"prompt\":\"p\"}", &ctx);
    assert(r && strstr(r, "error") && strstr(r, "no schedule"));
    free(r);
    assert(job_total(db) == 0);

    db_close(db);
    printf("  PASS: a new job needs a schedule\n");
}

/* ── cross-field rules ───────────────────────────────────────────────── */

static void expect_error(sqlite3 *db, int64_t sid, const char *agent,
                         const char *args, const char *needle,
                         const char *label) {
    ToolCronCtx ctx = mkctx(db, sid, agent);
    char *r = tool_cron_set_handler(args, &ctx);
    assert(r);
    if (!strstr(r, "error") || !strstr(r, needle)) {
        fprintf(stderr, "  %s: unexpected result: %s\n", label, r);
        assert(0);
    }
    free(r);
}

static void test_cross_field_rules(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "test", "A", -1, 0);

    expect_error(db, sid, "A",
        "{\"name\":\"x\",\"cron_expr\":\"0 * * * *\",\"in_seconds\":600}",
        "at most one of cron_expr", "both schedules");
    expect_error(db, sid, "A",
        "{\"name\":\"x\",\"in_seconds\":600,\"session\":\"new\",\"session_id\":1}",
        "at most one of session", "session + session_id");
    expect_error(db, sid, "A",
        "{\"name\":\"x\",\"in_seconds\":600,\"session\":\"fresh\"}",
        "only \"new\"", "bad session value");
    expect_error(db, sid, "A",
        "{\"name\":\"x\",\"in_seconds\":600,\"agent\":\"agent_b\"}",
        "session:\"new\"", "agent without new");
    expect_error(db, sid, "A",
        "{\"name\":\"x\",\"in_seconds\":600,\"session\":\"new\","
        "\"channel_name\":\"discord\"}",
        "go together", "channel without chat");
    expect_error(db, sid, "A",
        "{\"name\":\"x\",\"in_seconds\":600,\"chat_id\":\"55\"}",
        "go together", "chat without channel");
    expect_error(db, sid, "A",
        "{\"name\":\"x\",\"in_seconds\":600,\"promt\":\"typo\"}",
        "unknown field", "unknown field");
    expect_error(db, sid, "A",
        "{\"name\":\"x\",\"in_seconds\":-5,\"prompt\":\"p\"}",
        "positive delay", "negative in_seconds");
    expect_error(db, sid, "A",
        "{\"name\":\"x\",\"in_seconds\":600,\"session_id\":98765}",
        "does not exist", "pin to a missing session");

    assert(job_total(db) == 0);
    db_close(db);
    printf("  PASS: cross-field rules each name their rule\n");
}

/* Channel fields need session:"new" — the merged-row half of the rule. */
static void test_channel_needs_new_session(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "test", "A", -1, 0);
    expect_error(db, sid, "A",
        "{\"name\":\"x\",\"in_seconds\":600,\"channel_name\":\"discord\","
        "\"chat_id\":\"55\"}",
        "session:\"new\"", "channel without new");
    db_close(db);
    printf("  PASS: channel fields require a fresh-session job\n");
}

/* ── the floor, and the two schedule refusals ────────────────────────── */

static void test_floor_message_teaches(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "test", "A", -1, 0);
    ToolCronCtx ctx = mkctx(db, sid, "A");

    char *r = tool_cron_set_handler(
        "{\"name\":\"poke\",\"in_seconds\":20,\"prompt\":\"any news?\"}", &ctx);
    assert(r && strstr(r, "error"));
    assert(strstr(r, "in_seconds=20"));                 /* the offending value */
    assert(strstr(r, "floor=30s"));                     /* the configured floor */
    assert(strstr(r, "cron_min_interval_seconds"));     /* where it comes from */
    assert(strstr(r, "do not schedule checks"));        /* the redirect */
    free(r);

    /* A raised floor is echoed as configured, not as a constant. */
    assert(config_set(db, "cron_min_interval_seconds", "300") == 0);
    r = tool_cron_set_handler(
        "{\"name\":\"poke\",\"in_seconds\":60,\"prompt\":\"p\"}", &ctx);
    assert(r && strstr(r, "in_seconds=60") && strstr(r, "floor=300s"));
    free(r);

    /* Recurring: the message names the expression and its real cadence. */
    r = tool_cron_set_handler(
        "{\"name\":\"poke\",\"cron_expr\":\"* * * * *\",\"prompt\":\"p\"}", &ctx);
    assert(r && strstr(r, "below the floor"));
    assert(strstr(r, "* * * * *") && strstr(r, "every 60s"));
    assert(strstr(r, "do not schedule checks"));
    free(r);

    assert(job_total(db) == 0);
    db_close(db);
    printf("  PASS: floor refusal states floor, value and redirect\n");
}

static void test_bad_expr_message_is_distinct(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "test", "A", -1, 0);
    ToolCronCtx ctx = mkctx(db, sid, "A");

    char *r = tool_cron_set_handler(
        "{\"name\":\"bad\",\"cron_expr\":\"every day\",\"prompt\":\"p\"}", &ctx);
    assert(r && strstr(r, "error"));
    assert(strstr(r, "not a 5-field cron expression"));
    assert(strstr(r, "every day"));
    assert(!strstr(r, "floor"));          /* never conflated with the floor */
    free(r);

    db_close(db);
    printf("  PASS: bad-expression refusal is distinct from the floor\n");
}

/* ── script paths ────────────────────────────────────────────────────── */

static void ws_reset(void) {
    unlink(WS "/report.js");
    rmdir(WS);
    assert(mkdir(WS, 0700) == 0 || 1);
    FILE *f = fopen(WS "/report.js", "w");
    assert(f);
    fputs("1;\n", f);
    fclose(f);
}

static void test_script_paths(void) {
    ws_reset();
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "test", "A", -1, 0);
    ToolCronCtx ctx = mkctx(db, sid, "A");

    expect_error(db, sid, "A",
        "{\"name\":\"s\",\"in_seconds\":600,\"script\":\"/etc/passwd\"}",
        "workspace-relative", "absolute script path");
    expect_error(db, sid, "A",
        "{\"name\":\"s\",\"in_seconds\":600,\"script\":\"../escape.js\"}",
        "stay inside your workspace", "escaping script path");
    expect_error(db, sid, "A",
        "{\"name\":\"s\",\"in_seconds\":600,\"script\":\"missing.js\"}",
        "does not exist", "missing script file");
    assert(job_total(db) == 0);

    char *r = tool_cron_set_handler(
        "{\"name\":\"s\",\"in_seconds\":600,\"script\":\"report.js\"}", &ctx);
    assert(r && strstr(r, "created"));
    assert(strstr(r, "payload: script"));
    free(r);
    char *v = job_text(db, "script", "s");
    assert(v && strcmp(v, "report.js") == 0);   /* stored workspace-relative */
    free(v);

    /* prompt + script compose. */
    r = tool_cron_set_handler("{\"name\":\"s\",\"prompt\":\"summarize it\"}", &ctx);
    assert(r && strstr(r, "payload: prompt + script"));
    free(r);

    db_close(db);
    printf("  PASS: script path validation and storage\n");
}

/* ── targeting ───────────────────────────────────────────────────────── */

static void bind_chat(sqlite3 *db, int64_t sid, const char *chan, const char *chat) {
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db,
        "UPDATE sessions SET channel_name=?1, chat_id=?2 WHERE id=?3",
        -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, chan, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, chat, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 3, sid);
    assert(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
}

static void test_default_target_stamps_chat(void) {
    sqlite3 *db = open_seeded();
    int64_t bound = session_create(db, "bound", "A", -1, 0);
    bind_chat(db, bound, "discord", "chat-42");
    int64_t plain = session_create(db, "plain", "A", -1, 0);

    ToolCronCtx ctx = mkctx(db, bound, "A");
    char *r = tool_cron_set_handler(
        "{\"name\":\"bound\",\"in_seconds\":600,\"prompt\":\"p\"}", &ctx);
    assert(r && strstr(r, "created"));
    assert(strstr(r, "this conversation (discord:chat-42)"));
    free(r);
    char *v = job_text(db, "channel_name", "bound");
    assert(v && strcmp(v, "discord") == 0); free(v);
    v = job_text(db, "chat_id", "bound");
    assert(v && strcmp(v, "chat-42") == 0); free(v);
    assert(job_int(db, "session_id", "bound") == bound);

    /* An unbound caller (CLI, sub-agent) stamps no chat — the session id is
     * the fallback anchor. */
    ToolCronCtx ctx2 = mkctx(db, plain, "A");
    r = tool_cron_set_handler(
        "{\"name\":\"plain\",\"in_seconds\":600,\"prompt\":\"p\"}", &ctx2);
    assert(r && strstr(r, "created")); free(r);
    assert(job_text(db, "channel_name", "plain") == NULL);
    assert(job_text(db, "chat_id", "plain") == NULL);
    assert(job_int(db, "session_id", "plain") == plain);

    /* ...and an unbound caller updating a chat-bound job keeps its stamp
     * rather than silently stripping where the job reports. */
    r = tool_cron_set_handler("{\"name\":\"bound\",\"prompt\":\"q\"}", &ctx2);
    assert(r && strstr(r, "updated")); free(r);
    v = job_text(db, "channel_name", "bound");
    assert(v && strcmp(v, "discord") == 0); free(v);

    db_close(db);
    printf("  PASS: default target stamps the caller's chat\n");
}

static void test_new_session_same_agent(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "test", "A", -1, 0);
    ToolCronCtx ctx = mkctx(db, sid, "A");

    /* Naming yourself is not an escalation — nothing parks. */
    char *r = tool_cron_set_handler(
        "{\"name\":\"fresh\",\"in_seconds\":600,\"prompt\":\"p\","
        "\"session\":\"new\",\"agent\":\"A\"}", &ctx);
    assert(r && strstr(r, "created"));
    assert(strstr(r, "a fresh session"));
    free(r);
    char *v = job_text(db, "target", "fresh");
    assert(v && strcmp(v, "new") == 0); free(v);
    v = job_text(db, "target_agent", "fresh");
    assert(v && strcmp(v, "A") == 0); free(v);
    assert(approval_get_pending(db, sid) == NULL);

    db_close(db);
    printf("  PASS: fresh session under the calling agent\n");
}

/* An explicit chat must already route to the agent that will run the fire. */
static void test_channel_authority(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "test", "A", -1, 0);

    expect_error(db, sid, "A",
        "{\"name\":\"rep\",\"in_seconds\":600,\"prompt\":\"p\","
        "\"session\":\"new\",\"channel_name\":\"discord\",\"chat_id\":\"chat-9\"}",
        "no route sends", "unrouted chat");

    /* Route it to A and the same call is accepted. */
    int64_t rsid = session_create(db, "route", "A", -1, 0);
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO channel_routes(channel_name, chat_id, session_id)"
        " VALUES('discord','chat-9',?1)", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_int64(st, 1, rsid);
    assert(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);

    ToolCronCtx ctx = mkctx(db, sid, "A");
    char *r = tool_cron_set_handler(
        "{\"name\":\"rep\",\"in_seconds\":600,\"prompt\":\"p\","
        "\"session\":\"new\",\"channel_name\":\"discord\",\"chat_id\":\"chat-9\"}",
        &ctx);
    assert(r && strstr(r, "created"));
    assert(strstr(r, "reporting to discord:chat-9"));
    free(r);

    db_close(db);
    printf("  PASS: channel authority checked at set time\n");
}

/* ── cross-agent escalation ──────────────────────────────────────────── */

static void test_cross_agent_parks(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "test", "agent_a", -1, 0);
    ToolCronCtx ctx = mkctx(db, sid, "agent_a");

    char *r = tool_cron_set_handler(
        "{\"name\":\"borrow\",\"in_seconds\":600,\"prompt\":\"do it\","
        "\"session\":\"new\",\"agent\":\"agent_b\"}", &ctx);
    assert(r == NULL);                    /* parked: NULL_PARK contract */
    assert(job_total(db) == 0);           /* nothing written yet */

    Approval *a = approval_get_pending(db, sid);
    assert(a && strcmp(a->tool_name, "cron_set") == 0);
    assert(strcmp(a->action, "cron_set") == 0);
    assert(strcmp(a->resolve, "apply") == 0);
    assert(strcmp(a->tool_call_id, "call_1") == 0);
    /* The parked document carries the whole request, name included. */
    assert(strstr(a->args_json, "\"agent\":\"agent_b\""));
    assert(strstr(a->args_json, "\"name\":\"borrow\""));

    /* Re-requesting the same job while it is pending is refused, not doubled. */
    char *dup = tool_cron_set_handler(
        "{\"name\":\"borrow\",\"in_seconds\":600,\"prompt\":\"do it\","
        "\"session\":\"new\",\"agent\":\"agent_b\"}", &ctx);
    assert(dup && strstr(dup, "already"));
    free(dup);

    /* What apply_grant does once the human says yes. */
    char *err = NULL;
    assert(cron_upsert(db, "agent_a", sid, a->args_json, NULL, &err) > 0);
    assert(err == NULL);
    char *v = job_text(db, "target_agent", "borrow");
    assert(v && strcmp(v, "agent_b") == 0); free(v);
    approval_free(a);

    db_close(db);
    printf("  PASS: cross-agent job parks one approval, applies on yes\n");
}

static void test_cross_agent_session_pin_parks(void) {
    sqlite3 *db = open_seeded();
    int64_t mine = session_create(db, "mine", "agent_a", -1, 0);
    int64_t theirs = session_create(db, "theirs", "agent_b", -1, 0);
    ToolCronCtx ctx = mkctx(db, mine, "agent_a");

    char args[192];
    snprintf(args, sizeof(args),
             "{\"name\":\"into-b\",\"in_seconds\":600,\"prompt\":\"p\","
             "\"session_id\":%lld}", (long long)theirs);
    char *r = tool_cron_set_handler(args, &ctx);
    assert(r == NULL);
    assert(job_total(db) == 0);
    Approval *a = approval_get_pending(db, mine);
    assert(a && strcmp(a->tool_name, "cron_set") == 0);
    approval_free(a);

    /* Pinning one of the caller's own sessions parks nothing. */
    snprintf(args, sizeof(args),
             "{\"name\":\"into-mine\",\"in_seconds\":600,\"prompt\":\"p\","
             "\"session_id\":%lld}", (long long)mine);
    r = tool_cron_set_handler(args, &ctx);
    assert(r && strstr(r, "created"));
    assert(strstr(r, "fires in session"));
    free(r);

    db_close(db);
    printf("  PASS: pinning another agent's session parks\n");
}

/* ── caps, list, remove, registration ────────────────────────────────── */

static void test_per_session_cap(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "test", "A", -1, 0);
    assert(config_set(db, "cron_max_jobs_per_session", "1") == 0);

    ToolCronCtx ctx = mkctx(db, sid, "A");
    char *r1 = tool_cron_set_handler(
        "{\"name\":\"a\",\"cron_expr\":\"0 * * * *\",\"prompt\":\"t\"}", &ctx);
    assert(r1 && strstr(r1, "created"));
    free(r1);

    char *r2 = tool_cron_set_handler(
        "{\"name\":\"b\",\"cron_expr\":\"0 * * * *\",\"prompt\":\"t\"}", &ctx);
    assert(r2 && strstr(r2, "limit reached"));
    free(r2);

    /* Replacing a job the session already owns is not a new job. */
    char *r3 = tool_cron_set_handler(
        "{\"name\":\"a\",\"cron_expr\":\"30 * * * *\"}", &ctx);
    assert(r3 && strstr(r3, "updated"));
    free(r3);

    db_close(db);
    printf("  PASS: per-session cap counts new jobs only\n");
}

static void test_cron_list_empty(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    ToolCronCtx ctx = mkctx(db, sid, "test_agent");
    char *result = tool_cron_list_handler("{}", &ctx);
    assert(result);
    assert(strcmp(result, "no cron jobs") == 0);
    free(result);
    db_close(db);
    printf("  PASS: cron_list empty\n");
}

static void test_cron_list_with_jobs(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "test", "test_agent", -1, 0);
    ToolCronCtx ctx = mkctx(db, sid, "test_agent");

    char *r = tool_cron_set_handler(
        "{\"name\":\"j1\",\"cron_expr\":\"0 * * * *\",\"prompt\":\"task1\"}", &ctx);
    assert(r); free(r);
    r = tool_cron_set_handler(
        "{\"name\":\"j2\",\"cron_expr\":\"30 2 * * *\",\"prompt\":\"task2\"}", &ctx);
    assert(r); free(r);

    char *result = tool_cron_list_handler("{}", &ctx);
    assert(result);
    assert(strstr(result, "j1"));
    assert(strstr(result, "j2"));
    assert(strstr(result, "task1"));
    free(result);
    db_close(db);
    printf("  PASS: cron_list with jobs\n");
}

static void test_cron_remove_valid(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "test", "test_agent", -1, 0);
    ToolCronCtx ctx = mkctx(db, sid, "test_agent");

    char *r = tool_cron_set_handler(
        "{\"name\":\"rm_me\",\"cron_expr\":\"0 * * * *\",\"prompt\":\"bye\"}", &ctx);
    assert(r); free(r);
    int64_t jid = job_int(db, "id", "rm_me");
    assert(jid > 0);

    char buf[64];
    snprintf(buf, sizeof(buf), "{\"id\":%lld}", (long long)jid);
    char *result = tool_cron_remove_handler(buf, &ctx);
    assert(result);
    assert(strstr(result, "removed cron job"));
    free(result);

    int count = 0;
    CronJob *jobs = cron_list(db, "test_agent", &count);
    assert(count == 0 && jobs == NULL);

    db_close(db);
    printf("  PASS: cron_remove valid\n");
}

static void test_cron_remove_nonexistent(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    ToolCronCtx ctx = mkctx(db, sid, "test_agent");
    char *result = tool_cron_remove_handler("{\"id\":999}", &ctx);
    assert(result);
    assert(strstr(result, "error"));
    free(result);
    db_close(db);
    printf("  PASS: cron_remove nonexistent\n");
}

/* review-2 F2: an agent cannot remove another agent's job via the tool */
static void test_cron_remove_cross_agent(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "test", "agent_a", -1, 0);
    ToolCronCtx ctx_a = mkctx(db, sid, "agent_a");

    char *r = tool_cron_set_handler(
        "{\"name\":\"a_job\",\"cron_expr\":\"0 * * * *\",\"prompt\":\"hi\"}", &ctx_a);
    assert(r); free(r);
    int64_t jid = job_int(db, "id", "a_job");
    assert(jid > 0);

    ToolCronCtx ctx_b = mkctx(db, sid, "agent_b");
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"id\":%lld}", (long long)jid);
    char *result = tool_cron_remove_handler(buf, &ctx_b);
    assert(result && strstr(result, "error"));
    free(result);

    int count = 0;
    CronJob *jobs = cron_list(db, "agent_a", &count);
    assert(count == 1);
    cron_list_free(jobs, count);

    result = tool_cron_remove_handler(buf, &ctx_a);
    assert(result && strstr(result, "removed cron job"));
    free(result);

    db_close(db);
    printf("  PASS: cron_remove cross-agent scoped\n");
}

static void test_cron_register(void) {
    sqlite3 *db = open_seeded();

    ToolRegistry reg;
    tools_init(&reg);
    ToolCronCtx ctx = mkctx(db, 1, "test");
    assert(tool_cron_register(&reg, &ctx) == 0);
    ToolEntry *set = tools_lookup(&reg, "cron_set");
    assert(set != NULL);
    /* cron_set runs inline: it can park an approval, which a worker thread
     * has no way to do. */
    assert(set->recipe.vehicle == EXEC_INLINE);
    assert(tools_lookup(&reg, "cron_list") != NULL);
    assert(tools_lookup(&reg, "cron_remove") != NULL);
    tools_free(&reg);
    db_close(db);
    printf("  PASS: tool registration\n");
}

int main(void) {
    TEST_INIT();
    printf("test_tool_cron:\n");
    test_set_recurring();
    test_set_auto_name();
    test_set_bare_wake();
    test_upsert_keeps_unnamed_fields();
    test_upsert_clears_payload();
    test_upsert_merged_mode_conflict();
    test_upsert_no_schedule();
    test_cross_field_rules();
    test_channel_needs_new_session();
    test_floor_message_teaches();
    test_bad_expr_message_is_distinct();
    test_script_paths();
    test_default_target_stamps_chat();
    test_new_session_same_agent();
    test_channel_authority();
    test_cross_agent_parks();
    test_cross_agent_session_pin_parks();
    test_per_session_cap();
    test_cron_list_empty();
    test_cron_list_with_jobs();
    test_cron_remove_valid();
    test_cron_remove_nonexistent();
    test_cron_remove_cross_agent();
    test_cron_register();
    printf("ALL PASSED\n");
    return 0;
}
