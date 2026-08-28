/* session_condense: the sanctioned session-edit op. What matters here is that
 * it produces the *same* branch shape overflow compaction does, and that every
 * refusal is a refusal (nothing written) rather than a partial edit. */
#include "agent_config.h"
#include "config_registry.h"
#include "db.h"
#include "tool_session_condense.h"
#include "tools.h"
#include "test_util.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_DB "/tmp/test_cclaw_session_condense.sqlite"
#define AGENT "condenser"

static sqlite3 *setup(void) {
    test_db_clean(TEST_DB);
    sqlite3 *db = test_db_open(TEST_DB);
    test_seed_agent(db, AGENT);
    return db;
}

static void teardown(sqlite3 *db) {
    db_close(db);
    test_db_clean(TEST_DB);
}

/* Four turns: a user entry after an assistant entry mints a new turn_id
 * (entries_turn_ai), so this is the smallest branch with real turn structure. */
static int64_t build_session(sqlite3 *db) {
    int64_t sid = session_create(db, "condense", AGENT, -1, 0);
    const char *texts[] = {"u1","a1","u2","a2","u3","a3","u4","a4"};
    for (int i = 0; i < 8; i++) {
        Message m = {.role = (i % 2 == 0) ? ROLE_USER : ROLE_ASSISTANT,
                     .content = (char *)texts[i]};
        assert(entry_append_with_iteration(db, sid, &m, i / 2 + 1) > 0);
    }
    return sid;
}

static char *call(sqlite3 *db, int64_t sid, const char *args, int *is_error) {
    ToolCondenseCtx ctx = {.db = db, .session_id = sid};
    snprintf(ctx.agent_name, sizeof(ctx.agent_name), "%s", AGENT);
    *is_error = 0;
    char *r = tool_session_condense_handler(args, &ctx, is_error);
    assert(r != NULL);
    return r;
}

/* The refusals must leave the branch byte-identical — a half-applied condense
 * is worse than none. */
static int branch_len(sqlite3 *db, int64_t sid) {
    int n = 0;
    Entry *b = session_get_branch(db, sid, &n);
    entry_branch_free(b, n);
    return n;
}

static void test_happy_path(void) {
    sqlite3 *db = setup();
    int64_t sid = build_session(db);

    int is_error = 0;
    char *r = call(db, sid, "{\"from_turn\":2,\"to_turn\":3,"
                            "\"summary\":\"turns 2-3: decided X\"}", &is_error);
    assert(is_error == 0);
    assert(strstr(r, "turns 2-3") != NULL);
    free(r);

    /* Branch is now u1, a1, <compaction>, u4, a4 — the same shape
     * entry_compact produces for overflow compaction. */
    int n = 0;
    Entry *b = session_get_branch(db, sid, &n);
    assert(n == 5);
    assert(strcmp(b[0].message.content, "u1") == 0);
    assert(strcmp(b[1].message.content, "a1") == 0);
    assert(b[2].message.role == ROLE_COMPACTION);
    assert(strcmp(b[2].message.content, "turns 2-3: decided X") == 0);
    assert(b[2].original_parent_id == -1);
    assert(strcmp(b[3].message.content, "u4") == 0);
    assert(b[3].original_parent_id > 0);   /* reparented onto the summary */
    assert(strcmp(b[4].message.content, "a4") == 0);

    /* Nothing from the condensed turns survives on the branch. */
    for (int i = 0; i < n; i++) {
        const char *c = b[i].message.content;
        assert(!c || (strcmp(c, "u2") && strcmp(c, "a2")
                      && strcmp(c, "u3") && strcmp(c, "a3")));
    }
    entry_branch_free(b, n);

    /* The transcript still holds them — condensing hides, never deletes. */
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM entries WHERE session_id=?"
                             " AND content='u2';", sid, 0) == 1);

    teardown(db);
    printf("  PASS test_happy_path\n");
}

