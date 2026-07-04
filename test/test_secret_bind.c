/* Fail-closed credential rule (specs/trust.md): secret_hosts DB helpers and
 * the binding semantics they feed. The dispatch-gate park and the shell/js
 * egress narrowing live in main.c (integration-exercised); host_covered and
 * secret_placeholder_names have their own unit coverage. */
#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "host_match.h"
#include "secret_interp.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DB_PATH "/tmp/cclaw_test_secret_bind.db"

static void clean_db(void) {
    unlink(DB_PATH);
    unlink(DB_PATH "-wal");
    unlink(DB_PATH "-shm");
}

static sqlite3 *fresh_db(void) {
    clean_db();
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db != NULL);
    return db;
}

static void test_unbound_secret_has_no_hosts(void) {
    sqlite3 *db = fresh_db();
    int n = 99;
    char **hosts = db_secret_hosts(db, "GH_TOKEN", &n);
    assert(hosts == NULL && n == 0);   /* zero bindings → dispatch parks */
    sqlite3_close(db);
    printf("  PASS: unbound_secret_has_no_hosts\n");
}

static void test_bind_roundtrip_and_coverage(void) {
    sqlite3 *db = fresh_db();
    assert(db_secret_host_bind(db, "GH_TOKEN", "api.github.com") == 0);
    assert(db_secret_host_bind(db, "GH_TOKEN", ".githubusercontent.com") == 0);
    assert(db_secret_host_bind(db, "GH_TOKEN", "api.github.com") == 0); /* dup ok */
    int n = 0;
    char **hosts = db_secret_hosts(db, "GH_TOKEN", &n);
    assert(n == 2 && hosts);

    /* Binding semantics = host_covered: url-host checks + egress narrowing
     * both go through it. */
    assert(host_covered(hosts, (size_t)n, "api.github.com"));
    assert(host_covered(hosts, (size_t)n, "raw.githubusercontent.com"));
    assert(!host_covered(hosts, (size_t)n, "github.com"));       /* not bound */
    assert(!host_covered(hosts, (size_t)n, "evil.example.com"));
    for (int i = 0; i < n; i++) free(hosts[i]);
    free(hosts);

    /* Bindings are per-secret: another name stays unbound. */
    hosts = db_secret_hosts(db, "NPM_TOKEN", &n);
    assert(hosts == NULL && n == 0);

    assert(db_secret_host_unbind(db, "GH_TOKEN", "api.github.com") == 0);
    hosts = db_secret_hosts(db, "GH_TOKEN", &n);
    assert(n == 1 && strcmp(hosts[0], ".githubusercontent.com") == 0);
    free(hosts[0]); free(hosts);
    sqlite3_close(db);
    printf("  PASS: bind_roundtrip_and_coverage\n");
}

static void test_placeholder_to_binding_composition(void) {
    /* The gate's pipeline: placeholder names from args → bindings → coverage.
     * Pin the composition the dispatch code relies on. */
    sqlite3 *db = fresh_db();
    assert(db_secret_host_bind(db, "API_KEY", "api.example.com") == 0);

    size_t un = 0;
    char **names = secret_placeholder_names(
        "{\"url\":\"https://api.example.com/v1\",\"headers\":"
        "{\"Authorization\":\"Bearer {{SECRET:API_KEY}}\"}}", &un);
    assert(un == 1 && strcmp(names[0], "API_KEY") == 0);

    int bn = 0;
    char **bh = db_secret_hosts(db, names[0], &bn);
    assert(bn == 1);
    assert(host_covered(bh, (size_t)bn, "api.example.com"));      /* proceeds */
    assert(!host_covered(bh, (size_t)bn, "api.example.com.evil.co")); /* parks */
    for (int i = 0; i < bn; i++) free(bh[i]);
    free(bh);
    secret_names_free(names, un);
    sqlite3_close(db);
    printf("  PASS: placeholder_to_binding_composition\n");
}

static void test_invalid_args(void) {
    sqlite3 *db = fresh_db();
    assert(db_secret_host_bind(db, "", "x.com") == -1);
    assert(db_secret_host_bind(db, "N", "") == -1);
    assert(db_secret_host_bind(db, NULL, "x.com") == -1);
    assert(db_secret_host_bind(NULL, "N", "x.com") == -1);
    int n = 0;
    assert(db_secret_hosts(db, NULL, &n) == NULL && n == 0);
    sqlite3_close(db);
    printf("  PASS: invalid_args\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_secret_bind:\n");
    test_unbound_secret_has_no_hosts();
    test_bind_roundtrip_and_coverage();
    test_placeholder_to_binding_composition();
    test_invalid_args();
    clean_db();
    printf("  all tests passed\n");
    return 0;
}
