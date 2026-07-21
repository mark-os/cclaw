/* Live first-run test: fresh DB + seed.sql, exactly as a new user's first
 * `cclaw` invocation — schema, seed, config_load key-availability scan,
 * DB-driven model routing — then one real turn against whichever seeded
 * provider's key is in the environment (first run: OPENROUTER_API_KEY).
 *
 * Usage: ./build/test_e2e_first_run
 * Requires: OPENROUTER_API_KEY (or another seeded provider's key)
 */
#define _POSIX_C_SOURCE 200809L
#include "test_run_session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "test_util.h"

#define DB_PATH "/tmp/test_e2e_first_run.db"

int main(void) {
    TEST_INIT();
    alarm(60);
    printf("test_e2e_first_run:\n");

    /* First run means no operator overrides — clear anything the harness
     * environment might carry so the seed rows drive everything. */
    unsetenv("CCLAW_PROVIDER_BASE_URL");
    unsetenv("CCLAW_PROVIDER");
    unsetenv("CCLAW_MODEL");
    setenv("CCLAW_STREAM", "0", 1);
    setenv("CCLAW_DB_PATH", DB_PATH, 1);

    test_db_clean(DB_PATH);
    sqlite3 *db = test_db_open_seeded(DB_PATH);  /* schema + seed, like main() */
    assert(db);

    Config *cfg = config_load(db);
    assert(cfg);
    if (!cfg->provider.api_key) {
        printf("  SKIP: no seeded provider key in environment "
               "(set OPENROUTER_API_KEY)\n");
        config_free(cfg);
        db_close(db);
        test_db_clean(DB_PATH);
        return 0;
    }
    printf("  key scan selected: %s (%s)\n",
           cfg->provider.base_url, cfg->provider.model);

    db_agent_upsert(db, "default", NULL, NULL);
    int64_t sid = session_create(db, "first-run", "default", -1, 0);
    Message sys = {.role = ROLE_SYSTEM, .content = "You are helpful."};
    entry_append_with_turn(db, sid, &sys, 1);
    Message user = {.role = ROLE_USER,
                    .content = "Reply with exactly: first run ok"};
    entry_append_with_turn(db, sid, &user, 1);

    AgentSetup setup;
    agent_setup_init(&setup, db, sid, cfg, "default", AGENT_SETUP_CLI);
    int rc = test_run_session(db, sid, &setup);
    assert(rc == 0);

    /* A non-empty assistant reply arrived through the seeded route. */
    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    int found = 0;
    for (int i = 0; i < count; i++) {
        if (branch[i].message.role == ROLE_ASSISTANT &&
            branch[i].message.content && branch[i].message.content[0])
            found = 1;
    }
    assert(found);
    entry_branch_free(branch, count);

    agent_setup_destroy(&setup);
    config_free(cfg);
    db_close(db);
    test_db_clean(DB_PATH);
    printf("  PASS first-run turn via seeded provider\n");
    return 0;
}
