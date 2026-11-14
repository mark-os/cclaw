/* `cclaw update` — the compatibility handshake.
 *
 * This is the part that has to be right. Schema patches are forward-only, so
 * installing a binary that cannot open the live database is not a mistake you
 * undo by swapping the binary back: the old build then refuses a database
 * stamped newer than itself. Every case below is therefore a refusal, except
 * the one that genuinely fits. */

#define _GNU_SOURCE
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

static int tests_run = 0, tests_passed = 0;

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
    if (!mkdtemp(dir)) { printf("  rollback: mkdtemp failed\n"); return; }
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
    tests_run++; printf("  rollback_no_marker_is_noop... ");
    put(db, "new-db");
    if (update_rollback_apply(db, msg, sizeof(msg)) != 0 || !has(db, "new-db"))
        printf("FAIL\n");
    else { tests_passed++; printf("PASS\n"); }

    /* Marker but no snapshot: refused, marker consumed, nothing moved. */
    tests_run++; printf("  rollback_missing_snapshot_refuses... ");
    put(bin, "new-bin"); chmod(bin, 0755);
    put(prev, "old-bin"); chmod(prev, 0755);
    write_marker(db, failed, bin);
    int rc = update_rollback_apply(db, msg, sizeof(msg));
    if (rc != -1 || access(marker, F_OK) == 0 || !has(db, "new-db") ||
        !has(bin, "new-bin") || access(failed, F_OK) == 0)
        printf("FAIL: rc=%d %s\n", rc, msg);
    else { tests_passed++; printf("PASS\n"); }

    /* Marker but no previous build: same. */
    tests_run++; printf("  rollback_missing_prev_refuses... ");
    put(snap, "snap-db");
    unlink(prev);
    write_marker(db, failed, bin);
    rc = update_rollback_apply(db, msg, sizeof(msg));
    if (rc != -1 || access(marker, F_OK) == 0 || !has(db, "new-db") ||
        !has(snap, "snap-db") || access(failed, F_OK) == 0)
        printf("FAIL: rc=%d %s\n", rc, msg);
    else { tests_passed++; printf("PASS\n"); }

    /* Everything in place: the DB, its WAL, and the binary all swap; the
     * replaced ones are kept; the marker and snapshot are consumed. */
    tests_run++; printf("  rollback_applies... ");
    put(prev, "old-bin"); chmod(prev, 0755);
    put(wal, "new-wal");
    write_marker(db, failed, bin);
    rc = update_rollback_apply(db, msg, sizeof(msg));
    char failed_wal[340];
    snprintf(failed_wal, sizeof(failed_wal), "%s-wal", failed);
    if (rc != 1) printf("FAIL: rc=%d %s\n", rc, msg);
    else if (!has(db, "snap-db")) printf("FAIL: db not restored\n");
    else if (!has(failed, "new-db")) printf("FAIL: replaced db not kept\n");
    else if (!has(failed_wal, "new-wal") || access(wal, F_OK) == 0)
        printf("FAIL: wal did not travel with its db\n");
    else if (!has(bin, "old-bin")) printf("FAIL: binary not restored\n");
    else if (!has(bad, "new-bin")) printf("FAIL: replaced binary not kept\n");
    else if (access(marker, F_OK) == 0 || access(snap, F_OK) == 0 ||
             access(prev, F_OK) == 0)
        printf("FAIL: marker/snapshot/prev not consumed\n");
    else { tests_passed++; printf("PASS\n"); }

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

    tests_run++; printf("  checksum_matches... ");
    if (update_checksum_ok(list, "cclaw-linux-x64", hello, 5, why, sizeof(why)))
        { tests_passed++; printf("PASS\n"); }
    else printf("FAIL: %s\n", why);

    tests_run++; printf("  checksum_mismatch_refuses... ");
    if (!update_checksum_ok(list, "cclaw-linux-armv5te", hello, 5, why, sizeof(why)) &&
        strstr(why, "mismatch"))
        { tests_passed++; printf("PASS\n"); }
    else printf("FAIL: %s\n", why);

    tests_run++; printf("  checksum_unlisted_refuses... ");
    if (!update_checksum_ok(list, "cclaw-linux-x86", hello, 5, why, sizeof(why)) &&
        strstr(why, "not listed"))
        { tests_passed++; printf("PASS\n"); }
    else printf("FAIL: %s\n", why);

    /* A name that is a suffix of a listed one must not match it. */
    tests_run++; printf("  checksum_suffix_is_not_a_match... ");
    if (!update_checksum_ok(list, "linux-x64", hello, 5, why, sizeof(why)))
        { tests_passed++; printf("PASS\n"); }
    else printf("FAIL\n");
}

static void expect(const char *label, const char *range, int db_version,
                   int want_ok, const char *want_reason_substr) {
    tests_run++;
    printf("  %s... ", label);
    char why[256] = "";
    int ok = update_schema_ok(range, db_version, why, sizeof(why));
    if (ok != want_ok) {
        printf("FAIL: expected %s, got %s (%s)\n",
               want_ok ? "accept" : "refuse", ok ? "accept" : "refuse", why);
        return;
    }
    if (!want_ok && want_reason_substr && !strstr(why, want_reason_substr)) {
        printf("FAIL: reason %s did not mention %s\n", why, want_reason_substr);
        return;
    }
    tests_passed++;
    printf("PASS\n");
}

