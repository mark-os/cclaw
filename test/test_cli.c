#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <stdlib.h>
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
    CliOpts opts = {.session_id = -1};
    int rc = cli_run(NULL, &opts);
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
    CliOpts opts = {.session_id = -1};
    int rc = cli_run(&cfg, &opts);
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

    int64_t s1 = session_create(db, "first", NULL, -1, 0);
    int64_t s2 = session_create(db, "second", NULL, -1, 0);
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

/* T223: Verify CLI zero-config creates dirs + DB from scratch */
static void test_cli_zero_config_startup(void) {
    TEST(cli_zero_config_startup);

    /* Clean slate */
    system("rm -rf .cclaw_test_zc");

    /* Simulate zero-config: chdir to temp dir, verify defaults create structure */
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) { FAIL("getcwd"); return; }
    if (mkdir(".cclaw_test_zc", 0755) != 0) { FAIL("mkdir"); return; }
    if (chdir(".cclaw_test_zc") != 0) { FAIL("chdir"); return; }

    /* config_load_from_env with no env vars → defaults */
    unsetenv("CCLAW_WORKSPACE");
    unsetenv("CCLAW_AGENT_DB");
    Config *cfg = config_load_from_env();
    if (!cfg) { chdir(cwd); system("rm -rf .cclaw_test_zc"); FAIL("config_load_from_env"); return; }

    /* Verify defaults match T223 spec */
    if (strcmp(cfg->workspace, ".cclaw/agents/default/workspace") != 0) {
        chdir(cwd); system("rm -rf .cclaw_test_zc"); config_free(cfg);
        FAIL("workspace default wrong"); return;
    }
    if (strcmp(cfg->db_path, ".cclaw/agents/default/agent.db") != 0) {
        chdir(cwd); system("rm -rf .cclaw_test_zc"); config_free(cfg);
        FAIL("db_path default wrong"); return;
    }

    /* Verify workspace_init creates the directory */
    workspace_init(cfg);
    struct stat st;
    if (stat(".cclaw/agents/default/workspace", &st) != 0 || !S_ISDIR(st.st_mode)) {
        chdir(cwd); system("rm -rf .cclaw_test_zc"); config_free(cfg);
        FAIL("workspace dir not created"); return;
    }

    /* Verify DB can be opened (ensure_parent_dir + db_open_agent) */
    system("mkdir -p .cclaw/agents/default");
    sqlite3 *db = db_open_agent(".cclaw/agents/default/agent.db");
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
    test_cli_null_config();
    test_cli_no_api_key();
    test_cli_session_resume();
    test_cli_zero_config_startup();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
