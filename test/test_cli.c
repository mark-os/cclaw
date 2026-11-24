#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <stdlib.h>
#include "config.h"
#include "db.h"
#include "test_util.h"

static int ends_with(const char *s, const char *tail) {
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && strcmp(s + a - b, tail) == 0;
}

/* With no CCLAW_* env, config_load takes db_path from the open handle and
 * puts the default agent's workspace next to it; workspace_init creates it. */
static void test_cli_zero_config_startup(void) {
    system("rm -rf .cclaw_test_zc");
    char cwd[4096];
    assert(getcwd(cwd, sizeof(cwd)));
    assert(mkdir(".cclaw_test_zc", 0755) == 0);
    assert(chdir(".cclaw_test_zc") == 0);

    unsetenv("CCLAW_WORKSPACE");
    unsetenv("CCLAW_DB_PATH");
    sqlite3 *cfgdb = test_db_open("cclaw.db");
    assert(cfgdb);
    Config *cfg = config_load(cfgdb);
    db_close(cfgdb);
    assert(cfg);
    assert(ends_with(cfg->workspace, "/agents/default/workspace"));
    assert(ends_with(cfg->db_path, "cclaw.db"));

    workspace_init(cfg);
    struct stat st;
    assert(stat(cfg->workspace, &st) == 0 && S_ISDIR(st.st_mode));

    config_free(cfg);
    assert(chdir(cwd) == 0);
    system("rm -rf .cclaw_test_zc");
}

int main(void) {
    TEST_INIT();
    alarm(10);
    RUN(test_cli_zero_config_startup);
    printf("test_cli: all tests passed\n");
    return 0;
}
