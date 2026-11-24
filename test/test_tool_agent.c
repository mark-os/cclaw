#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "tool_agent.h"
#include "config_registry.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

static sqlite3 *setup_db(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db != NULL);
    /* launch_agent refuses names without an agents row */
    sqlite3_exec(db, "INSERT INTO agents(name) VALUES('default')", NULL, NULL, NULL);
    return db;
}

static void test_depth_limit(void) {
    sqlite3 *db = setup_db();
    /* Create a session at max depth */
    int64_t sid = session_create(db, "deep", NULL, 1, config_default_int("agent_max_depth"));
    assert(sid > 0);
    AgentLaunchCtx ctx = {.db = db, .session_id = sid};
    char *r = tool_launch_agent_handler("{\"task\":\"hello\"}", &ctx, &(int){0});
    assert(r != NULL);
    assert(strstr(r, "max agent depth") != NULL);
    free(r);
    db_close(db);
}

/* A single batch of launches counts against itself: freshly launched children
 * are idle with their spawn task queued (nothing advances them while the
 * dispatch loop is still running the batch), and the existence cap must see
 * them anyway — per-call refusal in call order, admit up to the cap, refuse
 * the rest. Before the queued arm in SESSION_IN_FLIGHT, a one-batch burst of
 * any size sailed past the cap. */
static void test_system_wide_limit_batch_aware(void) {
    sqlite3 *db = setup_db();
    int64_t parent_sid = session_create(db, "parent", "default", -1, 0);
    int max_active = config_default_int("session_max_active");

    AgentLaunchCtx ctx = {.db = db, .session_id = parent_sid};
    for (int i = 0; i < max_active + 3; i++) {
        char *r = tool_launch_agent_handler(
            "{\"task\":\"burst\",\"background\":true}", &ctx, &(int){0});
        assert(r != NULL);
        if (i < max_active)
            assert(strncmp(r, "error:", 6) != 0);   /* admitted */
        else {
            assert(strstr(r, "running or queued system-wide") != NULL);
            assert(strstr(r, "session_max_active") != NULL);
        }
        free(r);
    }
    /* Exactly the cap was admitted; the overflow created nothing. */
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM sessions"
                             " WHERE parent_session_id=?", parent_sid, -1)
           == max_active);

    db_close(db);
}

static void test_system_wide_limit(void) {
    sqlite3 *db = setup_db();

    /* Fill the system-wide existence cap across different parents */
    int max_active = config_default_int("session_max_active");
    for (int i = 0; i < max_active; i++) {
        int64_t psid = session_create(db, "p", NULL, -1, 0);
        int64_t csid = session_create(db, "c", NULL, psid, 1);
        session_set_state(db, csid, "llm_running");
    }

    int64_t my_sid = session_create(db, "me", "default", -1, 0);
    AgentLaunchCtx ctx = {.db = db, .session_id = my_sid};
    char *r = tool_launch_agent_handler("{\"task\":\"overflow\"}", &ctx, &(int){0});
    assert(r != NULL);
    assert(strstr(r, "running or queued system-wide") != NULL);
    assert(strstr(r, "session_max_active") != NULL);   /* names its knob */
    free(r);
    db_close(db);
}

static void test_spawn_blocking(void) {
    sqlite3 *db = setup_db();
    int64_t parent_sid = session_create(db, "parent", "default", -1, 0);
    assert(parent_sid > 0);

    /* Blocking launch returns NULL (no inline result — the child writes the
     * tool result on completion) and records the parent's tool_call_id on the
     * child so completion can route back. The handler does NOT change parent
     * state: the dispatcher marks the tool_call 'running' and the parent stays
     * in tool_running, so sibling launch_agent calls keep dispatching. */
    AgentLaunchCtx ctx = {.db = db, .session_id = parent_sid,
                          .current_tool_call_id = "call_123"};
    session_set_state(db, parent_sid, "llm_running");
    session_set_state(db, parent_sid, "tool_running");
    char *r = tool_launch_agent_handler("{\"task\":\"blocking task\"}", &ctx, &(int){0});
    assert(r == NULL); /* NULL = async, real result comes on child completion */

    /* Parent state is unchanged by the handler (still tool_running) */
    char *state = db_scalar_text(db, "SELECT state FROM sessions WHERE id=?1", parent_sid);
    assert(state && strcmp(state, "tool_running") == 0);
    free(state);

    /* The child carries the parent's tool_call_id for result routing */
    char *call = db_scalar_text(db, "SELECT parent_tool_call_id FROM sessions"
                                    " WHERE parent_session_id=?1 ORDER BY id DESC LIMIT 1",
                                parent_sid);
    assert(call && strcmp(call, "call_123") == 0);
    free(call);

    db_close(db);
}

