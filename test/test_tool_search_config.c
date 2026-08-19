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
    return e->handler(args, e->user_data, &(int){0});
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

/* The 2026-08-10 incident pin: "what model am I / what can I switch to" must
 * be answerable from search_config alone — own settings, providers, and
 * registered models (active one marked). Expired grants must not show. */
static void test_settings_models_providers(void) {
    sqlite3 *db = setup_db();
    assert(sqlite3_exec(db,
        "INSERT INTO providers(name, base_url, endpoint_type)"
        " VALUES('gw','http://127.0.0.1:8000/v1','openai');"
        "INSERT INTO models(id, provider_name, model, context_window)"
        " VALUES('claude-opus-4.5@gw','gw','claude-opus-4.5',200000);"
        "INSERT INTO models(id, provider_name, model)"
        " VALUES('claude-sonnet-4.6@gw','gw','claude-sonnet-4.6');"
        /* expired grant must be invisible in the per-kind listing */
        "INSERT INTO grants(agent_name,kind,value,expires_at)"
        " VALUES('default','tool','db_query', unixepoch()-10);"
        "INSERT INTO agent_models(agent_name,model_id,pos)"
        " VALUES('default','claude-opus-4.5@gw',0);",
        NULL, NULL, NULL) == SQLITE_OK);
    int64_t sid = session_create(db, "plain", "default", -1, 0);
    assert(sid > 0);

    ToolRegistry reg;
    tools_init(&reg);
    SearchConfigCtx ctx = {.db = db, .agent_name = "default", .session_id = sid};
    assert(tool_search_config_register(&reg, &ctx) == 0);

    char *out = call_handler(&reg, "{}");
    assert(out != NULL);
    assert(strstr(out, "## Your settings"));
    assert(strstr(out, "models: claude-opus-4.5@gw"));
    assert(strstr(out, "## Providers"));
    assert(strstr(out, "gw [openai] http://127.0.0.1:8000/v1"));
    assert(strstr(out, "## Registered models"));
    assert(strstr(out, "claude-opus-4.5@gw [healthy] ctx:200000 <- your list #1"));
    assert(strstr(out, "claude-sonnet-4.6@gw [healthy]"));
    /* per-kind grants line must not list the expired db_query grant */
    const char *tl = strstr(out, "tools: ");
    assert(tl != NULL);
    const char *tl_end = strchr(tl, '\n');
    assert(tl_end != NULL);
    char line[512];
    snprintf(line, sizeof(line), "%.*s", (int)(tl_end - tl), tl);
    assert(strstr(line, "db_query") == NULL);
    free(out);

    /* query filter narrows the models section */
    out = call_handler(&reg, "{\"query\":\"sonnet\"}");
    assert(out != NULL);
    assert(strstr(out, "claude-sonnet-4.6@gw"));
    assert(strstr(out, "claude-opus-4.5@gw [healthy]") == NULL);
    free(out);

    tools_free(&reg);
    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_settings_models_providers\n");
}

int main(void) {
    TEST_INIT();
    printf("test_tool_search_config:\n");
    test_no_filter_no_line();
    test_filter_line_present();
    test_empty_filter_is_explicit();
    test_settings_models_providers();
    printf("All search_config tests passed.\n");
    return 0;
}