static void test_refuses_live_turn(void) {
    sqlite3 *db = setup();
    int64_t sid = build_session(db);
    int before = branch_len(db, sid);

    int is_error = 0;
    char *r = call(db, sid, "{\"from_turn\":3,\"to_turn\":4,"
                            "\"summary\":\"s\"}", &is_error);
    assert(is_error == 1);
    assert(strstr(r, "live turn") != NULL);
    free(r);
    assert(branch_len(db, sid) == before);

    teardown(db);
    printf("  PASS test_refuses_live_turn\n");
}

static void test_refuses_bad_range(void) {
    sqlite3 *db = setup();
    int64_t sid = build_session(db);
    int before = branch_len(db, sid);
    int is_error = 0;

    /* Backwards */
    char *r = call(db, sid, "{\"from_turn\":3,\"to_turn\":2,\"summary\":\"s\"}",
                   &is_error);
    assert(is_error == 1);
    assert(strstr(r, "after to_turn") != NULL);
    free(r);

    /* Turn that does not exist */
    r = call(db, sid, "{\"from_turn\":2,\"to_turn\":99,\"summary\":\"s\"}",
             &is_error);
    assert(is_error == 1);
    assert(strstr(r, "not in this session") != NULL);
    free(r);

    /* Oldest turn: nothing to hang the summary off */
    r = call(db, sid, "{\"from_turn\":1,\"to_turn\":2,\"summary\":\"s\"}",
             &is_error);
    assert(is_error == 1);
    assert(strstr(r, "oldest turn") != NULL);
    free(r);

    /* Empty summary */
    r = call(db, sid, "{\"from_turn\":2,\"to_turn\":3,\"summary\":\"\"}",
             &is_error);
    assert(is_error == 1);
    assert(strstr(r, "summary") != NULL);
    free(r);

    assert(branch_len(db, sid) == before);
    teardown(db);
    printf("  PASS test_refuses_bad_range\n");
}

static void test_refuses_other_agents_session(void) {
    sqlite3 *db = setup();
    test_seed_agent(db, "someone_else");
    int64_t sid = session_create(db, "other", "someone_else", -1, 0);
    Message m = {.role = ROLE_USER, .content = "u1"};
    entry_append_with_iteration(db, sid, &m, 1);

    int is_error = 0;
    char *r = call(db, sid, "{\"from_turn\":1,\"to_turn\":1,\"summary\":\"s\"}",
                   &is_error);
    assert(is_error == 1);
    assert(strstr(r, "your own session") != NULL);
    free(r);

    teardown(db);
    printf("  PASS test_refuses_other_agents_session\n");
}

/* Registered (so it is requestable and grantable) but never baseline. */
static void test_not_in_default_baseline(void) {
    sqlite3 *db = setup();

    ToolRegistry reg;
    tools_init(&reg);
    ToolCondenseCtx ctx = {.db = db, .session_id = 1};
    assert(tool_session_condense_register(&reg, &ctx) == 0);
    assert(tools_lookup(&reg, "session_condense") != NULL);
    tools_free(&reg);

    char *defaults = config_get(db, "agent_default_tools");
    assert(defaults != NULL);
    assert(strstr(defaults, "session_condense") == NULL);
    free(defaults);

    agent_grant_defaults(db, AGENT);
    assert(grants_contains(db, AGENT, "tool", "js_eval") == 1);
    assert(grants_contains(db, AGENT, "tool", "session_condense") == 0);

    /* …and a grant is all it takes to have it. */
    assert(agent_config_grant(db, AGENT, "tool", "session_condense", 0) == 0);
    assert(grants_contains(db, AGENT, "tool", "session_condense") == 1);

    teardown(db);
    printf("  PASS test_not_in_default_baseline\n");
}

int main(void) {
    TEST_INIT();
    printf("TEST session_condense\n");
    test_happy_path();
    test_refuses_live_turn();
    test_refuses_bad_range();
    test_refuses_other_agents_session();
    test_not_in_default_baseline();
    printf("PASS session_condense\n");
    return 0;
}
