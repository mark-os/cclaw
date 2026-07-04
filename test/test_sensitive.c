/* Sensitivity axis (specs/trust.md): sensitive_targets DB helpers, and the
 * label semantics they feed (host_covered / host_in_text are covered in
 * test_host_match; proxy deny-first is covered in test_proxy_decide). */
#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "host_match.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DB_PATH "/tmp/cclaw_test_sensitive.db"

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

static void test_empty_list(void) {
    sqlite3 *db = fresh_db();
    int n = 99;
    char **hosts = db_sensitive_hosts(db, &n);
    assert(hosts == NULL && n == 0);
    sqlite3_close(db);
    printf("  PASS: empty_list\n");
}

static void test_add_list_rm_roundtrip(void) {
    sqlite3 *db = fresh_db();
    assert(db_sensitive_host_add(db, "chase.com") == 0);
    assert(db_sensitive_host_add(db, ".pay.example.com") == 0);
    assert(db_sensitive_host_add(db, "chase.com") == 0); /* dup: OR IGNORE */
    int n = 0;
    char **hosts = db_sensitive_hosts(db, &n);
    assert(n == 2 && hosts);
    /* ORDER BY value: ".pay.example.com" < "chase.com" */
    assert(strcmp(hosts[0], ".pay.example.com") == 0);
    assert(strcmp(hosts[1], "chase.com") == 0);

    /* The loaded list feeds host_covered/host_in_text with registered-domain
     * semantics — pin the composition end-to-end. */
    assert(host_covered(hosts, (size_t)n, "api.chase.com"));
    assert(!host_covered(hosts, (size_t)n, "mychase.com"));
    char got[64];
    assert(host_in_text(hosts, (size_t)n,
                        "{\"url\":\"https://pay.example.com/x\"}",
                        got, sizeof(got)));
    assert(strcmp(got, "pay.example.com") == 0);

    for (int i = 0; i < n; i++) free(hosts[i]);
    free(hosts);

    assert(db_sensitive_host_rm(db, "chase.com") == 0);
    assert(db_sensitive_host_rm(db, "never-added.example") == 0); /* absent ok */
    hosts = db_sensitive_hosts(db, &n);
    assert(n == 1 && strcmp(hosts[0], ".pay.example.com") == 0);
    free(hosts[0]); free(hosts);
    sqlite3_close(db);
    printf("  PASS: add_list_rm_roundtrip\n");
}

static void test_invalid_args(void) {
    sqlite3 *db = fresh_db();
    assert(db_sensitive_host_add(db, "") == -1);
    assert(db_sensitive_host_add(db, NULL) == -1);
    assert(db_sensitive_host_add(NULL, "x.com") == -1);
    int n = 0;
    assert(db_sensitive_hosts(NULL, &n) == NULL && n == 0);
    sqlite3_close(db);
    printf("  PASS: invalid_args\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_sensitive:\n");
    test_empty_list();
    test_add_list_rm_roundtrip();
    test_invalid_args();
    clean_db();
    printf("  all tests passed\n");
    return 0;
}
