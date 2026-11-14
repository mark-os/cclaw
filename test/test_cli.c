#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <stdlib.h>
#include "config.h"
#include "db.h"
#include "test_util.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

static void test_cli_zero_config_startup(void) {
    TEST(cli_zero_config_startup);

    system("rm -rf .cclaw_test_zc");

    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) { FAIL("getcwd"); return; }
    if (mkdir(".cclaw_test_zc", 0755) != 0) { FAIL("mkdir"); return; }
    if (chdir(".cclaw_test_zc") != 0) { FAIL("chdir"); return; }

    unsetenv("CCLAW_WORKSPACE");
    unsetenv("CCLAW_DB_PATH");
    sqlite3 *cfgdb = test_db_open("cclaw.db");
    if (!cfgdb) { chdir(cwd); system("rm -rf .cclaw_test_zc"); FAIL("db_open"); return; }
    Config *cfg = config_load(cfgdb);
    db_close(cfgdb);
    if (!cfg) { chdir(cwd); system("rm -rf .cclaw_test_zc"); FAIL("config_load"); return; }

    /* db_path comes from the open handle; workspace defaults next to it */
    const char *ws = cfg->workspace;
    size_t wl = strlen(ws);
    const char *want = "/agents/default/workspace";
    if (wl < strlen(want) || strcmp(ws + wl - strlen(want), want) != 0) {
        chdir(cwd); system("rm -rf .cclaw_test_zc"); config_free(cfg);
        FAIL("workspace default wrong"); return;
    }
    const char *dp = cfg->db_path;
    size_t dl = strlen(dp);
    if (dl < 8 || strcmp(dp + dl - 8, "cclaw.db") != 0) {
        chdir(cwd); system("rm -rf .cclaw_test_zc"); config_free(cfg);
        FAIL("db_path default wrong"); return;
    }

    workspace_init(cfg);
    struct stat st;
    if (stat(cfg->workspace, &st) != 0 || !S_ISDIR(st.st_mode)) {
        chdir(cwd); system("rm -rf .cclaw_test_zc"); config_free(cfg);
        FAIL("workspace dir not created"); return;
    }

    config_free(cfg);
    chdir(cwd);
    system("rm -rf .cclaw_test_zc");
    PASS();
}

int main(void) {
    TEST_INIT();
    alarm(10);
    printf("test_cli:\n");
    test_cli_zero_config_startup();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