static void test_unknown_agent_rejected(void) {
    sqlite3 *db = setup_db();
    int64_t parent_sid = session_create(db, "parent", "default", -1, 0);
    AgentLaunchCtx ctx = {.db = db, .session_id = parent_sid};
    /* Trust policy comes from the agents row; an unregistered name must be
     * refused, not spawned (it would run with no row to derive policy from) */
    char *r = tool_launch_agent_handler(
        "{\"task\":\"x\",\"name\":\"x-nonexistent\"}", &ctx, &(int){0});
    assert(r != NULL);
    assert(strstr(r, "unknown agent") != NULL);
    free(r);
    db_close(db);
}

static void test_check_agent_idle_with_result(void) {
    sqlite3 *db = setup_db();
    int64_t parent_sid = session_create(db, "parent", "default", -1, 0);
    int64_t child_sid = session_create(db, "child", NULL, parent_sid, 1);

    /* Add an assistant entry to child session */
    Message msg = {.role = ROLE_ASSISTANT, .content = "the answer is 42"};
    entry_append_with_iteration(db, child_sid, &msg, 1);

    char args[64];
    snprintf(args, sizeof(args), "{\"session_id\":%lld}", (long long)child_sid);

    AgentLaunchCtx ctx = {.db = db, .session_id = parent_sid};
    char *r = tool_check_session_handler(args, &ctx, &(int){0});
    assert(strstr(r, "state: idle") != NULL);
    assert(strstr(r, "result: the answer is 42") != NULL);
    free(r);
    db_close(db);
}

/* Collecting a terminal result by poll is delivery: it advances the standing
 * parent edge's cursor to the child's leaf, so neither a boundary evaluation
 * nor the convergence sweep re-pushes what the parent already has. */
static void test_check_session_stamps_poll_delivery(void) {
    sqlite3 *db = setup_db();
    int64_t parent_sid = session_create(db, "parent", "default", -1, 0);
    int64_t child_sid = session_create(db, "child", NULL, parent_sid, 1);

    Message msg = {.role = ROLE_ASSISTANT, .content = "done"};
    int64_t eid = entry_append_with_iteration(db, child_sid, &msg, 1);
    assert(eid > 0);
    assert(db_scalar_i64(db, "SELECT cursor FROM delivery_edges"
                             " WHERE session_id=? AND target_kind='parent'"
                             " AND one_shot=0;", child_sid, -1) == 0);

    char args[64];
    snprintf(args, sizeof(args), "{\"session_id\":%lld}", (long long)child_sid);
    AgentLaunchCtx ctx = {.db = db, .session_id = parent_sid};
    char *r = tool_check_session_handler(args, &ctx, &(int){0});
    assert(strstr(r, "state: idle") != NULL);
    free(r);

    assert(db_scalar_i64(db, "SELECT cursor FROM delivery_edges"
                             " WHERE session_id=? AND target_kind='parent'"
                             " AND one_shot=0;", child_sid, -1) == eid);

    db_close(db);
}

/* --- Filter resolution tests for launch_agent --- */

static char *child_tool_filter(sqlite3 *db, int64_t parent_sid) {
    return db_scalar_text(db, "SELECT tool_filter FROM sessions WHERE parent_session_id=?1"
                              " ORDER BY id DESC LIMIT 1;", parent_sid);
}

static int child_count(sqlite3 *db, int64_t parent_sid) {
    return (int)db_scalar_i64(db, "SELECT COUNT(*) FROM sessions WHERE parent_session_id=?1;",
                              parent_sid, -1);
}

#define LAUNCHER_FILTER "[\"file_read\",\"launch_agent\"]"
#define WORKER_DEFAULT  "<worker_tools default>"

/* Which filter a spawned child gets. A self-spawn takes explicit `tools`,
 * else worker_tools (registry default unless set), and inherits the parent's
 * agent name. The launcher's own scope (its session filter, or the filter of
 * the chat pinning it, joined at read time) caps every spawn, named agents
 * included; an unfiltered launcher leaves a named agent unrestricted. */
