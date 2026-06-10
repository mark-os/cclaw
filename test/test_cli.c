#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <stdlib.h>
#include "config.h"
#include "db.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

static void test_cli_session_resume(void) {
    TEST(cli_session_resume);
    const char *dbpath = "test_cli_resume.db";
    unlink(dbpath);
    sqlite3 *db = test_db_open(dbpath);
    if (!db) { FAIL("db_open"); return; }

    int64_t s1 = session_create(db, "first", NULL, -1, 0);
    int64_t s2 = session_create(db, "second", NULL, -1, 0);
    if (s1 < 0 || s2 < 0) { FAIL("session_create"); db_close(db); unlink(dbpath); return; }

    int count = 0;
    Session *sessions = session_list(db, &count);
    if (count != 2) { FAIL("expected 2 sessions"); session_list_free(sessions, count); db_close(db); unlink(dbpath); return; }
    if (sessions[0].id != s1 || sessions[1].id != s2) { FAIL("wrong session ids"); session_list_free(sessions, count); db_close(db); unlink(dbpath); return; }

    Message msg = {.role = ROLE_USER, .content = "hello"};
    entry_append_with_turn(db, s1, &msg, 1);

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

static void test_cli_zero_config_startup(void) {
    TEST(cli_zero_config_startup);

    system("rm -rf .cclaw_test_zc");

    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) { FAIL("getcwd"); return; }
    if (mkdir(".cclaw_test_zc", 0755) != 0) { FAIL("mkdir"); return; }
    if (chdir(".cclaw_test_zc") != 0) { FAIL("chdir"); return; }

    unsetenv("CCLAW_WORKSPACE");
    unsetenv("CCLAW_DB");
    Config *cfg = config_load_from_env();
    if (!cfg) { chdir(cwd); system("rm -rf .cclaw_test_zc"); FAIL("config_load_from_env"); return; }

    if (strcmp(cfg->workspace, ".cclaw/agents/default/workspace") != 0) {
        chdir(cwd); system("rm -rf .cclaw_test_zc"); config_free(cfg);
        FAIL("workspace default wrong"); return;
    }
    if (strcmp(cfg->db_path, "cclaw.db") != 0) {
        chdir(cwd); system("rm -rf .cclaw_test_zc"); config_free(cfg);
        FAIL("db_path default wrong"); return;
    }

    workspace_init(cfg);
    struct stat st;
    if (stat(".cclaw/agents/default/workspace", &st) != 0 || !S_ISDIR(st.st_mode)) {
        chdir(cwd); system("rm -rf .cclaw_test_zc"); config_free(cfg);
        FAIL("workspace dir not created"); return;
    }

    system("mkdir -p .cclaw/agents/default");
    sqlite3 *db = test_db_open(".cclaw/agents/default/agent.db");
    if (!db) {
        chdir(cwd); system("rm -rf .cclaw_test_zc"); config_free(cfg);
        FAIL("db_open_agent failed"); return;
    }
    db_close(db);

    config_free(cfg);
    chdir(cwd);
    system("rm -rf .cclaw_test_zc");
    PASS();
}

int main(void) {
    alarm(10);
    printf("test_cli:\n");
    test_cli_session_resume();
    test_cli_zero_config_startup();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
