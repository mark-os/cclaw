/* `cclaw update` — the compatibility handshake.
 *
 * This is the part that has to be right. Schema patches are forward-only, so
 * installing a binary that cannot open the live database is not a mistake you
 * undo by swapping the binary back: the old build then refuses a database
 * stamped newer than itself. Every case below is therefore a refusal, except
 * the one that genuinely fits. */

#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "update.h"
#include "db.h"
#include "config_registry.h"
#include "cclaw.h"
#include "test_util.h"

/* ── rollback apply: the file swap, on a scratch directory ─────────
 * No daemon and no real database: the applier only renames files, so fake
 * ones with distinct contents prove which file ended up where. */

static void put(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (f) { fputs(content, f); fclose(f); }
}

static int has(const char *path, const char *content) {
    char buf[64] = "";
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strcmp(buf, content) == 0;
}

static void write_marker(const char *db, const char *failed, const char *bin) {
    char m[512];
    snprintf(m, sizeof(m), "%s.rollback", db);
    FILE *f = fopen(m, "w");
    if (f) { fprintf(f, "%s\n%s\n", failed, bin); fclose(f); }
}

static void test_rollback_apply(void) {
    char dir[] = "/tmp/cclaw_rollback_XXXXXX";
    assert(mkdtemp(dir));
    char db[300], wal[320], snap[320], bin[300], prev[320], bad[320], failed[320],
         marker[320], msg[1024];
    snprintf(db, sizeof(db), "%s/cclaw.db", dir);
    snprintf(wal, sizeof(wal), "%s/cclaw.db-wal", dir);
    snprintf(snap, sizeof(snap), "%s/cclaw.db.preupdate", dir);
    snprintf(bin, sizeof(bin), "%s/cclaw", dir);
    snprintf(prev, sizeof(prev), "%s/cclaw.prev", dir);
    snprintf(bad, sizeof(bad), "%s/cclaw.bad", dir);
    snprintf(failed, sizeof(failed), "%s/cclaw.db.failed-v9", dir);
    snprintf(marker, sizeof(marker), "%s.rollback", db);

    /* No marker: a no-op that touches nothing. */
    put(db, "new-db");
    assert(update_rollback_apply(db, msg, sizeof(msg)) == 0 && has(db, "new-db"));

    /* Marker but no snapshot: refused, marker consumed, nothing moved. */
    put(bin, "new-bin"); chmod(bin, 0755);
    put(prev, "old-bin"); chmod(prev, 0755);
    write_marker(db, failed, bin);
    assert(update_rollback_apply(db, msg, sizeof(msg)) == -1);
    assert(access(marker, F_OK) != 0 && has(db, "new-db") && has(bin, "new-bin") &&
           access(failed, F_OK) != 0);

    /* Marker but no previous build: same. */
    put(snap, "snap-db");
    unlink(prev);
    write_marker(db, failed, bin);
    assert(update_rollback_apply(db, msg, sizeof(msg)) == -1);
    assert(access(marker, F_OK) != 0 && has(db, "new-db") && has(snap, "snap-db") &&
           access(failed, F_OK) != 0);

    /* Everything in place: the DB, its WAL, and the binary all swap; the
     * replaced ones are kept; the marker and snapshot are consumed. */
    put(prev, "old-bin"); chmod(prev, 0755);
    put(wal, "new-wal");
    write_marker(db, failed, bin);
    int rc = update_rollback_apply(db, msg, sizeof(msg));
    if (rc != 1) fprintf(stderr, "rollback: rc=%d %s\n", rc, msg);
    assert(rc == 1);
    char failed_wal[340];
    snprintf(failed_wal, sizeof(failed_wal), "%s-wal", failed);
    assert(has(db, "snap-db"));                                   /* db restored */
    assert(has(failed, "new-db"));                                /* replaced db kept */
    assert(has(failed_wal, "new-wal") && access(wal, F_OK) != 0); /* wal travels with its db */
    assert(has(bin, "old-bin") && has(bad, "new-bin"));           /* binary swapped, kept */
    assert(access(marker, F_OK) != 0 && access(snap, F_OK) != 0 && access(prev, F_OK) != 0);

    unlink(db); unlink(failed); unlink(failed_wal); unlink(bin); unlink(bad);
    unlink(snap); unlink(prev); unlink(wal); unlink(marker);
    rmdir(dir);
}

/* ── checksums.b2 ── */

static void test_checksum(void) {
    /* blake2b-512 of "hello" — b2sum's answer. */
    static const char *list =
        "e4cfa39a3d37be31c59609e807970799caa68a19bfaa15135f165085e01d41a6"
        "5ba1e1b146aeb6bd0092b49eac214c103ccfa3a365954bbbe52f74a2b3620c94"
        "  cclaw-linux-x64\n"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000000"
        " *cclaw-linux-armv5te\n";
    char why[256] = "";
    const unsigned char *hello = (const unsigned char *)"hello";

    assert(update_checksum_ok(list, "cclaw-linux-x64", hello, 5, why, sizeof(why)));
    assert(!update_checksum_ok(list, "cclaw-linux-armv5te", hello, 5, why, sizeof(why)) &&
           strstr(why, "mismatch"));
    assert(!update_checksum_ok(list, "cclaw-linux-x86", hello, 5, why, sizeof(why)) &&
           strstr(why, "not listed"));
    /* A name that is a suffix of a listed one must not match it. */
    assert(!update_checksum_ok(list, "linux-x64", hello, 5, why, sizeof(why)));
}

