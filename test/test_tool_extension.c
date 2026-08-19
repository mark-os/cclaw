/* test_tool_extension.c — trust-relevant DB writes of the extension
 * lifecycle tools (r4 F19): promote parks an apply-approval (never registers
 * directly), publish is owner-only, attach honors the published-or-owned
 * visibility boundary. Handlers are exercised through the registry, exactly
 * as the dispatcher calls them. */
#define _POSIX_C_SOURCE 200809L
#include "tool_extension.h"
#include "approval.h"
#include "db.h"
#include "extension_manifest.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_DB "/tmp/cclaw_test_tool_extension.db"
#define WS "/tmp/cclaw_test_tool_extension_ws"
/* A sub-agent authors in its OWN workspace, never the parent's. */
#define SUBWS "/tmp/cclaw_test_tool_extension_subws"
/* <dirname(TEST_DB)>/extensions/<name> — where a promoted bundle would land. */
#define SUBSTORE "/tmp/extensions/subx"

static sqlite3 *g_db;
static ToolRegistry g_reg;
static ToolExtensionCtx g_ctx;

static char *call(const char *tool, const char *args) {
    ToolEntry *e = tools_lookup(&g_reg, tool);
    assert(e && e->handler);
    return e->handler(args, e->user_data, &(int){0});
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
        "SELECT tool_name, resolve, args_json FROM approvals WHERE state='pending';",
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

/* Materialize `agent`'s extension tools exactly as the dispatcher does — the
 * tools⨝agent_extensions⨝extensions join in tools.c is the only thing that
 * decides what reaches the LLM's tools array — and report whether `tool` is
 * in that callable set. Fresh registry per probe: the loader skips names
 * already materialized, so a shared one would leak visibility between
 * agents. */
static int callable_by(const char *agent, const char *tool) {
    ToolRegistry r;
    tools_init(&r);
    tools_load_extension_tools(&r, g_db, agent, NULL);
    int found = tools_lookup(&r, tool) != NULL;
    tools_free(&r);
    return found;
}

static void test_subagent_draft_never_globally_callable(void) {
    /* specs/extensions.md, "Lifecycle": "Promotion is the trust boundary
     * (per security.md): a sub-agent's draft must not auto-promote into a
     * globally callable tool." A sub-agent is a session with a parent
     * (sessions.parent_session_id/depth) running under its own agent
     * identity; nothing in the promote path inspects either, because the
     * gate is uniform — the handler can only park an approval, and only the
     * approved apply (main.c apply_grant -> extension_install) registers. */
    test_seed_agent(g_db, "worker");
    int64_t root_sid = g_ctx.session_id;
    const char *root_agent = g_ctx.agent_name;
    const char *root_ws = g_ctx.workspace;

    int64_t sub_sid = session_create(g_db, "worker-sub", "worker", root_sid, 1);
    assert(sub_sid > 0);

    /* The draft: a valid bundle, authored entirely inside the sub-agent's
     * own writable workspace — nothing about it is malformed, so only the
     * trust boundary can be what stops it. */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s && mkdir -p %s/extensions/subx",
             SUBWS, SUBSTORE, SUBWS);
    assert(system(cmd) == 0);
    write_file(SUBWS "/extensions/subx/extension.json",
        "{\"name\":\"subx\",\"version\":\"1.0.0\","
        "\"tools\":[{\"name\":\"sub_backdoor\",\"description\":\"d\","
        "\"parameters\":{\"type\":\"object\",\"properties\":"
        "{\"cmd\":{\"type\":\"string\"}}},"
        "\"hosts\":[\"api.sub.test\"],\"handler\":\"sub.qjs\"}]}");
    write_file(SUBWS "/extensions/subx/sub.qjs", "'ok'");

    /* Dispatch as the sub-agent: its own identity, its own workspace, its
     * own session (state pre-set to tool_running, the park's CAS source). */
    g_ctx.agent_name = "worker";
    g_ctx.workspace = SUBWS;
    g_ctx.session_id = sub_sid;
    assert(session_set_state(g_db, sub_sid, "tool_running") == 0);

    char *r = call("extension_promote", "{\"name\":\"subx\"}");
    assert(r == NULL);   /* parked, not applied */

    /* THE INVARIANT: the draft is registered nowhere and callable by nobody. */
    assert(scalar("SELECT COUNT(*) FROM extensions WHERE name='subx'") == 0);
    assert(scalar("SELECT COUNT(*) FROM tools WHERE name='sub_backdoor'") == 0);
    assert(scalar("SELECT COUNT(*) FROM agent_extensions"
                  " WHERE extension_name='subx'") == 0);
    assert(!callable_by("worker", "sub_backdoor"));   /* not by its author */
    assert(!callable_by("default", "sub_backdoor"));  /* not by the parent */
    /* Nor is the code in the shared store — promote copies only on apply. */
    struct stat stbuf;
    assert(stat(SUBSTORE, &stbuf) != 0);

    /* The request survives as a parked approval attributed to the asking
     * sub-agent session — that attribution is what lets an approver see who
     * is asking for the capability. */
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(g_db,
        "SELECT session_id, tool_name FROM approvals"
        " WHERE state='pending' AND args_json LIKE '%subx%';",
        -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_int64(st, 0) == sub_sid);
    const char *action = (const char *)sqlite3_column_text(st, 1);
    assert(action && strcmp(action, "extension_promote") == 0);
    sqlite3_finalize(st);

    /* Positive control: the legitimate path works. extension_install is what
     * apply_grant() runs once a human resolves that approval — the draft was
     * valid all along, so it is *registration*, not authoring, that makes a
     * tool callable. */
    char *err = NULL;
    assert(extension_install(g_db, SUBWS "/extensions/subx", "worker", &err) == 0);
    assert(err == NULL);
    assert(scalar("SELECT COUNT(*) FROM tools WHERE name='sub_backdoor'") == 1);
    assert(callable_by("worker", "sub_backdoor"));   /* owner sees its own */

    /* ...and even then it is not GLOBAL: an installed extension is
     * published=0, so no other agent can call it without publish+attach. */
    assert(scalar("SELECT published FROM extensions WHERE name='subx'") == 0);
    assert(!callable_by("default", "sub_backdoor"));

    g_ctx.session_id = root_sid;
    g_ctx.agent_name = root_agent;
    g_ctx.workspace = root_ws;
    printf("  PASS test_subagent_draft_never_globally_callable\n");
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
    test_subagent_draft_never_globally_callable();

    tools_free(&g_reg);
    db_close(g_db);
    test_db_clean(TEST_DB);
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s %s", WS, SUBWS, SUBSTORE);
    assert(system(cmd) == 0);
    printf("All tool_extension tests passed.\n");
    return 0;
}
