/* test_tool_extension.c — trust-relevant DB writes of the extension
 * lifecycle tools (r4 F19): promote parks an apply-approval (never registers
 * directly), publish is owner-only, attach honors the published-or-owned
 * visibility boundary. Handlers are exercised through the registry, exactly
 * as the dispatcher calls them. */
#define _POSIX_C_SOURCE 200809L
#include "tool_extension.h"
#include "approval.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_DB "/tmp/cclaw_test_tool_extension.db"
#define WS "/tmp/cclaw_test_tool_extension_ws"

static sqlite3 *g_db;
static ToolRegistry g_reg;
static ToolExtensionCtx g_ctx;

static char *call(const char *tool, const char *args) {
    ToolEntry *e = tools_lookup(&g_reg, tool);
    assert(e && e->handler);
    return e->handler(args, e->user_data);
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    assert(f);
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

static int64_t scalar(const char *sql) {
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(g_db, sql, -1, &st, NULL) == SQLITE_OK);
    int64_t v = -1;
    if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

static void seed_extension(const char *name, const char *owner, int published) {
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(g_db,
        "INSERT INTO extensions(name, path, owner_agent, published)"
        " VALUES(?, '/tmp/x', ?, ?);", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, owner, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 3, published);
    assert(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
}

static void test_promote_rejects_bad_name(void) {
    char *r = call("extension_promote", "{\"name\":\"../evil\"}");
    assert(r && strstr(r, "error:"));
    free(r);
    r = call("extension_promote", "{}");
    assert(r && strstr(r, "error:"));
    free(r);
    /* Nothing parked, nothing registered. */
    assert(scalar("SELECT COUNT(*) FROM approvals") == 0);
    assert(scalar("SELECT COUNT(*) FROM extensions") == 0);
    printf("  PASS test_promote_rejects_bad_name\n");
}

static void test_promote_invalid_bundle_errors(void) {
    /* Draft dir exists but the manifest is invalid — eager validation must
     * refuse before anything parks. */
    mkdir(WS "/extensions/badx", 0755);
    write_file(WS "/extensions/badx/extension.json", "{ not json");
    char *r = call("extension_promote", "{\"name\":\"badx\"}");
    assert(r && strstr(r, "error: promote failed"));
    free(r);
    assert(scalar("SELECT COUNT(*) FROM approvals") == 0);
    printf("  PASS test_promote_invalid_bundle_errors\n");
}

static void test_promote_parks_apply_approval(void) {
    /* Valid draft: promote must NOT write extensions rows itself — it parks
     * an apply-approval carrying the bundle path, and returns NULL (park). */
    mkdir(WS "/extensions/goodx", 0755);
    write_file(WS "/extensions/goodx/extension.json",
        "{\"name\":\"goodx\",\"version\":\"1.0.0\","
        "\"tools\":[{\"name\":\"tx\",\"description\":\"d\","
        "\"parameters\":{\"type\":\"object\",\"properties\":"
        "{\"channel\":{\"type\":\"string\"}}},"
        "\"hosts\":[\"api.good.test\"],\"handler\":\"tx.qjs\"}]}");
    write_file(WS "/extensions/goodx/tx.qjs", "'ok'");

    /* The dispatcher has the session in tool_running before dispatching a
     * call; the promote park's awaiting_approval transition is CAS-guarded
     * to that source state. */
    assert(session_set_state(g_db, g_ctx.session_id, "tool_running") == 0);

    char *r = call("extension_promote", "{\"name\":\"goodx\"}");
    assert(r == NULL);   /* parked */

    /* The trust boundary: no registration happened... */
    assert(scalar("SELECT COUNT(*) FROM extensions") == 0);
    /* ...the approval carries the action + bundle for the approver... */
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(g_db,
        "SELECT action, resolve, args_json FROM approvals WHERE state='pending';",
        -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    const char *action = (const char *)sqlite3_column_text(st, 0);
    const char *resolve = (const char *)sqlite3_column_text(st, 1);
    const char *args = (const char *)sqlite3_column_text(st, 2);
    assert(action && strcmp(action, "extension_promote") == 0);
    assert(resolve && strcmp(resolve, "apply") == 0);
    assert(args && strstr(args, "goodx"));
    /* ...including the two things only a human can judge: where the tool
     * talks (a declared list REPLACES the agent's grants once promoted) and
     * its parameter surface (slack_post_message(channel,text) vs a
     * credential passthrough like slack_api(method,params)). */
    assert(strstr(args, "hosts=[api.good.test]"));
    assert(strstr(args, "params=") && strstr(args, "channel"));
    sqlite3_finalize(st);
    /* ...and the session is parked awaiting it. */
    assert(sqlite3_prepare_v2(g_db,
        "SELECT state FROM sessions WHERE id=?;", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_int64(st, 1, g_ctx.session_id);
    assert(sqlite3_step(st) == SQLITE_ROW);
    const char *sstate = (const char *)sqlite3_column_text(st, 0);
    assert(sstate && strcmp(sstate, "awaiting_approval") == 0);
    sqlite3_finalize(st);
    printf("  PASS test_promote_parks_apply_approval\n");
}

static void test_publish_owner_only(void) {
    seed_extension("mine", "default", 0);
    seed_extension("theirs", "other-agent", 0);

    char *r = call("extension_publish", "{\"name\":\"mine\"}");
    assert(r && strstr(r, "published extension 'mine'"));
    free(r);
    assert(scalar("SELECT published FROM extensions WHERE name='mine'") == 1);

    /* Not the owner: refused, flag untouched. */
    r = call("extension_publish", "{\"name\":\"theirs\"}");
    assert(r && strstr(r, "error:"));
    free(r);
    assert(scalar("SELECT published FROM extensions WHERE name='theirs'") == 0);
    printf("  PASS test_publish_owner_only\n");
}

static void test_attach_visibility_boundary(void) {
    /* Published (someone else's): attachable. */
    seed_extension("pub", "other-agent", 1);
    char *r = call("extension_attach", "{\"name\":\"pub\"}");
    assert(r && strstr(r, "attached extension 'pub'"));
    free(r);
    assert(scalar("SELECT enabled FROM agent_extensions"
                  " WHERE agent_name='default' AND extension_name='pub'") == 1);

    /* Unpublished and not owned: invisible — no row minted. */
    seed_extension("hidden", "other-agent", 0);
    r = call("extension_attach", "{\"name\":\"hidden\"}");
    assert(r && strstr(r, "error:"));
    free(r);
    assert(scalar("SELECT COUNT(*) FROM agent_extensions"
                  " WHERE extension_name='hidden'") == 0);

    /* Own unpublished draft: attachable (owner visibility). */
    r = call("extension_attach", "{\"name\":\"mine\"}");
    assert(r && strstr(r, "attached extension 'mine'"));
    free(r);
    printf("  PASS test_attach_visibility_boundary\n");
}

static void test_list_scopes_to_visible(void) {
    char *r = call("extension_list", "{}");
    assert(r);
    assert(strstr(r, "\"pub\""));       /* published */
    assert(strstr(r, "\"mine\""));      /* owned */
    assert(strstr(r, "\"hidden\"") == NULL);   /* neither */
    free(r);
    printf("  PASS test_list_scopes_to_visible\n");
}

int main(void) {
    TEST_INIT();
    alarm(15);
    printf("test_tool_extension:\n");
    test_db_clean(TEST_DB);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s/extensions", WS, WS);
    assert(system(cmd) == 0);

    g_db = test_db_open(TEST_DB);
    assert(g_db);
    /* Acting agent: attach writes agent_extensions.agent_name='default'. */
    test_seed_agent(g_db, "default");
    int64_t sid = session_create(g_db, "ext-test", NULL, -1, 0);
    assert(sid > 0);

    tools_init(&g_reg);
    g_ctx.db = g_db;
    g_ctx.agent_name = "default";
    g_ctx.workspace = WS;
    g_ctx.session_id = sid;
    g_ctx.current_tool_call_id = "tc_ext_1";
    assert(tool_extension_register(&g_reg, &g_ctx) == 0);

    test_promote_rejects_bad_name();
    test_promote_invalid_bundle_errors();
    test_promote_parks_apply_approval();
    test_publish_owner_only();
    test_attach_visibility_boundary();
    test_list_scopes_to_visible();

    tools_free(&g_reg);
    db_close(g_db);
    test_db_clean(TEST_DB);
    snprintf(cmd, sizeof(cmd), "rm -rf %s", WS);
    assert(system(cmd) == 0);
    printf("All tool_extension tests passed.\n");
    return 0;
}
