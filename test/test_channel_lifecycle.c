/* Channel lifecycle: draft -> validated -> active -> broken.
 * extension_install() lands a channel in 'draft'; channel_runner_check()
 * (the --check path, minus process exit/DB transition, which main.c does)
 * validates the manifest + loads JS + runs onInit(); channel_mark_validated
 * / channel_activate do the DB transitions; channel_launch_all's gate query
 * is exercised directly (not via real fork/exec — that would spawn a live
 * channel process from a unit test). */
#define _POSIX_C_SOURCE 200809L
#include "channel.h"
#include "channel_runner.h"
#include "extension_manifest.h"
#include "approval.h"
#include "resolve.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* channel.o references resolve_approval (normally defined in main.c, which
 * isn't linked into libcclaw.a) — see test_channel_events.c for the same
 * pattern. Unused here beyond satisfying the linker. */
void resolve_approval(int64_t approval_id, ApprovalDecision decision, const char *decided_via,
                      int64_t grant_expires_at) {
    (void)approval_id; (void)decision; (void)decided_via; (void)grant_expires_at;
}

#define TEST_DB "/tmp/test_channel_lifecycle.sqlite"
#define BUNDLE  "/tmp/test_channel_lifecycle_bundle/echo"

static const char *MANIFEST =
    "{\n"
    "  \"name\": \"echo\",\n"
    "  \"version\": \"0.1.0\",\n"
    "  \"description\": \"trivial test channel\",\n"
    "  \"channel\": { \"type\": \"echo\", \"handler\": \"channel.qjs\" }\n"
    "}\n";

static const char *CHANNEL_JS =
    "function onInit() { return {}; }\n";

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    assert(f);
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

static void rm_rf(const char *path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    int rc = system(cmd);
    (void)rc;
}

static void setup_bundle(void) {
    rm_rf("/tmp/test_channel_lifecycle_bundle");
    rm_rf("/tmp/extensions");
    mkdir("/tmp/test_channel_lifecycle_bundle", 0755);
    mkdir(BUNDLE, 0755);
    write_file(BUNDLE "/extension.json", MANIFEST);
    write_file(BUNDLE "/channel.qjs", CHANNEL_JS);
}

static void test_install_lands_in_draft(void) {
    test_db_clean(TEST_DB);
    setup_bundle();
    sqlite3 *db = test_db_open(TEST_DB);
    assert(db);

    char *err = NULL;
    int rc = extension_install(db, BUNDLE, "default", &err);
    if (rc != 0) fprintf(stderr, "install error: %s\n", err ? err : "?");
    assert(rc == 0);

    char *status = channel_get_status(db, "echo");
    assert(status && strcmp(status, "draft") == 0);
    free(status);

    db_close(db);
    printf("  PASS test_install_lands_in_draft\n");
}

static void test_check_then_activate(void) {
    test_db_clean(TEST_DB);
    setup_bundle();
    sqlite3 *db = test_db_open(TEST_DB);
    char *err = NULL;
    assert(extension_install(db, BUNDLE, "default", &err) == 0);
    db_close(db);

    /* --activate before --check must fail: not yet 'validated'. */
    db = test_db_open(TEST_DB);
    assert(channel_activate(db, "echo") == -1);
    char *status = channel_get_status(db, "echo");
    assert(status && strcmp(status, "draft") == 0);
    free(status);
    db_close(db);

    /* channel_runner_check runs the manifest + JS-load + onInit() sequence
     * in its own process image (opens db_path itself); it must not touch
     * channels.status — main.c applies that transition on success. */
    err = NULL;
    int rc = channel_runner_check(TEST_DB, "echo", &err);
    if (rc != 0) fprintf(stderr, "check error: %s\n", err ? err : "?");
    assert(rc == 0);
    assert(err == NULL);

    db = test_db_open(TEST_DB);
    status = channel_get_status(db, "echo");
    assert(status && strcmp(status, "draft") == 0);
    free(status);

    /* Caller applies the transition on a passing check. */
    assert(channel_mark_validated(db, "echo") == 0);
    status = channel_get_status(db, "echo");
    assert(status && strcmp(status, "validated") == 0);
    free(status);

    /* A second draft->validated call is a no-op failure — already validated. */
    assert(channel_mark_validated(db, "echo") == -1);

    /* Now --activate succeeds. */
    assert(channel_activate(db, "echo") == 0);
    status = channel_get_status(db, "echo");
    assert(status && strcmp(status, "active") == 0);
    free(status);

    /* --activate again fails: no longer 'validated'. */
    assert(channel_activate(db, "echo") == -1);

    db_close(db);
    printf("  PASS test_check_then_activate\n");
}

static void test_check_reports_js_error(void) {
    test_db_clean(TEST_DB);
    rm_rf("/tmp/test_channel_lifecycle_bundle");
    rm_rf("/tmp/extensions");
    mkdir("/tmp/test_channel_lifecycle_bundle", 0755);
    mkdir(BUNDLE, 0755);
    write_file(BUNDLE "/extension.json", MANIFEST);
    /* onInit throws — check must fail and report why, and must not touch
     * channels.status. */
    write_file(BUNDLE "/channel.qjs", "function onInit() { throw new Error('boom'); }\n");

    sqlite3 *db = test_db_open(TEST_DB);
    char *err = NULL;
    assert(extension_install(db, BUNDLE, "default", &err) == 0);
    db_close(db);

    err = NULL;
    int rc = channel_runner_check(TEST_DB, "echo", &err);
    assert(rc != 0);
    assert(err != NULL);
    free(err);

    db = test_db_open(TEST_DB);
    char *status = channel_get_status(db, "echo");
    assert(status && strcmp(status, "draft") == 0);
    free(status);
    db_close(db);
    printf("  PASS test_check_reports_js_error\n");
}

