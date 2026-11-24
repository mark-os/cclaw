#ifndef TEST_RUN_SESSION_H
#define TEST_RUN_SESSION_H

#include "db.h"
#include "db_response.h"
#include "config.h"
#include "agent_setup.h"
#include "tools.h"
#include "llm_proc.h"
#include "mock_server.h"
#include "test_util.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define TEST_MAX_ITERATIONS 20

/* Run the agent loop synchronously (no threads, no fork).
 * Calls llm_req() in-process, dispatches tools inline.
 * Returns 0 on success, -1 on error. */
static inline int test_run_session(sqlite3 *db, int64_t session_id, AgentSetup *setup) {
    int iteration = 0;
    while (iteration < TEST_MAX_ITERATIONS) {
        int recall = (iteration == 0) ? 1 : 0;
        int rc = llm_req(db, NULL, session_id, recall);
        if (rc != 0) return -1;

        /* Check for pending tool calls */
        int tc_count = 0;
        PendingToolCall *tcs = db_tool_call_get_pending(db, session_id, &tc_count);
        if (!tcs || tc_count == 0) {
            db_tool_call_free_pending(tcs, tc_count);
            return 0; /* No tools — turn complete */
        }

        /* Dispatch pending tool calls inline */
        for (int i = 0; i < tc_count; i++) {
            PendingToolCall *tc = &tcs[i];
            ToolEntry *te = tools_lookup(&setup->reg, tc->name);
            char *result = NULL;
            int is_err = 0;
            if (te) {
                result = te->handler(tc->arguments, te->user_data, &is_err);
            } else {
                result = strdup("error: tool not found");
                is_err = 1;
            }
            if (!result) { result = strdup("error: null result"); is_err = 1; }

            ToolResult tr = {.tool_call_id = tc->call_id, .content = result};
            Message msg = {.role = ROLE_TOOL, .tool_result = &tr,
                           .tool_name = tc->name, .is_error = is_err};
            int64_t rid = entry_append_with_iteration(db, session_id, &msg, tc->iteration_id);
            db_tool_call_complete_with_result(db, tc->entry_id, tc->call_id, rid);
            free(result);
        }
        db_tool_call_free_pending(tcs, tc_count);
        iteration++;
    }
    return -1; /* max iterations */
}

/* Start the in-process mock server and point llm_req at it. */
static inline void test_mock_env(const char *db_path) {
    int port = mock_server_start();
    assert(port > 0);
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", port);
    setenv("CCLAW_DB_PATH", db_path, 1);
    setenv("CCLAW_PROVIDER_BASE_URL", url, 1);
    setenv("OPENROUTER_API_KEY", "test-key", 1);
    setenv("CCLAW_MODEL", "test-model", 1);
}

/* One in-process session against the mock: a fresh DB, agent "default",
 * a system entry and the user's message. The caller enqueues the mock's
 * answers, then runs. */
typedef struct {
    const char *path;
    sqlite3 *db;
    int64_t sid;
    Config *cfg;           /* loaded on the first run */
    AgentSetup setup;
} TestTurn;

static inline void test_turn_open(TestTurn *t, const char *db_path, const char *user) {
    memset(t, 0, sizeof(*t));
    t->path = db_path;
    test_db_clean(db_path);
    t->db = test_db_open(db_path);
    assert(t->db);
    test_mock_env(db_path);
    db_agent_upsert(t->db, "default", NULL, NULL);
    t->sid = session_create(t->db, "test", "default", -1, 0);
    Message sys = {.role = ROLE_SYSTEM, .content = "You are helpful."};
    entry_append_with_iteration(t->db, t->sid, &sys, 1);
    Message msg = {.role = ROLE_USER, .content = (char *)user};
    entry_append_with_iteration(t->db, t->sid, &msg, 1);
}

static inline int test_turn_run(TestTurn *t) {
    if (!t->cfg) {
        t->cfg = config_load(t->db);
        agent_setup_init(&t->setup, t->db, t->sid, t->cfg, "default");
    }
    return test_run_session(t->db, t->sid, &t->setup);
}

/* The last assistant message on the branch (caller frees), or NULL. */
static inline char *test_turn_reply(TestTurn *t) {
    int count = 0;
    Entry *branch = session_get_branch(t->db, t->sid, &count);
    char *reply = NULL;
    for (int i = 0; i < count; i++)
        if (branch[i].message.role == ROLE_ASSISTANT && branch[i].message.content) {
            free(reply);
            reply = test_strdup_(branch[i].message.content);
        }
    entry_branch_free(branch, count);
    return reply;
}

static inline void test_turn_close(TestTurn *t) {
    if (t->cfg) {
        agent_setup_destroy(&t->setup);
        config_free(t->cfg);
    }
    mock_server_stop();
    db_close(t->db);
    test_db_clean(t->path);
}

#endif
