/* Unit test for the search_config grants block.
 *
 * The regression this pins (CharlesDow, 2026-07-31): a self-spawned worker
 * introspected its grants, saw `launch_agent` listed as granted, called it,
 * and was refused — the display showed agent-level authority but never the
 * session tool_filter that actually decides. Effective tools = grants ∩
 * filter, so both have to be on screen. */
#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "test_util.h"
#include "tools.h"
#include "tool_search_config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *DB_PATH = "/tmp/test_cclaw_search_config.db";

static char *call_handler(ToolRegistry *reg, const char *args) {
    ToolEntry *e = tools_lookup(reg, "search_config");
    assert(e != NULL);
    return e->handler(args, e->user_data);
}

/* Fresh db + one agent holding two tool grants. */
static sqlite3 *setup_db(void) {
    test_db_clean(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db != NULL);
    test_seed_agent(db, "default");
    assert(sqlite3_exec(db,
        "INSERT INTO grants(agent_name,kind,value) VALUES('default','tool','launch_agent');"
        "INSERT INTO grants(agent_name,kind,value) VALUES('default','tool','file_read');",
        NULL, NULL, NULL) == SQLITE_OK);
    return db;
}

/* An unfiltered session says nothing about filters — no noise for the common
 * case, and no way to read a missing line as an empty filter. */
static void test_no_filter_no_line(void) {
    sqlite3 *db = setup_db();
    int64_t sid = session_create(db, "plain", "default", -1, 0);
    assert(sid > 0);

    ToolRegistry reg;
    tools_init(&reg);
    SearchConfigCtx ctx = {.db = db, .agent_name = "default", .session_id = sid};
    assert(tool_search_config_register(&reg, &ctx) == 0);

    char *out = call_handler(&reg, "{}");
    assert(out != NULL);
    assert(strstr(out, "## Your current grants"));
    assert(strstr(out, "tools: ") != NULL);
    assert(strstr(out, "session tool filter:") == NULL);
    free(out);

    tools_free(&reg);
    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_no_filter_no_line\n");
}

/* A filtered session shows the filter next to the grants, so a worker can
 * see why a *granted* tool is still refused at dispatch. */
static void test_filter_line_present(void) {
    sqlite3 *db = setup_db();
    int64_t sid = session_create_filtered(db, "worker", "default", -1, 1,
                                          "[\"file_read\",\"search_config\"]");
    assert(sid > 0);

    ToolRegistry reg;
    tools_init(&reg);
    SearchConfigCtx ctx = {.db = db, .agent_name = "default", .session_id = sid};
    assert(tool_search_config_register(&reg, &ctx) == 0);

    char *out = call_handler(&reg, "{}");
    assert(out != NULL);
    assert(strstr(out, "session tool filter: file_read, search_config"));
    assert(strstr(out, "THIS session only"));
    /* The grants line still reports agent authority — the two are different
     * facts, and the filter line is what reconciles them. */
    assert(strstr(out, "launch_agent"));
    free(out);

    tools_free(&reg);
    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_filter_line_present\n");
}

/* An empty filter is a real state (nothing callable), not "unfiltered" —
 * it must not render as a blank list. */
static void test_empty_filter_is_explicit(void) {
    sqlite3 *db = setup_db();
    int64_t sid = session_create_filtered(db, "muted", "default", -1, 1, "[]");
    assert(sid > 0);

    ToolRegistry reg;
    tools_init(&reg);
    SearchConfigCtx ctx = {.db = db, .agent_name = "default", .session_id = sid};
    assert(tool_search_config_register(&reg, &ctx) == 0);

    char *out = call_handler(&reg, "{}");
    assert(out != NULL);
    assert(strstr(out, "session tool filter: (empty — no tools callable)"));
    free(out);

    tools_free(&reg);
    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_empty_filter_is_explicit\n");
}

int main(void) {
    TEST_INIT();
    printf("test_tool_search_config:\n");
    test_no_filter_no_line();
    test_filter_line_present();
    test_empty_filter_is_explicit();
    printf("All search_config tests passed.\n");
    return 0;
}