/* channel_launch_all's gate is `WHERE c.status='active'` — exercised here via
 * the identical query rather than the real function, which forks a live
 * channel process (channel_runner_main) that a unit test shouldn't spawn. */
static void test_launch_all_gate_query(void) {
    test_db_clean(TEST_DB);
    setup_bundle();
    sqlite3 *db = test_db_open(TEST_DB);
    char *err = NULL;
    assert(extension_install(db, BUNDLE, "default", &err) == 0);
    /* Second channel, never checked/activated — stays 'draft'. */
    assert(sqlite3_exec(db,
        "INSERT INTO extensions(name, path, version, owner_agent, published, enabled) "
        "VALUES('other', '/tmp/extensions/other', '0.0.0', 'default', 0, 1);",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_exec(db,
        "INSERT INTO channels(name, extension_name, type, binary_path, status) "
        "VALUES('other', 'other', 'other', '/tmp/extensions/other/channel.qjs', 'draft');",
        NULL, NULL, NULL) == SQLITE_OK);

    assert(channel_mark_validated(db, "echo") == 0);
    assert(channel_activate(db, "echo") == 0);

    const char *sql = "SELECT c.name, e.path FROM channels c"
                      " JOIN extensions e ON c.extension_name=e.name"
                      " WHERE c.status='active';";
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
    int count = 0;
    char matched[64] = {0};
    while (sqlite3_step(st) == SQLITE_ROW) {
        count++;
        snprintf(matched, sizeof(matched), "%s", (const char *)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
    assert(count == 1);
    assert(strcmp(matched, "echo") == 0);

    db_close(db);
    printf("  PASS test_launch_all_gate_query\n");
}

static char *chan_col(sqlite3 *db, const char *col, const char *name) {
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT COALESCE(%s,'') FROM channels WHERE name=?;", col);
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    char *out = NULL;
    if (sqlite3_step(s) == SQLITE_ROW)
        out = strdup((const char *)sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    assert(out);
    return out;
}

/* swap records the revert target; revert restores it; re-swap to the same
 * extension (a restart) never clobbers the rollback pointer. No processes
 * are involved: pid is NULL so channel_bounce has nothing to signal. */
static void test_swap_and_revert(void) {
    test_db_clean(TEST_DB);
    setup_bundle();
    sqlite3 *db = test_db_open(TEST_DB);
    char *err = NULL;
    assert(extension_install(db, BUNDLE, "default", &err) == 0);
    assert(sqlite3_exec(db,
        "INSERT INTO extensions(name, path, version, owner_agent, published, enabled) "
        "VALUES('echo-ng', '/tmp/extensions/echo-ng', '0.0.0', 'default', 0, 1);",
        NULL, NULL, NULL) == SQLITE_OK);

    assert(channel_swap(db, "echo", "no-such-extension") == -2);
    assert(channel_swap(db, "no-such-channel", "echo-ng") == -1);

    assert(channel_swap(db, "echo", "echo-ng") == 0);
    char *v = chan_col(db, "extension_name", "echo");
    assert(strcmp(v, "echo-ng") == 0); free(v);
    v = chan_col(db, "prev_extension_name", "echo");
    assert(strcmp(v, "echo") == 0); free(v);

    /* Same-extension swap = restart; revert target untouched */
    assert(channel_swap(db, "echo", "echo-ng") == 0);
    v = chan_col(db, "prev_extension_name", "echo");
    assert(strcmp(v, "echo") == 0); free(v);

    char *prev = channel_prev_extension(db, "echo");
    assert(prev && strcmp(prev, "echo") == 0); free(prev);

    assert(channel_revert(db, "echo") == 0);
    v = chan_col(db, "extension_name", "echo");
    assert(strcmp(v, "echo") == 0); free(v);
    v = chan_col(db, "prev_extension_name", "echo");
    assert(v[0] == '\0'); free(v);
    assert(channel_prev_extension(db, "echo") == NULL);

    /* Nothing left to revert to */
    assert(channel_revert(db, "echo") == -1);

    db_close(db);
    printf("  PASS test_swap_and_revert\n");
}

/* Admin notices land one outbox row per admin id from channel_state. */
static void test_notify_admins(void) {
    test_db_clean(TEST_DB);
    setup_bundle();
    sqlite3 *db = test_db_open(TEST_DB);
    char *err = NULL;
    assert(extension_install(db, BUNDLE, "default", &err) == 0);
    /* admin_ids is a registry key (<ext>.admin_ids) since specs/config.md. */
    assert(sqlite3_exec(db,
        "INSERT INTO config(key, value, default_value, description)"
        " VALUES('echo.admin_ids', '111, 222', '', 'admins')"
        " ON CONFLICT(key) DO UPDATE SET value=excluded.value;",
        NULL, NULL, NULL) == SQLITE_OK);

    channel_notify_admins(db, "echo", "reverted");

    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM channel_outbox WHERE channel_name='echo'"
        " AND json_extract(payload,'$.text')='reverted';", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0) == 2);
    sqlite3_finalize(s);
    assert(sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM channel_outbox"
        " WHERE json_extract(payload,'$.chat_id') IN ('111','222');", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0) == 2);
    sqlite3_finalize(s);

    /* Channel without admin_ids: silent no-op */
    channel_notify_admins(db, "ghost", "x");

    db_close(db);
    printf("  PASS test_notify_admins\n");
}

int main(void) {
    TEST_INIT();
    printf("test_channel_lifecycle:\n");
    test_install_lands_in_draft();
    test_check_then_activate();
    test_check_reports_js_error();
    test_launch_all_gate_query();
    test_swap_and_revert();
    test_notify_admins();
    printf("all channel lifecycle tests passed\n");
    return 0;
}
