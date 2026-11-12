/* Update channels: publish → signed manifest → verified fetch, and every way
 * a channel must refuse (wrong key, tampered manifest, tampered binary,
 * missing key, missing asset, unsafe names). Runs entirely on a scratch
 * directory with file:// URLs; https shares the same verify/parse path. */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "monocypher.h"
#include "update.h"
#include "update_channel.h"
#include "config_registry.h"
#include "util.h"
#include "test_util.h"

#define BINARY "./build/cclaw"
#define ASSET  "cclaw-linux-test"

static int tests_run = 0, tests_passed = 0;

static void check(const char *name, int ok) {
    tests_run++;
    if (ok) { tests_passed++; printf("  PASS %s\n", name); }
    else printf("  FAIL %s\n", name);
}

static void hexs(const uint8_t *in, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", in[i]);
}

/* A key pair from a fixed seed: the test knows both halves. */
static void make_key(const char *path, uint8_t seedbyte, char pub_hex[65]) {
    uint8_t seed[32], sk[64], pk[32];
    memset(seed, seedbyte, sizeof(seed));
    crypto_eddsa_key_pair(sk, pk, seed);
    char skh[129];
    hexs(sk, 64, skh);
    hexs(pk, 32, pub_hex);
    FILE *f = fopen(path, "w");
    fprintf(f, "%s\n", skh);
    fclose(f);
    chmod(path, 0600);
}

static int publish(const char *dir, const char *key, const char *notes, const char *keep) {
    char *argv[] = { "cclaw", "publish", "--dir", (char *)dir, "--binary", BINARY,
                     "--asset", ASSET, "--key", (char *)key, "--notes", (char *)notes,
                     "--keep", (char *)keep, NULL };
    return publish_main(13 + 1, argv);
}

static int exists(const char *dir, const char *name) {
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    return access(p, F_OK) == 0;
}

static void test_tags(void) {
    char t[32];
    uchan_tag(42, t, sizeof(t));
    check("tag_format", strcmp(t, "pre-42") == 0);
    check("tag_roundtrip", uchan_tag_seq(t) == 42);
    check("tag_release_is_not_seq", uchan_tag_seq("v0.5.0") == -1);
    check("tag_garbage", uchan_tag_seq("pre-") == -1 && uchan_tag_seq("pre-4x") == -1 &&
                         uchan_tag_seq("pre-0") == -1 && uchan_tag_seq(NULL) == -1);
    check("channel_kinds", uchan_is_channel("file:///x") && uchan_is_channel("https://h/x") &&
                           !uchan_is_channel("github:o/r") && !uchan_is_channel("") &&
                           !uchan_is_channel("http://insecure/x") && !uchan_is_channel(NULL));
}

static void test_parse_rejects(void) {
    UchanRelease r;
    char why[256];
    char b2[129];
    memset(b2, '0', 128);
    b2[128] = '\0';
    char j[1024];
    snprintf(j, sizeof(j), "{\"format\":1,\"seq\":3,\"assets\":{\"a\":"
             "{\"file\":\"../../etc/passwd\",\"b2\":\"%s\",\"size\":1}}}", b2);
    check("parse_rejects_path_traversal", uchan_parse_manifest(j, "a", &r, why, sizeof(why)) != 0);
    snprintf(j, sizeof(j), "{\"format\":2,\"seq\":3,\"assets\":{}}");
    check("parse_rejects_unknown_format", uchan_parse_manifest(j, "a", &r, why, sizeof(why)) != 0);
    check("parse_rejects_bad_asset_name",
          uchan_parse_manifest("{\"format\":1}", "a\"b", &r, why, sizeof(why)) != 0);
    snprintf(j, sizeof(j), "{\"format\":1,\"seq\":3,\"assets\":{\"a\":"
             "{\"file\":\"a-3\",\"b2\":\"%s\",\"size\":1}}}", b2);
    int ok = uchan_parse_manifest(j, "a", &r, why, sizeof(why)) == 0 && r.seq == 3 &&
             strcmp(r.file, "a-3") == 0;
    check("parse_accepts_minimal", ok);
    uchan_release_free(&r);
}

