/* session_condense: the agent's own session edit. It is a summarise of whole
 * turns through turn_edit.c — the same checks, record and writing a hook's
 * reply gets — so what matters here is the branch shape it leaves, the
 * record, and that every refusal is a refusal (nothing written) rather than a
 * partial edit. */
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

    /* Branch is now u1, a1, <compaction>, u4, a4: the summary spliced in,
     * the next turn re-pointed onto it. */
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

    /* Written and recorded as any edit is: the summary says who wrote it
     * and carries the first replaced turn's id; the record, on the leaf,
     * lists the four entries that left. */
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM entries WHERE session_id=?"
                             " AND type='compaction' AND turn_id=2"
                             " AND json_extract(data,'$.source')='tool:session_condense'",
                         sid, 0) == 1);
    assert(db_scalar_i64(db, "SELECT json_array_length(data,'$.turn_edits[0].left_branch')"
                             " FROM entries WHERE id=(SELECT leaf_id FROM sessions"
                             " WHERE id=?)", sid, 0) == 4);

    teardown(db);
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

    /* Empty summary */
    r = call(db, sid, "{\"from_turn\":2,\"to_turn\":3,\"summary\":\"\"}",
             &is_error);
    assert(is_error == 1);
    assert(strstr(r, "summary") != NULL);
    free(r);

    assert(branch_len(db, sid) == before);
    teardown(db);
}

/* The first turn has nothing before it; the summary becomes the root. */
static void test_from_first_turn(void) {
    sqlite3 *db = setup();
    int64_t sid = build_session(db);
    int is_error = 0;
    char *r = call(db, sid, "{\"from_turn\":1,\"to_turn\":2,\"summary\":\"s12\"}",
                   &is_error);
    assert(is_error == 0);
    free(r);
    int n = 0;
    Entry *b = session_get_branch(db, sid, &n);
    assert(n == 5);
    assert(b[0].message.role == ROLE_COMPACTION && strcmp(b[0].message.content, "s12") == 0);
    assert(strcmp(b[1].message.content, "u3") == 0 && b[1].original_parent_id > 0);
    entry_branch_free(b, n);
    assert(db_scalar_i64(db, "SELECT parent_id FROM entries WHERE session_id=?"
                             " AND type='compaction'", sid, 0) == -1);
    teardown(db);
}

/* A secret in the summary refuses it, as any text an edit writes. */
static void test_refuses_secret(void) {
    sqlite3 *db = setup();
    int64_t sid = build_session(db);
    int before = branch_len(db, sid);
    int is_error = 0;
    char *r = call(db, sid, "{\"from_turn\":2,\"to_turn\":3,"
                            "\"summary\":\"key AKIAIOSFODNN7EXAMPLE\"}", &is_error);
    assert(is_error == 1);
    assert(strstr(r, "secret") != NULL);
    free(r);
    assert(branch_len(db, sid) == before);
    teardown(db);
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

    agent_grant_defaults(db, AGENT, GRANT_ORIGIN("seed", 0));
    assert(grants_contains(db, AGENT, "tool", "js_eval") == 1);
    assert(grants_contains(db, AGENT, "tool", "session_condense") == 0);

    /* …and a grant is all it takes to have it. */
    assert(agent_config_grant(db, AGENT, "tool", "session_condense", 0, GRANT_ORIGIN("operator", 0)) == 0);
    assert(grants_contains(db, AGENT, "tool", "session_condense") == 1);

    teardown(db);
}

/* E2: the mechanical coda names every open commitment, and says nothing at
 * all when there is none. */
static void test_state_coda(void) {
    sqlite3 *db = setup();
    test_seed_agent(db, "default");
    int64_t sid = session_create(db, "coda", "default", -1, 0);

    /* Empty state → no coda, not an empty marker. */
    assert(condense_carried_state(db, sid) == NULL);

    /* A pending approval, an approved-but-unconsumed ticket, a decided one
     * (must NOT appear), a running child, an idle child (must not appear),
     * an armed one-shot and a recurring job (recurring must not appear). */
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO approvals (session_id, tool_name, state) VALUES"
        " (?1,'shell_exec','pending'), (?1,'web_fetch','approved'),"
        " (?1,'js_eval','consumed'), (?1,'file_write','rejected');",
        -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);

    int64_t kid = session_create(db, "child", "default", sid, 1);
    int64_t idle_kid = session_create(db, "idle_child", "default", sid, 1);
    assert(session_set_state(db, kid, "llm_running") == 0);
    (void)idle_kid;   /* stays 'idle' */

    assert(sqlite3_prepare_v2(db,
        "INSERT INTO cron_jobs (agent_name,name,cron_expr,run_at,session_id,task,"
        " next_run_at) VALUES"
        " ('default','ship-it','',unixepoch()+3600,?1,'post the release note',"
        "  unixepoch()+3600);",
        -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
    assert(sqlite3_exec(db,
        "INSERT INTO cron_jobs (agent_name,name,cron_expr,interval_s,session_id,task,"
        " next_run_at) VALUES ('default','hourly','0 * * * *',3600,1,'poll',"
        " unixepoch()+60);", NULL, NULL, NULL) == SQLITE_OK);

    char *coda = condense_carried_state(db, sid);
    assert(coda);
    assert(strstr(coda, CONDENSE_CODA_MARKER));
    assert(strstr(coda, "shell_exec") && strstr(coda, "waiting on a human"));
    assert(!strstr(coda, "web_fetch"));  /* approved = decided = absent */
    assert(!strstr(coda, "js_eval") && !strstr(coda, "file_write"));
    assert(strstr(coda, "llm_running"));
    assert(!strstr(coda, "idle"));
    assert(strstr(coda, "ship-it") && strstr(coda, "post the release note"));
    assert(!strstr(coda, "hourly"));
    free(coda);

    teardown(db);
}

int main(void) {
    TEST_INIT();
    printf("TEST session_condense\n");
    RUN(test_happy_path);
    RUN(test_refuses_live_turn);
    RUN(test_refuses_bad_range);
    RUN(test_from_first_turn);
    RUN(test_refuses_secret);
    RUN(test_refuses_other_agents_session);
    RUN(test_not_in_default_baseline);
    RUN(test_state_coda);
    printf("PASS session_condense\n");
    return 0;
}
