#include "shutdown.h"
#include "agent.h"
#include "db.h"
#include "config.h"
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { printf("FAIL: %s\n", msg); } \
    else { tests_passed++; } \
} while(0)

static void test_initial_state(void) {
    shutdown_reset();
    ASSERT(shutdown_requested() == 0, "initially not requested");
}

static void test_request(void) {
    shutdown_reset();
    shutdown_request();
    ASSERT(shutdown_requested() != 0, "requested after shutdown_request()");
}

static void test_reset(void) {
    shutdown_request();
    shutdown_reset();
    ASSERT(shutdown_requested() == 0, "reset clears flag");
}

static void test_signal_handler(void) {
    shutdown_reset();
    shutdown_init();
    /* Deliver SIGINT to ourselves */
    raise(SIGINT);
    ASSERT(shutdown_requested() != 0, "SIGINT sets shutdown flag");
    shutdown_reset();
}

/* V31: agent writes aborted entry when shutdown is requested */
static void test_agent_aborted_on_shutdown(void) {
    const char *db_path = "/tmp/test_cclaw_shutdown_agent.sqlite";
    unlink(db_path);
    sqlite3 *db = db_open(db_path);
    ASSERT(db != NULL, "db open");
    if (!db) return;

    int64_t sid = session_create(db, "shutdown_test", NULL);
    ASSERT(sid > 0, "session created");

    /* Append a user message so agent has something to process */
    Message user_msg = {.role = ROLE_USER, .content = "hello"};
    entry_append(db, sid, &user_msg);

    /* Set shutdown flag BEFORE running agent */
    shutdown_request();

    Config cfg = {0};
    cfg.db_path = (char *)db_path;
    cfg.workspace = "/tmp";
    cfg.provider.base_url = "http://localhost:1/v1";
    cfg.provider.api_key = "test";
    cfg.provider.model = "test";
    cfg.provider.max_tokens = 100;
    cfg.provider.context_window = 4000;
    cfg.max_iterations = 5;

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;

    int rc = agent_run(&ctx);
    ASSERT(rc == -1, "agent_run returns -1 on shutdown");

    /* Check that an aborted entry was written */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    ASSERT(entries != NULL, "branch not null");
    ASSERT(count >= 2, "at least user + aborted entry");

    /* Last entry should be assistant with shutdown error content */
    int found_aborted = 0;
    for (int i = 0; i < count; i++) {
        if (entries[i].message.role == ROLE_ASSISTANT &&
            entries[i].message.content &&
            strstr(entries[i].message.content, "shutdown signal") != NULL) {
            found_aborted = 1;
            break;
        }
    }
    ASSERT(found_aborted, "aborted entry written to DB");

    entry_branch_free(entries, count);
    db_close(db);
    unlink(db_path);
    shutdown_reset();
}

int main(void) {
    test_initial_state();
    test_request();
    test_reset();
    test_signal_handler();
    test_agent_aborted_on_shutdown();

    printf("%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