static void test_publish_and_verify(void) {
    char tmpl[] = "/tmp/cclaw_test_uchan_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) { check("mkdtemp", 0); return; }
    char key[512], other[512], url[600], pub[65], otherpub[65], why[512];
    snprintf(key, sizeof(key), "%s/publish.key", dir);
    snprintf(other, sizeof(other), "%s/other.key", dir);
    char chan[512];
    snprintf(chan, sizeof(chan), "%s/chan", dir);
    mkdir(chan, 0755);
    snprintf(url, sizeof(url), "file://%s", chan);
    make_key(key, 7, pub);
    make_key(other, 9, otherpub);

    check("publish_1", publish(chan, key, "first build", "5") == 0);
    UchanRelease r;
    int ok = uchan_latest(url, pub, ASSET, &r, why, sizeof(why)) == 0;
    check("latest_verifies", ok);
    if (!ok) printf("    why: %s\n", why);
    check("latest_seq_1", ok && r.seq == 1);
    check("latest_notes", ok && r.notes && strcmp(r.notes, "first build") == 0);
    check("latest_version_from_binary", ok && r.version && strncmp(r.version, "cclaw ", 6) == 0);
    char *data = NULL; size_t len = 0;
    check("asset_verifies", ok && uchan_fetch_asset(url, &r, &data, &len, why, sizeof(why)) == 0);
    free(data);
    uchan_release_free(&r);

    check("publish_2_increments", publish(chan, key, "second", "1") == 0 &&
          uchan_latest(url, pub, ASSET, &r, why, sizeof(why)) == 0 && r.seq == 2);
    uchan_release_free(&r);
    check("prune_keeps_newest", exists(chan, ASSET "-2") && !exists(chan, ASSET "-1"));

    check("wrong_key_refuses", uchan_latest(url, otherpub, ASSET, &r, why, sizeof(why)) != 0 &&
          strstr(why, "signature"));
    check("missing_key_refuses", uchan_latest(url, "", ASSET, &r, why, sizeof(why)) != 0 &&
          strstr(why, "pubkey"));
    check("missing_asset_refuses",
          uchan_latest(url, pub, "cclaw-linux-other", &r, why, sizeof(why)) != 0);

    /* Tampered binary: the manifest still verifies, the asset must not. */
    char ap[700];
    snprintf(ap, sizeof(ap), "%s/%s-2", chan, ASSET);
    FILE *f = fopen(ap, "ab"); fputc('X', f); fclose(f);
    ok = uchan_latest(url, pub, ASSET, &r, why, sizeof(why)) == 0;
    check("tampered_asset_refuses", ok &&
          uchan_fetch_asset(url, &r, &data, &len, why, sizeof(why)) != 0 && data == NULL);
    uchan_release_free(&r);

    /* Tampered manifest: one byte of the signed JSON changed. */
    char mp[700];
    snprintf(mp, sizeof(mp), "%s/manifest.signed", chan);
    size_t mlen = 0;
    char *m = util_read_file(mp, &mlen);
    char *hit = m ? strstr(m, "second") : NULL;
    if (hit) hit[0] = 'S';
    f = fopen(mp, "wb"); if (m) fwrite(m, 1, mlen, f); fclose(f);
    free(m);
    check("tampered_manifest_refuses", hit &&
          uchan_latest(url, pub, ASSET, &r, why, sizeof(why)) != 0 && strstr(why, "signature"));

    char *kargv[] = { "cclaw", "publish", "--keygen", "--key", key, NULL };
    check("keygen_refuses_overwrite", publish_main(5, kargv) != 0);

    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) printf("  (cleanup of %s failed)\n", dir);
}

