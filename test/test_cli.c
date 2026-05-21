#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "cli.h"
#include "config.h"
#include "db.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

static void test_cli_null_config(void) {
    TEST(cli_null_config);
    int rc = cli_run(NULL);
    if (rc != -1) { FAIL("expected -1 for NULL config"); return; }
    PASS();
}

static void test_cli_no_api_key(void) {
    TEST(cli_no_api_key);
    Config cfg = {0};
    cfg.db_path = "test_cli.db";
    cfg.workspace = "./workspace";
    cfg.provider.base_url = "http://localhost:9999";
    cfg.provider.model = "test";
    cfg.provider.api_key = NULL;
    int rc = cli_run(&cfg);
    if (rc != -1) { FAIL("expected -1 without api_key"); return; }
    PASS();
}

static void test_cli_session_resume(void) {
    TEST(cli_session_resume);
    /* Verify session_list returns previously created sessions for resumption */
    const char *dbpath = "test_cli_resume.db";
    unlink(dbpath);
    sqlite3 *db = db_open(dbpath);
    if (!db) { FAIL("db_open"); return; }

    int64_t s1 = session_create(db, "first");
    int64_t s2 = session_create(db, "second");
    if (s1 < 0 || s2 < 0) { FAIL("session_create"); db_close(db); unlink(dbpath); return; }

    int count = 0;
    Session *sessions = session_list(db, &count);
    if (count != 2) { FAIL("expected 2 sessions"); session_list_free(sessions, count); db_close(db); unlink(dbpath); return; }
    if (sessions[0].id != s1 || sessions[1].id != s2) { FAIL("wrong session ids"); session_list_free(sessions, count); db_close(db); unlink(dbpath); return; }

    /* Verify resumed session preserves entries */
    Message msg = {.role = ROLE_USER, .content = "hello"};
    entry_append(db, s1, &msg);

    int branch_count = 0;
    Entry *branch = session_get_branch(db, s1, &branch_count);
    if (branch_count != 1) { FAIL("expected 1 entry on resume"); entry_branch_free(branch, branch_count); session_list_free(sessions, count); db_close(db); unlink(dbpath); return; }
    if (strcmp(branch[0].message.content, "hello") != 0) { FAIL("wrong content"); entry_branch_free(branch, branch_count); session_list_free(sessions, count); db_close(db); unlink(dbpath); return; }

    entry_branch_free(branch, branch_count);
    session_list_free(sessions, count);
    db_close(db);
    unlink(dbpath);
    PASS();
}

int main(void) {
    printf("test_cli:\n");
    test_cli_null_config();
    test_cli_no_api_key();
    test_cli_session_resume();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