/* The crash-loop marker is JSON read back with json_extract; a tag that needs
 * escaping (a sideloaded or channel build names its own) must not turn it into
 * a marker the guard cannot read — an unreadable marker restarts the count on
 * every start, so the guard never trips. */
static void test_verify_marker_escapes_tag(void) {
    tests_run++;
    sqlite3 *db = test_db_open(":memory:");
    const char *tag = "pre-7 \"quoted\" back\\slash";
    update_verify_arm(db, tag);
    if (db_scalar_i64(db,
            "SELECT json_valid(value) FROM config WHERE key='update.verify'",
            0, -1) != 1) {
        printf("  FAIL verify_marker_escapes_tag: armed marker is not JSON\n");
        sqlite3_close(db);
        return;
    }
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
    if (ok) { tests_passed++; printf("  PASS verify_marker_escapes_tag\n"); }
    else printf("  FAIL verify_marker_escapes_tag: count or tag lost on restart\n");
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
    if (!mkdtemp(dir)) { printf("  verify_rollback: mkdtemp failed\n"); return; }
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
    tests_run++; printf("  verify_rollback_failure_is_recorded_in_place... ");
    write_marker(db, failed, bin);
    update_verify_rollback(db);
    outcome_of(db, got, sizeof(got));
    if (strcmp(got, "live:crash_loop_no_revert:") != 0 || !has(bin, "new-bin"))
        printf("FAIL: %s\n", got);
    else { tests_passed++; printf("PASS\n"); }

    /* Snapshot present: it becomes the database and carries the outcome. */
    tests_run++; printf("  verify_rollback_records_in_restored_db... ");
    stage_db(snap, "snap", 0);
    write_marker(db, failed, bin);
    update_verify_rollback(db);
    outcome_of(db, got, sizeof(got));
    if (strcmp(got, "snap:crash_loop_rolled_back:") != 0 ||
        !has(bin, "old-bin") || access(failed, F_OK) != 0)
        printf("FAIL: %s\n", got);
    else { tests_passed++; printf("PASS\n"); }

    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) printf("  (could not remove %s)\n", dir);
}

/* Writing the downloaded binary: a failure that only shows at fclose (the
 * flush hitting a full disk) is still a failure, reported once. */
static void test_write_file_reports_flush_failure(void) {
    tests_run++;
    int full = update_write_file("/dev/full", "x", 1);
    char path[] = "/tmp/cclaw_test_update_wf_XXXXXX";
    int fd = mkstemp(path);
    if (fd >= 0) close(fd);
    int good = update_write_file(path, "binary", 6);
    int roundtrip = has(path, "binary");
    unlink(path);
    if (full != 0 && good == 0 && roundtrip) {
        tests_passed++; printf("  PASS write_file_reports_flush_failure\n");
    } else printf("  FAIL write_file_reports_flush_failure (full=%d good=%d)\n", full, good);
}

int main(void) {
    TEST_INIT();
    printf("test_update:\n");

    /* The ordinary case: our own range, our own database. */
    char ours[64];
    snprintf(ours, sizeof(ours), "min=%d current=%d",
             CCLAW_SCHEMA_MIN, CCLAW_SCHEMA_VERSION);
    expect("same_build_accepts", ours, CCLAW_SCHEMA_VERSION, 1, NULL);

    /* A newer build that still patches from our floor is the whole point. */
    expect("newer_build_accepts", "min=40 current=99", 51, 1, NULL);

    /* The case this feature exists for: the database predates the candidate's
     * floor, so the candidate would refuse to open it. Exactly the v30-vs-v40
     * wall the Pogoplug was behind. */
    expect("db_below_candidate_floor_refuses", "min=40 current=51", 30, 0,
           "patches forward from");

    /* Downgrade: the candidate does not know this database's shape. */
    expect("db_newer_than_candidate_refuses", "min=40 current=45", 51, 0,
           "downgrade");

    /* A binary that cannot say what it accepts is not one we can reason about,
     * which also covers every build older than --schema-range itself. */
    expect("no_answer_refuses", NULL, 51, 0, "does not answer");
    expect("empty_answer_refuses", "", 51, 0, "does not answer");
    expect("garbage_refuses", "cclaw 1307fc3 (2026-08-28)", 51, 0, "unparseable");
    expect("partial_refuses", "min=40", 51, 0, "unparseable");
    expect("nonsense_range_refuses", "min=99 current=40", 51, 0, "unparseable");
    expect("zero_min_refuses", "min=0 current=0", 51, 0, "unparseable");

    /* Boundary: a database sitting exactly on either end is fine. */
    expect("db_at_floor_accepts", "min=40 current=51", 40, 1, NULL);
    expect("db_at_ceiling_accepts", "min=40 current=51", 51, 1, NULL);

    /* Loader noise before the marker must not break the handshake — a custom
     * libcurl prints "no version information available" to stderr on every
     * exec, and the capture merges stderr (the CI websocket target found
     * this; a real deployment with a hand-built library would hit it too). */
    expect("loader_noise_prefix_accepts",
           "/opt/lib/libcurl.so.4: no version information available "
           "min=40 current=51", 51, 1, NULL);

    test_rollback_apply();
    test_checksum();
    test_verify_marker_escapes_tag();
    test_verify_rollback_records_outcome();
    test_write_file_reports_flush_failure();

    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
