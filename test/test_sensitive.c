/* Sensitivity axis (specs/trust.md): the sensitive_targets list as loaded
 * from the DB feeds host_covered / host_in_text with registered-domain
 * semantics (the matchers themselves are covered in test_host_match). */
#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "host_match.h"
#include "test_util.h"
#include <assert.h>

static void test_sensitive_list(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int n = 99;
    assert(db_sensitive_hosts(db, &n) == NULL && n == 0);
    assert(db_sensitive_host_add(db, "") == -1);
    assert(db_sensitive_host_add(db, "chase.com") == 0);
    assert(db_sensitive_host_add(db, ".pay.example.com") == 0);
    assert(db_sensitive_host_add(db, "chase.com") == 0); /* dup: OR IGNORE */

    char **hosts = db_sensitive_hosts(db, &n);
    assert(n == 2 && hosts);
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
    hosts = db_sensitive_hosts(db, &n);
    assert(n == 1 && strcmp(hosts[0], ".pay.example.com") == 0);
    free(hosts[0]);
    free(hosts);
    sqlite3_close(db);
}

int main(void) {
    TEST_INIT();
    RUN(test_sensitive_list);
    return 0;
}