/* The crash-loop marker is JSON read back with json_extract; a tag that needs
 * escaping (a sideloaded or channel build names its own) must not turn it into
 * a marker the guard cannot read — an unreadable marker restarts the count on
 * every start, so the guard never trips. */
static void test_verify_marker_escapes_tag(void) {
    sqlite3 *db = test_db_open(":memory:");
    const char *tag = "pre-7 \"quoted\" back\\slash";
    update_verify_arm(db, tag);
    assert(test_qi(db, "SELECT json_valid(value) FROM config WHERE key='update.verify'") == 1);
    /* Each start rewrites the marker; the count must survive the rewrite. */
    update_verify_startup(db);
    update_verify_startup(db);
    sqlite3_stmt *st = NULL;
    int ok = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT json_extract(value,'$.starts')=2 AND json_extract(value,'$.tag')=?1"
            " FROM config WHERE key='update.verify' AND json_valid(value)",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, tag, -1, SQLITE_STATIC);
        ok = sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    assert(ok);   /* count and tag survive the restart */
}

/* The guard's two forward events. "Served" is sticky and asks the agent, once,
 * to say the update landed; "verified" is the first turn that did not end in
 * an error, and nothing else — not uptime, not a turn that failed. */
static void test_verify_events(void) {
    sqlite3 *db = test_db_open(":memory:");
    char *def = config_get(db, "default_agent");
    const char *agent = def && def[0] ? def : "bot";
    db_agent_upsert(db, agent, NULL, NULL);
    int64_t sid = session_create(db, "chat", agent, -1, 0);
    free(def);
    update_verify_arm(db, "v9");

    update_verify_serving(db);
    update_verify_serving(db);
    update_verify_startup(db);   /* a restart must not lose the flag */
    int served = (int)db_scalar_i64(db,
        "SELECT json_extract(value,'$.served') FROM config WHERE key='update.verify'", 0, -1);
    int notes = (int)db_scalar_i64(db,
        "SELECT COUNT(*) FROM inbox WHERE source='update' AND payload LIKE '%v9%'", 0, -1);
    assert(served == 1 && notes == 1);

    Message bad = { .role = ROLE_ASSISTANT, .content = "provider error",
                    .stop_reason = STOP_REASON_ERROR };
    entry_append_with_iteration(db, sid, &bad, 0);
    update_verify_turn_done(db, sid);
    int armed = (int)db_scalar_i64(db,
        "SELECT value<>'' FROM config WHERE key='update.verify'", 0, -1);
    assert(armed == 1);   /* a failed turn does not clear the marker */

    Message good = { .role = ROLE_ASSISTANT, .content = "updated to v9",
                     .stop_reason = STOP_REASON_STOP };
    entry_append_with_iteration(db, sid, &good, 0);
    update_verify_turn_done(db, sid);
    armed = (int)db_scalar_i64(db,
        "SELECT value<>'' FROM config WHERE key='update.verify'", 0, -1);
    int verified = (int)db_scalar_i64(db,
        "SELECT json_extract(value,'$.outcome')='verified' AND"
        " json_extract(value,'$.tag')='v9' FROM config WHERE key='update.last'", 0, -1);
    assert(armed == 0 && verified == 1);
    sqlite3_close(db);
}

/* The guard's own rollback (a build that migrated, then crash-looped): the
 * outcome has to be written to whichever database is at the path afterwards —
 * the one it was decided in has just been moved aside. */