static void test_spawn_filter_resolution(void) {
    static const struct {
        const char *name;
        const char *worker_tools, *parent_filter, *chat_filter;
        const char *args;
        const char *want_filter;   /* NULL: unrestricted */
        const char *want_agent;
    } cases[] = {
        { "explicit tools", NULL, NULL, NULL,
          "{\"task\":\"w\",\"background\":true,\"tools\":[\"file_read\"]}",
          "[\"file_read\"]", "default" },
        { "worker_tools set", "[\"js_eval\"]", NULL, NULL,
          "{\"task\":\"w\",\"background\":true}", "[\"js_eval\"]", "default" },
        /* a worker is never unrestricted */
        { "registry default", NULL, NULL, NULL,
          "{\"task\":\"w\",\"background\":true}", WORKER_DEFAULT, "default" },
        { "parent caps explicit tools", NULL, LAUNCHER_FILTER, NULL,
          "{\"task\":\"w\",\"background\":true,\"tools\":[\"web_fetch\",\"file_read\"]}",
          "[\"file_read\"]", "default" },
        /* the default list (which has web_fetch) is cut back, in its own order */
        { "parent caps worker_tools", NULL, LAUNCHER_FILTER, NULL,
          "{\"task\":\"w\",\"background\":true}", LAUNCHER_FILTER, "default" },
        { "parent caps named agent", NULL, LAUNCHER_FILTER, NULL,
          "{\"task\":\"w\",\"name\":\"helper\",\"background\":true}", LAUNCHER_FILTER, "helper" },
        { "unfiltered parent, named agent", NULL, NULL, NULL,
          "{\"task\":\"w\",\"name\":\"helper\",\"background\":true}", NULL, "helper" },
        { "pinning chat caps spawn", NULL, NULL, LAUNCHER_FILTER,
          "{\"task\":\"w\",\"background\":true,\"tools\":[\"web_fetch\",\"file_read\"]}",
          "[\"file_read\"]", "default" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        printf("    %s\n", cases[i].name);
        sqlite3 *db = setup_db();
        sqlite3_exec(db, "INSERT INTO agents(name) VALUES('helper')", NULL, NULL, NULL);
        if (cases[i].worker_tools) config_set(db, "worker_tools", cases[i].worker_tools);
        int64_t p = session_create_filtered(db, "parent", "default", -1, 0,
                                            cases[i].parent_filter);
        assert(p > 0);
        if (cases[i].chat_filter) {
            assert(sqlite3_exec(db, "INSERT INTO channels(name) VALUES('tg');",
                                NULL, NULL, NULL) == SQLITE_OK);
            char sql[256];
            snprintf(sql, sizeof(sql),
                     "INSERT INTO channel_chats(channel_name, chat_id, session_id, tool_filter)"
                     " VALUES('tg','42',%lld,'%s');", (long long)p, cases[i].chat_filter);
            assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
        }

        AgentLaunchCtx ctx = {.db = db, .session_id = p};
        char *r = tool_launch_agent_handler(cases[i].args, &ctx, &(int){0});
        assert(r != NULL && strstr(r, "agent delegated") != NULL);
        free(r);
        assert(child_count(db, p) == 1);

        const char *want = cases[i].want_filter;
        if (want && strcmp(want, WORKER_DEFAULT) == 0) want = config_default("worker_tools");
        char *f = child_tool_filter(db, p);
        assert(want ? (f && strcmp(f, want) == 0) : f == NULL);
        free(f);
        char *agent = db_scalar_text(db, "SELECT agent_name FROM sessions"
                                         " WHERE parent_session_id=?1", p);
        assert(agent && strcmp(agent, cases[i].want_agent) == 0);
        free(agent);
        db_close(db);
    }
}

/* D15 (2026-07-31): a default worker may delegate again — launch_agent and
 * check_session are in the shipped worker_tools list. The recursion rail is
 * agent_max_depth + the per-parent/system caps, not a missing tool. The rest
 * of the list is pinned too: an accidental widening (memory mutators, config
 * or agent/extension tools, channel_send, the file navigation family) would
 * silently hand every worker more authority than the spec documents. */
static void test_worker_tools_default_is_the_documented_list(void) {
    const char *def = config_default("worker_tools");
    assert(def != NULL);
    assert(strcmp(def,
        "[\"file_read\",\"file_write\",\"shell_exec\",\"web_fetch\",\"js_eval\","
        "\"launch_agent\",\"check_session\",\"cancel\",\"search_config\","
        "\"secret_create\"]") == 0);

    /* Spelled out so a rename of the constant can't quietly drop nesting. */
    assert(strstr(def, "\"launch_agent\"") != NULL);
    assert(strstr(def, "\"check_session\"") != NULL);
    /* Deliberate omissions — see specs/security.md. */
    assert(strstr(def, "\"file_grep\"") == NULL);
    assert(strstr(def, "\"memory_note_add\"") == NULL);
    assert(strstr(def, "\"request_config\"") == NULL);
    assert(strstr(def, "\"channel_send\"") == NULL);

}

/* A worker spawned with the default filter can itself spawn: the tool is in
 * the filter, and depth 1 → 2 is inside agent_max_depth. */
static void test_default_worker_may_nest(void) {
    sqlite3 *db = setup_db();
    int64_t parent_sid = session_create(db, "parent", "default", -1, 0);
    assert(parent_sid > 0);

    AgentLaunchCtx ctx = {.db = db, .session_id = parent_sid};
    char *r = tool_launch_agent_handler("{\"task\":\"work\",\"background\":true}", &ctx, &(int){0});
    assert(r != NULL && strstr(r, "agent delegated") != NULL);
    free(r);

    /* The worker's own session carries the default filter, which now allows
     * launch_agent — the dispatch gate would have refused it before D15. */
    int64_t worker_sid = db_scalar_i64(db, "SELECT id FROM sessions WHERE parent_session_id=?1",
                                       parent_sid, -1);
    assert(worker_sid > 0);
    assert(session_tool_allowed(db, worker_sid, "launch_agent") == 1);
    assert(session_tool_allowed(db, worker_sid, "check_session") == 1);
    assert(session_tool_allowed(db, worker_sid, "memory_note_add") == 0);

    /* And the nested spawn actually succeeds (depth 1 < agent_max_depth 2). */
    AgentLaunchCtx wctx = {.db = db, .session_id = worker_sid};
    r = tool_launch_agent_handler("{\"task\":\"deeper\",\"background\":true}", &wctx, &(int){0});
    assert(r != NULL && strstr(r, "agent delegated") != NULL);
    free(r);

    db_close(db);
}

static void test_tools_with_name_rejected(void) {
    sqlite3 *db = setup_db();
    int64_t parent_sid = session_create(db, "parent", "default", -1, 0);
    assert(parent_sid > 0);

    int before = child_count(db, parent_sid);

    AgentLaunchCtx ctx = {.db = db, .session_id = parent_sid};
    /* Passing "tools" with an explicit "name" → error */
    char *r = tool_launch_agent_handler(
        "{\"task\":\"x\",\"name\":\"default\",\"tools\":[\"file_read\"],\"background\":true}", &ctx, &(int){0});
    assert(r != NULL);
    assert(strstr(r, "only valid for self-spawn") != NULL);
    free(r);

    /* No child session created */
    int after = child_count(db, parent_sid);
    assert(after == before);

    db_close(db);
}

/* --- The launcher's own scope caps every spawn (TODO: launch_agent did not
 * inherit the launcher's tool filter) --- */

static int64_t filtered_parent(sqlite3 *db, const char *filter) {
    int64_t sid = session_create_filtered(db, "parent", "default", -1, 0, filter);
    assert(sid > 0);
    return sid;
}

/* Nothing in common: refused with the cause named, no child created. */
static void test_filter_empty_intersection_refused(void) {
    sqlite3 *db = setup_db();
    int64_t p = filtered_parent(db, "[\"file_read\",\"launch_agent\"]");
    AgentLaunchCtx ctx = {.db = db, .session_id = p};
    int err = 0;
    char *r = tool_launch_agent_handler(
        "{\"task\":\"w\",\"background\":true,\"tools\":[\"web_fetch\"]}", &ctx, &err);
    assert(r != NULL && err);
    assert(strstr(r, "tool filter") != NULL);
    assert(strstr(r, "file_read") != NULL);
    free(r);
    assert(child_count(db, p) == 0);
    db_close(db);
}

/* Depth 2: the grandchild inherits the child's already-narrowed scope. */
static void test_filter_grandchild_inherits(void) {
    sqlite3 *db = setup_db();
    int64_t p = filtered_parent(db, "[\"file_read\",\"launch_agent\"]");
    AgentLaunchCtx ctx = {.db = db, .session_id = p};
    char *r = tool_launch_agent_handler("{\"task\":\"w\",\"background\":true}",
                                        &ctx, &(int){0});
    assert(r != NULL && strstr(r, "agent delegated") != NULL);
    free(r);
    int64_t c = db_scalar_i64(db, "SELECT id FROM sessions WHERE parent_session_id=?",
                              p, -1);
    assert(c > 0);
    AgentLaunchCtx cctx = {.db = db, .session_id = c};
    r = tool_launch_agent_handler(
        "{\"task\":\"x\",\"background\":true,\"tools\":[\"web_fetch\",\"launch_agent\"]}",
        &cctx, &(int){0});
    assert(r != NULL && strstr(r, "agent delegated") != NULL);
    free(r);
    char *f = child_tool_filter(db, c);
    assert(f != NULL && strcmp(f, "[\"launch_agent\"]") == 0);
    free(f);
    db_close(db);
}

int main(void) {
    TEST_INIT();
    printf("test_tool_agent:\n");
    RUN(test_depth_limit);
    RUN(test_system_wide_limit);
    RUN(test_system_wide_limit_batch_aware);
    RUN(test_spawn_blocking);
    RUN(test_unknown_agent_rejected);
    RUN(test_check_agent_idle_with_result);
    RUN(test_check_session_stamps_poll_delivery);
    RUN(test_spawn_filter_resolution);
    RUN(test_worker_tools_default_is_the_documented_list);
    RUN(test_default_worker_may_nest);
    RUN(test_tools_with_name_rejected);
    RUN(test_filter_empty_intersection_refused);
    RUN(test_filter_grandchild_inherits);
    return 0;
}
