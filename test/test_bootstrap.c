#define _POSIX_C_SOURCE 200809L
#include "daemon.h"
#include "agent_config.h"
#include "db.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AGENTS_DIR "agents"
#define DB_PATH "/tmp/test_bootstrap.db"

#define FAIL(msg) do { fprintf(stderr, "FAIL: %s\n", msg); return 1; } while(0)

static void cleanup(void) {
    system("rm -rf /tmp/test_bootstrap_agents");
    unlink(DB_PATH);
    unlink(DB_PATH "-wal");
    unlink(DB_PATH "-shm");
}

/* T189: bootstrap creates ephemeral when no named agents exist */
static int test_bootstrap_creates_ephemeral(void) {
    cleanup();
    /* Use a temp agents dir to avoid polluting real agents/ */
    system("rm -rf /tmp/test_bootstrap_agents");
    mkdir("/tmp/test_bootstrap_agents", 0755);

    /* Temporarily chdir so agent_discover("agents",...) finds our test dir */
    char orig_cwd[1024];
    getcwd(orig_cwd, sizeof(orig_cwd));
    chdir("/tmp/test_bootstrap_agents/..");
    /* Create the "agents" dir relative to new cwd */
    mkdir("/tmp/test_bootstrap_agents/../agents", 0755);

    /* Actually, let's just test the logic directly with a custom approach.
     * The daemon_bootstrap function uses hardcoded "agents" dir, so we need
     * to be in a directory where "agents" exists and is empty. */
    chdir(orig_cwd);

    /* Create a temp working dir with empty agents/ */
    system("rm -rf /tmp/test_bootstrap_work");
    mkdir("/tmp/test_bootstrap_work", 0755);
    mkdir("/tmp/test_bootstrap_work/agents", 0755);
    chdir("/tmp/test_bootstrap_work");

    sqlite3 *db = db_open(DB_PATH);
    if (!db) { chdir(orig_cwd); FAIL("db_open"); }

    int64_t sid = daemon_bootstrap(db);
    if (sid <= 0) { db_close(db); chdir(orig_cwd); FAIL("daemon_bootstrap should create session"); }

    /* T200: Session is now in agent DB, not daemon DB.
     * Find the ephemeral agent that was created. */
    size_t agent_count = 0;
    char **agents = agent_discover("agents", &agent_count);
    assert(agents != NULL && agent_count > 0);
    char *agent_name = NULL;
    for (size_t i = 0; i < agent_count; i++) {
        if (strncmp(agents[i], "ephemeral-", 10) == 0) {
            agent_name = strdup(agents[i]);
            break;
        }
    }
    agent_discover_free(agents, agent_count);
    assert(agent_name != NULL);
    assert(strncmp(agent_name, "ephemeral-", 10) == 0);

    int count = daemon_inbox_count(agent_name, sid);
    assert(count == 1);

    /* T196: Verify config in daemon.db agent_config table (no agent.json) */
    AgentConfig *ac = agent_config_load_db(db, agent_name);
    assert(ac != NULL);
    assert(ac->tool_count == 3);
    int has_configure_provider = 0, has_configure_channel = 0, has_create_agent = 0;
    int has_shell_exec = 0;
    for (size_t i = 0; i < ac->tool_count; i++) {
        if (strcmp(ac->tools[i], "configure_provider") == 0) has_configure_provider = 1;
        if (strcmp(ac->tools[i], "configure_channel") == 0) has_configure_channel = 1;
        if (strcmp(ac->tools[i], "create_agent") == 0) has_create_agent = 1;
        if (strcmp(ac->tools[i], "shell_exec") == 0) has_shell_exec = 1;
    }
    assert(has_configure_provider);
    assert(has_configure_channel);
    assert(has_create_agent);
    assert(!has_shell_exec);
    agent_config_free(ac);

    /* Verify system.md exists */
    char path[512];
    snprintf(path, sizeof(path), "agents/%s/system.md", agent_name);
    struct stat st;
    assert(stat(path, &st) == 0 && st.st_size > 0);

    free(agent_name);
    db_close(db);
    chdir(orig_cwd);
    system("rm -rf /tmp/test_bootstrap_work");
    printf("  PASS: test_bootstrap_creates_ephemeral\n");
    return 0;
}

/* T189: bootstrap does nothing when named agents exist */
static int test_bootstrap_skips_when_agents_exist(void) {
    char orig_cwd[1024];
    getcwd(orig_cwd, sizeof(orig_cwd));

    system("rm -rf /tmp/test_bootstrap_work2");
    mkdir("/tmp/test_bootstrap_work2", 0755);
    mkdir("/tmp/test_bootstrap_work2/agents", 0755);
    mkdir("/tmp/test_bootstrap_work2/agents/mybot", 0755);
    chdir("/tmp/test_bootstrap_work2");

    /* Write a minimal agent.json so it's a valid agent dir */
    FILE *f = fopen("agents/mybot/agent.json", "w");
    fputs("{}", f);
    fclose(f);

    sqlite3 *db = db_open("/tmp/test_bootstrap2.db");
    if (!db) { chdir(orig_cwd); FAIL("db_open"); }

    int64_t sid = daemon_bootstrap(db);
    assert(sid == 0); /* no bootstrap needed */

    db_close(db);
    unlink("/tmp/test_bootstrap2.db");
    unlink("/tmp/test_bootstrap2.db-wal");
    unlink("/tmp/test_bootstrap2.db-shm");
    chdir(orig_cwd);
    system("rm -rf /tmp/test_bootstrap_work2");
    printf("  PASS: test_bootstrap_skips_when_agents_exist\n");
    return 0;
}

/* T189: bootstrap skips ephemeral-only dirs (they don't count as named) */
static int test_bootstrap_ignores_ephemeral_only(void) {
    char orig_cwd[1024];
    getcwd(orig_cwd, sizeof(orig_cwd));

    system("rm -rf /tmp/test_bootstrap_work3");
    mkdir("/tmp/test_bootstrap_work3", 0755);
    mkdir("/tmp/test_bootstrap_work3/agents", 0755);
    mkdir("/tmp/test_bootstrap_work3/agents/ephemeral-old-one", 0755);
    chdir("/tmp/test_bootstrap_work3");

    FILE *f = fopen("agents/ephemeral-old-one/agent.json", "w");
    fputs("{}", f);
    fclose(f);

    sqlite3 *db = db_open("/tmp/test_bootstrap3.db");
    if (!db) { chdir(orig_cwd); FAIL("db_open"); }

    int64_t sid = daemon_bootstrap(db);
    assert(sid > 0); /* should bootstrap since only ephemeral exists */

    db_close(db);
    unlink("/tmp/test_bootstrap3.db");
    unlink("/tmp/test_bootstrap3.db-wal");
    unlink("/tmp/test_bootstrap3.db-shm");
    chdir(orig_cwd);
    system("rm -rf /tmp/test_bootstrap_work3");
    printf("  PASS: test_bootstrap_ignores_ephemeral_only\n");
    return 0;
}

int main(void) {
    printf("test_bootstrap:\n");
    int rc = 0;
    rc |= test_bootstrap_creates_ephemeral();
    rc |= test_bootstrap_skips_when_agents_exist();
    rc |= test_bootstrap_ignores_ephemeral_only();
    return rc;
}
