/* test_integration_backup.c — the `cclaw backup` snapshot path (project
 * backup-restore, step 2). Confirms db_backup_to:
 *   (a) captures a consistent, openable snapshot after a real mock-server turn,
 *   (b) preserves a mid-turn (llm_running) session — restore-time recovery
 *       reconciles it, so the snapshot need only be internally consistent,
 *   (c) succeeds while a separate connection writes concurrently (VACUUM INTO
 *       takes a read snapshot under WAL — no torn write, no writer lockout),
 *   (d) refuses to overwrite an existing file (fail-closed). */
#define _POSIX_C_SOURCE 200809L
#include "test_run_session.h"
#include "mock_server.h"
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "test_util.h"

#define DB_PATH  "/tmp/test_integ_backup.db"
#define BAK1     "/tmp/test_integ_backup.snap1.db"
#define BAK2     "/tmp/test_integ_backup.snap2.db"

static const char *MOCK_RESPONSE =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"ok\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":2}}";

/* Count entries for a session in an arbitrary DB handle. */
static int64_t entry_count(sqlite3 *db, int64_t sid) {
    return db_scalar_i64(db, "SELECT COUNT(*) FROM entries WHERE session_id=?", sid, -1);
}

/* Writer thread: hammer a second connection with inserts while a backup runs,
 * proving VACUUM INTO coexists with a busy writer under WAL. */
struct writer_arg { const char *db_path; volatile int *stop; int64_t sid; };
static void *writer_fn(void *arg) {
    struct writer_arg *w = (struct writer_arg *)arg;
    sqlite3 *wdb = db_open(w->db_path);
    if (!wdb) return NULL;
    db_set_child_pragmas(wdb);
    int n = 0;
    while (!*w->stop && n < 500) {
        Message m = {.role = ROLE_USER, .content = "writer churn"};
        entry_append_with_turn(wdb, w->sid, &m, 1);
        n++;
    }
    db_close(wdb);
    return NULL;
}

int main(void) {
    TEST_INIT();
    alarm(30);
    test_db_clean(DB_PATH);
    unlink(BAK1); unlink(BAK2);

    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);

    int port = mock_server_start();
    assert(port > 0);
    char url[64]; snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", port);
    setenv("CCLAW_DB_PATH", DB_PATH, 1);
    setenv("CCLAW_PROVIDER_BASE_URL", url, 1);
    setenv("OPENROUTER_API_KEY", "test-key", 1);
    setenv("CCLAW_MODEL", "test-model", 1);
    setenv("CCLAW_AGENT_NAME", "Backup", 1);
    setenv("CCLAW_STREAM", "0", 1);
    db_agent_upsert(db, "Backup", NULL, NULL);

    /* A real turn so the snapshot has genuine content. */
    int64_t sid = session_create(db, "bk", "Backup", -1, 0);
    assert(sid > 0);
    Message sys = {.role = ROLE_SYSTEM, .content = "You are helpful."};
    entry_append_with_turn(db, sid, &sys, 1);
    Message user = {.role = ROLE_USER, .content = "hi"};
    entry_append_with_turn(db, sid, &user, 1);
    mock_server_enqueue(200, MOCK_RESPONSE);

    Config *cfg = config_load(db);
    AgentSetup setup;
    assert(agent_setup_init(&setup, db, sid, cfg, "Backup", AGENT_SETUP_CLI) == 0);
    assert(test_run_session(db, sid, &setup) == 0);
    agent_setup_destroy(&setup);
    config_free(cfg);

    int64_t entries_live = entry_count(db, sid);
    assert(entries_live > 0);

    /* (b) A mid-turn session: leave it in llm_running when the snapshot fires. */
    int64_t midsid = session_create(db, "midturn", "Backup", -1, 0);
    Message mu = {.role = ROLE_USER, .content = "in flight"};
    entry_append_with_turn(db, midsid, &mu, 1);
    assert(session_set_state(db, midsid, "llm_running") == 0);

    /* (a) Snapshot, then verify it opens and carries the same data. */
    long long bytes = -1;
    assert(db_backup_to(db, BAK1, &bytes) == 0);
    assert(bytes > 0);
    struct stat st;
    assert(stat(BAK1, &st) == 0 && st.st_size == bytes);

    sqlite3 *snap = db_open(BAK1);
    assert(snap);
    assert(db_schema_compat(snap));                 /* valid cclaw schema */
    assert(entry_count(snap, sid) == entries_live); /* same content */
    /* (b) the mid-turn session is present and still marked llm_running — the
     * restart recovery path (db_recover_stale_sessions) reconciles it. */
    char *midstate = db_scalar_text(snap,
        "SELECT state FROM sessions WHERE id=?", midsid);
    assert(midstate && strcmp(midstate, "llm_running") == 0);
    free(midstate);
    db_close(snap);
    printf("  PASS backup captures consistent snapshot (incl. mid-turn session)\n");

    /* (d) Overwrite is refused. */
    assert(db_backup_to(db, BAK1, NULL) == -1);
    printf("  PASS backup refuses to overwrite\n");

    /* (c) Backup under a concurrent writer. */
    volatile int stop = 0;
    struct writer_arg warg = { DB_PATH, &stop, sid };
    pthread_t wt;
    assert(pthread_create(&wt, NULL, writer_fn, &warg) == 0);
    /* Fire the snapshot while the writer churns. */
    int rc = db_backup_to(db, BAK2, &bytes);
    stop = 1;
    pthread_join(wt, NULL);
    assert(rc == 0 && bytes > 0);
    snap = db_open(BAK2);
    assert(snap && db_schema_compat(snap));
    assert(entry_count(snap, sid) >= entries_live);  /* consistent, not torn */
    db_close(snap);
    printf("  PASS backup succeeds under a concurrent writer (WAL read snapshot)\n");

    mock_server_stop();
    db_close(db);
    test_db_clean(DB_PATH);
    unlink(BAK1); unlink(BAK2);
    printf("PASS test_integration_backup\n");
    return 0;
}