static void outcome_of(const char *path, char *out, size_t cap) {
    out[0] = '\0';
    sqlite3 *db = test_db_open(path);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT COALESCE((SELECT value FROM config WHERE key='which'),'') || ':' ||"
            " COALESCE((SELECT json_extract(value,'$.outcome') FROM config"
            "           WHERE key='update.last'),'') || ':' ||"
            " COALESCE((SELECT value FROM config WHERE key='update.verify'),'')",
            -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW)
        snprintf(out, cap, "%s", (const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    sqlite3_close(db);
}

static void stage_db(const char *path, const char *which, int armed) {
    sqlite3 *db = test_db_open(path);
    char sql[128];
    snprintf(sql, sizeof(sql),
             "INSERT INTO config(key, value) VALUES('which', '%s')", which);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
    if (armed) update_verify_arm(db, "v9");
    sqlite3_close(db);
}

static void test_verify_rollback_records_outcome(void) {
    char dir[] = "/tmp/cclaw_vrollback_XXXXXX";
    assert(mkdtemp(dir));
    char db[300], snap[320], bin[300], prev[320], failed[320], got[256], cmd[400];
    snprintf(db, sizeof(db), "%s/cclaw.db", dir);
    snprintf(snap, sizeof(snap), "%s/cclaw.db.preupdate", dir);
    snprintf(bin, sizeof(bin), "%s/cclaw", dir);
    snprintf(prev, sizeof(prev), "%s/cclaw.prev", dir);
    snprintf(failed, sizeof(failed), "%s/cclaw.db.failed-v9", dir);
    put(bin, "new-bin"); chmod(bin, 0755);
    put(prev, "old-bin"); chmod(prev, 0755);
    stage_db(db, "live", 1);

    /* Nothing to go back to: nothing moves, and the live database says so and
     * stops counting, rather than retrying the same rollback at every start. */
    write_marker(db, failed, bin);
    update_verify_rollback(db);
    outcome_of(db, got, sizeof(got));
    if (strcmp(got, "live:crash_loop_no_revert:") != 0) fprintf(stderr, "got %s\n", got);
    assert(strcmp(got, "live:crash_loop_no_revert:") == 0 && has(bin, "new-bin"));

    /* Snapshot present: it becomes the database and carries the outcome. */
    stage_db(snap, "snap", 0);
    write_marker(db, failed, bin);
    update_verify_rollback(db);
    outcome_of(db, got, sizeof(got));
    if (strcmp(got, "snap:crash_loop_rolled_back:") != 0) fprintf(stderr, "got %s\n", got);
    assert(strcmp(got, "snap:crash_loop_rolled_back:") == 0 && has(bin, "old-bin") &&
           access(failed, F_OK) == 0);

    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) printf("  (could not remove %s)\n", dir);
}

/* Writing the downloaded binary: a failure that only shows at fclose (the
 * flush hitting a full disk) is still a failure, reported once. */
static void test_write_file_reports_flush_failure(void) {
    int full = update_write_file("/dev/full", "x", 1);
    char path[] = "/tmp/cclaw_test_update_wf_XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) close(fd);
    int good = update_write_file(path, "binary", 6);
    int roundtrip = has(path, "binary");
    unlink(path);
    assert(full != 0 && good == 0 && roundtrip);
}

/* The handshake: update_schema_ok(range the candidate prints, live DB
 * version). Every row but the fitting ones is a refusal, with a reason. */
static void test_schema_handshake(void) {
    char ours[64];
    snprintf(ours, sizeof(ours), "min=%d current=%d", CCLAW_SCHEMA_MIN, CCLAW_SCHEMA_VERSION);
    const struct { const char *range; int db; int ok; const char *why; } cases[] = {
        /* The ordinary case: our own range, our own database. */
        { ours, CCLAW_SCHEMA_VERSION, 1, NULL },
        /* A newer build that still patches from our floor is the whole point. */
        { "min=40 current=99", 51, 1, NULL },
        /* The database predates the candidate's floor, so the candidate would
         * refuse to open it: the v30-vs-v40 wall the Pogoplug was behind. */
        { "min=40 current=51", 30, 0, "patches forward from" },
        /* Downgrade: the candidate does not know this database's shape. */
        { "min=40 current=45", 51, 0, "downgrade" },
        /* A binary that cannot say what it accepts, which also covers every
         * build older than --schema-range itself. */
        { NULL, 51, 0, "does not answer" },
        { "", 51, 0, "does not answer" },
        { "cclaw 1307fc3 (2026-08-28)", 51, 0, "unparseable" },
        { "min=40", 51, 0, "unparseable" },
        { "min=99 current=40", 51, 0, "unparseable" },
        { "min=0 current=0", 51, 0, "unparseable" },
        /* Boundary: a database sitting exactly on either end is fine. */
        { "min=40 current=51", 40, 1, NULL },
        { "min=40 current=51", 51, 1, NULL },
        /* Loader noise before the marker must not break the handshake: a
         * custom libcurl prints "no version information available" to stderr
         * on every exec, and the capture merges stderr (found by the CI
         * websocket target). */
        { "/opt/lib/libcurl.so.4: no version information available min=40 current=51", 51, 1, NULL },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char why[256] = "";
        int ok = update_schema_ok(cases[i].range, cases[i].db, why, sizeof(why));
        int good = ok == cases[i].ok && (ok || !cases[i].why || strstr(why, cases[i].why));
        if (!good) fprintf(stderr, "row %zu (%s, db %d): ok=%d why=%s\n", i,
                           cases[i].range ? cases[i].range : "NULL", cases[i].db, ok, why);
        assert(good);
    }
}

int main(void) {
    TEST_INIT();
    RUN(test_schema_handshake);
    RUN(test_rollback_apply);
    RUN(test_checksum);
    RUN(test_verify_marker_escapes_tag);
    RUN(test_verify_events);
    RUN(test_verify_rollback_records_outcome);
    RUN(test_write_file_reports_flush_failure);
    printf("test_update: all tests passed\n");
    return 0;
}
