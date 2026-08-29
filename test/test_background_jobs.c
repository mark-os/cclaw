/* Background jobs (blocking-vs-background step 4): the job IS the tool_calls
 * row in status 'background'. Unit-level coverage — the drain-to-log stream,
 * the jobs snapshot in the turn context, orphan reconciliation at recovery,
 * and the check_session/cancel surfaces. The full fork path (job start,
 * live log growth, inbox completion) is exercised against a live daemon
 * before release, like the other EXEC_SANDBOX behaviors. */
#define _GNU_SOURCE
#include "test_util.h"
#include "child.h"
#include "context.h"
#include "llm_payload.h"
#include "run_tool.h"
#include "tool_agent.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_ws[256];

/* Seed one session with a tool_call parked in status 'background', args
 * carrying a command. Returns the tool_calls rowid (the job id). */
static int64_t seed_job(sqlite3 *db, int64_t sid, const char *call_id,
                        const char *status, const char *resolved_by) {
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO entries (session_id, role, type, content, tool_name,"
        " part_index) VALUES (?1, 2, 'tool_call',"
        " '{\"command\":\"sleep 30 && echo done\",\"background\":true}',"
        " 'shell_exec', 0);", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_int64(st, 1, sid);
    assert(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    int64_t eid = sqlite3_last_insert_rowid(db);
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO tool_calls (session_id, entry_id, call_id, name, status,"
        " resolved_by) VALUES (?1, ?2, ?3, 'shell_exec', ?4, ?5);",
        -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_int64(st, 1, sid);
    sqlite3_bind_int64(st, 2, eid);
    sqlite3_bind_text(st, 3, call_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, status, -1, SQLITE_STATIC);
    if (resolved_by)
        sqlite3_bind_text(st, 5, resolved_by, -1, SQLITE_STATIC);
    assert(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    return sqlite3_last_insert_rowid(db);
}

/* ── drain streams body to log_fd, not outbuf ─────────────────────── */
static void test_drain_streams_to_log(void) {
    char logp[300];
    snprintf(logp, sizeof(logp), "%s/drain.log", g_ws);
    int lfd = open(logp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(lfd >= 0);

    int p[2];
    assert(pipe(p) == 0);
    ChildProc c;
    memset(&c, 0, sizeof(c));
    c.result_pipe = p[0];
    c.log_fd = lfd;

    /* Frame: status ok, zero-length meta, then a body larger than one read. */
    unsigned char hdr[5] = { RUNTOOL_STATUS_OK, 0, 0, 0, 0 };
    assert(write(p[1], hdr, 5) == 5);
    char body[9000];
    memset(body, 'x', sizeof(body));
    memcpy(body, "hello-log ", 10);
    assert(write(p[1], body, sizeof(body)) == (ssize_t)sizeof(body));
    close(p[1]);
    child_drain_pipe(&c);

    assert(c.outbuf == NULL);           /* nothing kept in daemon RAM */
    assert(c.result_pipe == -1);        /* EOF closed the pipe */
    close(lfd);
    struct stat sb;
    assert(stat(logp, &sb) == 0);
    assert(sb.st_size == (off_t)sizeof(body));   /* every byte hit disk */
    char first[16] = {0};
    int rfd = open(logp, O_RDONLY);
    assert(read(rfd, first, 10) == 10);
    close(rfd);
    assert(memcmp(first, "hello-log ", 10) == 0);
    unlink(logp);
    printf("  PASS test_drain_streams_to_log\n");
}

/* ── job log path + tail round trip ───────────────────────────────── */
static void test_log_path_and_tail(void) {
    sqlite3 *db = test_db_open(":memory:");
    char path[512];
    assert(job_log_path_build(db, 7, "call_abc", path, sizeof(path)) == 0);
    /* No agent row for session 7 in this fixture → the /tmp/cclaw-<sid>
     * fallback home; the suffix contract is what matters here. */
    assert(strstr(path, "7") != NULL);
    assert(strstr(path, "call_abc.log") != NULL);
    FILE *f = fopen(path, "w");
    assert(f);
    for (int i = 0; i < 400; i++) fprintf(f, "line %d\n", i);
    fclose(f);
    char *tail = job_log_tail(db, 7, "call_abc", 64);
    assert(tail != NULL);
    assert(strlen(tail) <= 64);
    assert(strstr(tail, "line 399") != NULL);   /* end of file, not start */
    free(tail);
    unlink(path);
    db_close(db);
    printf("  PASS test_log_path_and_tail\n");
}

/* ── jobs snapshot renders into the session context block ─────────── */
static void test_context_block_lists_jobs(void) {
    sqlite3 *db = test_db_open(":memory:");
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    int64_t job = seed_job(db, sid, "call_ctx", "background", "inst-live");
    char *ctx = session_context_text(db, sid, NULL);
    assert(ctx != NULL);
    assert(strstr(ctx, "<background_jobs>") != NULL);
    char needle[64];
    snprintf(needle, sizeof(needle), "job %lld (shell_exec)", (long long)job);
    assert(strstr(ctx, needle) != NULL);
    assert(strstr(ctx, "sleep 30") != NULL);            /* command snippet */
    assert(strstr(ctx, ".tool_results/") != NULL);      /* log pointer */
    assert(strstr(ctx, "ps cannot see") != NULL);       /* teaching line */
    free(ctx);

    /* A finished job disappears from the snapshot. */
    assert(sqlite3_exec(db,
        "UPDATE tool_calls SET status='done', resolved_by='job:exit=0';",
        NULL, NULL, NULL) == SQLITE_OK);
    ctx = session_context_text(db, sid, NULL);
    assert(ctx && strstr(ctx, "<background_jobs>") == NULL);
    free(ctx);
    db_close(db);
    printf("  PASS test_context_block_lists_jobs\n");
}

/* ── orphan reconciliation: dead instance's job closes, live one stays ── */
static void test_recovery_reconciles_orphans(void) {
    sqlite3 *db = test_db_open(":memory:");
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    assert(sqlite3_exec(db,
        "INSERT INTO processes (instance_id, pid, mode)"
        " VALUES ('inst-live', 1234, 'daemon');",
        NULL, NULL, NULL) == SQLITE_OK);
    int64_t dead = seed_job(db, sid, "call_dead", "background", "inst-dead");
    int64_t live = seed_job(db, sid, "call_live", "background", "inst-live");

    assert(db_recover_stale_sessions(db) == 0);

    char sql[128];
    snprintf(sql, sizeof(sql),
             "SELECT status='done' AND resolved_by='job:orphaned'"
             " FROM tool_calls WHERE id=%lld;", (long long)dead);
    assert(db_scalar_i64(db, sql, 0, 0) == 1);
    snprintf(sql, sizeof(sql),
             "SELECT status='background' FROM tool_calls WHERE id=%lld;",
             (long long)live);
    assert(db_scalar_i64(db, sql, 0, 0) == 1);
    /* The model is told, with the log pointer. */
    assert(db_scalar_i64(db,
        "SELECT EXISTS(SELECT 1 FROM inbox WHERE source='job_result'"
        " AND payload LIKE '%did not survive%');", 0, 0) == 1);
    assert(db_scalar_i64(db,
        "SELECT COUNT(*) FROM inbox WHERE source='job_result';", 0, 0) == 1);
    db_close(db);
    printf("  PASS test_recovery_reconciles_orphans\n");
}

/* ── check_session arities ────────────────────────────────────────── */
static void test_check_session_job_and_list(void) {
    sqlite3 *db = test_db_open(":memory:");
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    int64_t other = session_create(db, "test", NULL, -1, 0);
    int64_t job = seed_job(db, sid, "call_chk", "background", "inst-x");
    AgentLaunchCtx ctx = { .db = db, .session_id = sid };
    int err = 0;

    /* job arity */
    char args[64];
    snprintf(args, sizeof(args), "{\"job_id\":%lld}", (long long)job);
    char *r = tool_check_session_handler(args, &ctx, &err);
    assert(r && !err);
    assert(strstr(r, "running") != NULL);
    assert(strstr(r, "sleep 30") != NULL);
    assert(strstr(r, ".tool_results/") != NULL);
    free(r);

    /* both args is an error */
    err = 0;
    snprintf(args, sizeof(args), "{\"job_id\":%lld,\"session_id\":%lld}",
             (long long)job, (long long)other);
    r = tool_check_session_handler(args, &ctx, &err);
    assert(r && err);
    free(r);

    /* ownership: another session's ctx can't see the job */
    err = 0;
    AgentLaunchCtx octx = { .db = db, .session_id = other };
    snprintf(args, sizeof(args), "{\"job_id\":%lld}", (long long)job);
    r = tool_check_session_handler(args, &octx, &err);
    assert(r && err);
    free(r);

    /* bare list names the job */
    err = 0;
    r = tool_check_session_handler("{}", &ctx, &err);
    assert(r && !err);
    assert(strstr(r, "job ") != NULL);
    free(r);
    /* ...and an empty session says so */
    err = 0;
    r = tool_check_session_handler("{}", &octx, &err);
    assert(r && !err);
    assert(strstr(r, "no live sub-agent sessions") != NULL);
    free(r);
    db_close(db);
    printf("  PASS test_check_session_job_and_list\n");
}

/* ── cancel: ownership + no-live-child path ───────────────────────── */
static void test_cancel(void) {
    sqlite3 *db = test_db_open(":memory:");
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    int64_t other = session_create(db, "test", NULL, -1, 0);
    int64_t job = seed_job(db, sid, "call_cx", "background", "inst-x");
    AgentLaunchCtx ctx = { .db = db, .session_id = sid };
    int err = 0;
    char args[64];

    /* not yours */
    AgentLaunchCtx octx = { .db = db, .session_id = other };
    snprintf(args, sizeof(args), "{\"job_id\":%lld}", (long long)job);
    char *r = tool_cancel_handler(args, &octx, &err);
    assert(r && err);
    free(r);

    /* session cancel is explicitly not supported yet */
    err = 0;
    r = tool_cancel_handler("{\"session_id\":1}", &ctx, &err);
    assert(r && err && strstr(r, "not supported"));
    free(r);

    /* yours, but no live child (e.g. post-restart): row closes as cancelled */
    err = 0;
    snprintf(args, sizeof(args), "{\"job_id\":%lld}", (long long)job);
    r = tool_cancel_handler(args, &ctx, &err);
    assert(r && !err);
    assert(strstr(r, "already gone") != NULL);
    char sql[128];
    snprintf(sql, sizeof(sql),
             "SELECT status='done' AND resolved_by='job:cancelled'"
             " FROM tool_calls WHERE id=%lld;", (long long)job);
    assert(db_scalar_i64(db, sql, 0, 0) == 1);

    /* cancelling it again refuses (no longer background) */
    err = 0;
    r = tool_cancel_handler(args, &ctx, &err);
    assert(r && err);
    free(r);
    db_close(db);
    printf("  PASS test_cancel\n");
}

/* ── turn-join ignores 'background' ───────────────────────────────── */
static void test_turn_join_ignores_background(void) {
    sqlite3 *db = test_db_open(":memory:");
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    seed_job(db, sid, "call_tj", "background", "inst-x");
    assert(db_tool_call_any_running(db, sid) == 0);
    int n = 0;
    PendingToolCall *p = db_tool_call_get_pending(db, sid, &n);
    assert(n == 0 && p == NULL);
    db_close(db);
    printf("  PASS test_turn_join_ignores_background\n");
}

int main(void) {
    TEST_INIT();
    unsetenv("CCLAW_WORKSPACE");   /* log paths must derive per-session */
    snprintf(g_ws, sizeof(g_ws), "/tmp/cclaw_test_bgjobs_XXXXXX");
    assert(mkdtemp(g_ws) != NULL);
    printf("test_background_jobs:\n");
    test_drain_streams_to_log();
    test_log_path_and_tail();
    test_context_block_lists_jobs();
    test_recovery_reconciles_orphans();
    test_check_session_job_and_list();
    test_cancel();
    test_turn_join_ignores_background();
    rmdir(g_ws);
    printf("All background-job tests passed\n");
    return 0;
}
