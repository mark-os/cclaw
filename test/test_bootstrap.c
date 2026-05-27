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

    /* Verify inbox has a message */
    int count = inbox_count(db, sid);
    assert(count == 1);

    /* Verify session has agent_name starting with "ephemeral-" */
    char *agent_name = session_get_agent_name(db, sid);
    assert(agent_name != NULL);
    assert(strncmp(agent_name, "ephemeral-", 10) == 0);

    /* Verify agent.json has limited tools */
    char path[512];
    snprintf(path, sizeof(path), "agents/%s/agent.json", agent_name);
    FILE *f = fopen(path, "r");
    assert(f != NULL);
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    assert(strstr(buf, "configure_provider") != NULL);
    assert(strstr(buf, "configure_channel") != NULL);
    assert(strstr(buf, "create_agent") != NULL);
    /* Should NOT have shell_exec etc */
    assert(strstr(buf, "shell_exec") == NULL);

    /* Verify system.md exists */
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