static int update_inbox_count(sqlite3 *db, const char *tag) {
    sqlite3_stmt *st = NULL;
    int n = -1;
    if (sqlite3_prepare_v2(db, "SELECT count(*) FROM inbox WHERE source='update'"
                               " AND source_ref=?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, tag, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

/* The daemon side: a local publish reaches the default agent's inbox on the
 * next tick, exactly once, and a second publish in the same second is still
 * seen (the change stamp is inode + ns mtime, not a 1 s mtime). */
static void test_daemon_notice(void) {
    char tmpl[] = "/tmp/cclaw_test_uchan_tick_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) { check("mkdtemp_tick", 0); return; }
    char key[512], chan[512], url[600], dbp[512], pub[65];
    snprintf(key, sizeof(key), "%s/k", dir);
    snprintf(chan, sizeof(chan), "%s/chan", dir);
    snprintf(dbp, sizeof(dbp), "%s/t.db", dir);
    mkdir(chan, 0755);
    snprintf(url, sizeof(url), "file://%s", chan);
    make_key(key, 11, pub);

    sqlite3 *db = test_db_open(dbp);
    /* The note goes to the default agent's most recent session. */
    char *agent = config_get(db, "default_agent");
    if (!agent || !agent[0]) { free(agent); agent = test_strdup_("assistant"); }
    test_seed_agent(db, agent);
    sqlite3_stmt *ins = NULL;
    sqlite3_prepare_v2(db, "INSERT INTO sessions(agent_name) VALUES(?)", -1, &ins, NULL);
    sqlite3_bind_text(ins, 1, agent, -1, SQLITE_STATIC);
    sqlite3_step(ins);
    sqlite3_finalize(ins);
    free(agent);
    config_set(db, "update.channel", url);
    config_set(db, "update.pubkey", pub);
    config_set(db, "update.asset", ASSET);

    update_check_tick(db);   /* no manifest yet: nothing, no crash */
    check("tick_empty_channel_quiet", update_inbox_count(db, "pre-1") == 0);

    publish(chan, key, "tick one", "5");
    update_check_tick(db);
    check("tick_notices_publish", update_inbox_count(db, "pre-1") == 1);
    char *nt = config_get(db, "update.notified_tag");
    check("tick_sets_notified", nt && strcmp(nt, "pre-1") == 0);
    free(nt);
    update_check_tick(db);
    check("tick_notifies_once", update_inbox_count(db, "pre-1") == 1);

    publish(chan, key, "tick two", "5");   /* same second as the first */
    update_check_tick(db);
    check("tick_sees_same_second_publish", update_inbox_count(db, "pre-2") == 1);

    config_set(db, "update.installed_tag", "pre-3");   /* already ahead */
    publish(chan, key, "tick three", "5");
    update_check_tick(db);
    check("tick_skips_installed_or_older", update_inbox_count(db, "pre-3") == 0);

    sqlite3_close(db);
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) printf("  (cleanup of %s failed)\n", dir);
}

/* A publish that lands before the default agent has any session is not lost:
 * the manifest is only marked seen once someone has been told. */
static void test_notice_waits_for_session(void) {
    char tmpl[] = "/tmp/cclaw_test_uchan_wait_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) { check("mkdtemp_wait", 0); return; }
    char key[512], chan[512], url[600], dbp[512], pub[65];
    snprintf(key, sizeof(key), "%s/k", dir);
    snprintf(chan, sizeof(chan), "%s/chan", dir);
    snprintf(dbp, sizeof(dbp), "%s/t.db", dir);
    mkdir(chan, 0755);
    snprintf(url, sizeof(url), "file://%s", chan);
    make_key(key, 13, pub);

    sqlite3 *db = test_db_open(dbp);
    char *agent = config_get(db, "default_agent");
    if (!agent || !agent[0]) { free(agent); agent = test_strdup_("assistant"); }
    test_seed_agent(db, agent);
    config_set(db, "update.channel", url);
    config_set(db, "update.pubkey", pub);
    config_set(db, "update.asset", ASSET);

    publish(chan, key, "early", "5");
    update_check_tick(db);
    check("wait_no_session_no_note", update_inbox_count(db, "pre-1") == 0);

    sqlite3_stmt *ins = NULL;
    sqlite3_prepare_v2(db, "INSERT INTO sessions(agent_name) VALUES(?)", -1, &ins, NULL);
    sqlite3_bind_text(ins, 1, agent, -1, SQLITE_STATIC);
    sqlite3_step(ins);
    sqlite3_finalize(ins);
    free(agent);
    update_check_tick(db);
    check("wait_delivered_once_session_exists", update_inbox_count(db, "pre-1") == 1);

    sqlite3_close(db);
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) printf("  (cleanup of %s failed)\n", dir);
}

int main(void) {
    TEST_INIT();
    printf("test_update_channel:\n");
    test_tags();
    test_parse_rejects();
    test_publish_and_verify();
    test_daemon_notice();
    test_notice_waits_for_session();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
