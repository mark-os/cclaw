/* channel_send: routes are the allowlist (default-deny, exact chat_id only),
 * origin-chat dedup in auto mode, action=list discovery, outbox insert. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "tool_channel_send.h"
#include "channel_api.h"
#include "tools.h"
#include "db.h"
#include "test_util.h"

static const char *DB_PATH = "/tmp/test_tool_channel_send.db";

static int qcount(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    int c = -1;
    if (sqlite3_step(s) == SQLITE_ROW) c = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return c;
}

static char *call(ToolRegistry *reg, ToolChannelSendCtx *ctx,
                  const char *agent, int64_t sid, const char *args) {
    snprintf(ctx->agent_name, sizeof(ctx->agent_name), "%s", agent);
    ctx->session_id = sid;
    ToolEntry *te = tools_lookup(reg, "channel_send");
    assert(te);
    return te->handler(args, te->user_data, &(int){0});
}

int main(void) {
    TEST_INIT();
    printf("test_tool_channel_send:\n");
    test_db_clean(DB_PATH);

    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    test_seed_agent(db, "Alice");
    test_seed_agent(db, "Bob");
    test_seed_agent(db, "Carol");

    assert(sqlite3_exec(db,
        "INSERT INTO sessions(id, agent_name, channel_name, chat_id)"
        " VALUES(10,'Alice','telegram','42'),"
        "       (11,'Alice',NULL,NULL),"
        "       (12,'Bob','telegram','77');"
        /* chat_title: display name the runner observed for chat 42 */
        "UPDATE sessions SET chat_title='#general @ My Server' WHERE id=10;"
        /* open-door default_agent must grant NO send authority */
        "INSERT INTO channels(name, default_agent) VALUES('telegram','Alice');"
        "INSERT INTO channel_routes(channel_name, chat_id, session_id, delivery_mode)"
        " VALUES('telegram','42',10,'auto'),"
        "       ('telegram','77',12,'explicit'),"
        /* group chat pinned to Alice's session 11 */
        "       ('telegram','-500',11,'explicit');",
        NULL, NULL, NULL) == SQLITE_OK);

    ToolRegistry reg;
    tools_init(&reg);
    ToolChannelSendCtx ctx = {.db = db, .db_path = NULL};
    assert(tool_channel_send_register(&reg, &ctx) == 0);

    /* Unrouted target → default-deny. */
    printf("  deny_unrouted... ");
    char *r = call(&reg, &ctx, "Alice", 11,
        "{\"channel\":\"telegram\",\"chat_id\":\"999\",\"message\":\"hi\"}");
    assert(r && strstr(r, "error: no route"));
    free(r);
    printf("PASS\n");

    /* channels.default_agent grants no send authority. */
    printf("  deny_default_agent... ");
    r = call(&reg, &ctx, "Alice", 11,
        "{\"channel\":\"telegram\",\"chat_id\":\"888\",\"message\":\"hi\"}");
    assert(r && strstr(r, "error: no route"));
    free(r);
    printf("PASS\n");

    /* Another agent's route → denied. */
    printf("  deny_other_agent... ");
    r = call(&reg, &ctx, "Alice", 11,
        "{\"channel\":\"telegram\",\"chat_id\":\"77\",\"message\":\"hi\"}");
    assert(r && strstr(r, "error: no route"));
    free(r);
    printf("PASS\n");

    /* Own route, not origin session → queued + outbox row. */
    printf("  send_queued... ");
    r = call(&reg, &ctx, "Alice", 11,
        "{\"channel\":\"telegram\",\"chat_id\":\"42\",\"message\":\"hello\"}");
    assert(r && strstr(r, "queued, outbox id"));
    free(r);
    assert(qcount(db,
        "SELECT COUNT(*) FROM channel_outbox WHERE channel_name='telegram'"
        " AND json_extract(payload,'$.chat_id')='42'"
        " AND json_extract(payload,'$.text')='hello'") == 1);
    printf("PASS\n");

    /* Origin chat + auto route → dedup short-circuit, no row. */
    printf("  dedup_auto_origin... ");
    r = call(&reg, &ctx, "Alice", 10,
        "{\"channel\":\"telegram\",\"chat_id\":\"42\",\"message\":\"again\"}");
    assert(r && strstr(r, "already delivers"));
    free(r);
    assert(qcount(db,
        "SELECT COUNT(*) FROM channel_outbox"
        " WHERE json_extract(payload,'$.text')='again'") == 0);
    printf("PASS\n");

    /* Origin chat but explicit route → the tool IS the path; queued. */
    printf("  explicit_origin_sends... ");
    r = call(&reg, &ctx, "Bob", 12,
        "{\"channel\":\"telegram\",\"chat_id\":\"77\",\"message\":\"deliberate\"}");
    assert(r && strstr(r, "queued, outbox id"));
    free(r);
    printf("PASS\n");

    /* Route resolving through a pinned session owned by the agent. */
    printf("  pinned_session_route... ");
    r = call(&reg, &ctx, "Alice", 11,
        "{\"channel\":\"telegram\",\"chat_id\":\"-500\",\"message\":\"group msg\"}");
    assert(r && strstr(r, "queued, outbox id"));
    free(r);
    printf("PASS\n");

    /* action=list: Alice sees 42 and -500 (her pinned sessions), not 77. */
    printf("  list_targets... ");
    r = call(&reg, &ctx, "Alice", 11, "{\"action\":\"list\"}");
    assert(r && strstr(r, "\"42\"") && strstr(r, "\"-500\""));
    assert(!strstr(r, "\"77\"") && !strstr(r, "\"*\""));
    /* Chats carry their display name so the model can pick one by name —
     * unnamed chats still list (name: null), they just have nothing to show. */
    assert(strstr(r, "\"name\":\"#general @ My Server\""));
    assert(strstr(r, "\"name\":null"));
    free(r);
    printf("PASS\n");

    /* No target named → defaults to the session's own bound chat. */
    printf("  default_own_chat... ");
    r = call(&reg, &ctx, "Bob", 12, "{\"message\":\"speak here\"}");
    assert(r && strstr(r, "queued, outbox id"));
    free(r);
    assert(qcount(db,
        "SELECT COUNT(*) FROM channel_outbox WHERE channel_name='telegram'"
        " AND json_extract(payload,'$.chat_id')='77'"
        " AND json_extract(payload,'$.text')='speak here'") == 1);
    printf("PASS\n");

    /* Unbound session (no channel/chat) cannot default → error, no row. */
    printf("  default_unbound_errors... ");
    r = call(&reg, &ctx, "Alice", 11, "{\"message\":\"nowhere\"}");
    assert(r && strstr(r, "error:"));
    free(r);
    assert(qcount(db,
        "SELECT COUNT(*) FROM channel_outbox"
        " WHERE json_extract(payload,'$.text')='nowhere'") == 0);
    printf("PASS\n");

    /* Missing args → error. */
    printf("  missing_args... ");
    r = call(&reg, &ctx, "Alice", 11, "{\"channel\":\"telegram\"}");
    assert(r && strstr(r, "error:"));
    free(r);
    printf("PASS\n");

    /* A successful send is not a failed tool call. */
    printf("  success_is_not_an_error... ");
    int failed = 0;   /* the dispatcher presets it; a handler only ever sets it */
    ToolEntry *te = tools_lookup(&reg, "channel_send");
    r = te->handler("{\"channel\":\"telegram\",\"chat_id\":\"-500\",\"message\":\"ok\"}",
                    te->user_data, &failed);
    assert(r && strstr(r, "queued") && failed == 0);
    free(r);
    printf("PASS\n");

    /* ── file: a workspace file, copied into the channel's media spool ── */
    static const unsigned char png[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n',
                                         0x00, 0xFF, 0x00, 0x80 };
    const char *WS = "/tmp/test_tool_channel_send_ws";
    assert(system("rm -rf /tmp/test_tool_channel_send_ws &&"
                  " mkdir -p /tmp/test_tool_channel_send_ws/sub &&"
                  " ln -s /etc/hostname /tmp/test_tool_channel_send_ws/leak.txt &&"
                  " ln -s /etc /tmp/test_tool_channel_send_ws/etc &&"
                  " : > /tmp/test_tool_channel_send_ws/empty.bin") == 0);
    FILE *pf = fopen("/tmp/test_tool_channel_send_ws/sub/my pic.png", "wb");
    assert(pf && fwrite(png, 1, sizeof(png), pf) == sizeof(png) && fclose(pf) == 0);
    ctx.workspace = WS;
    ctx.db_path = DB_PATH;

    printf("  file_queued_as_spool_copy... ");
    r = call(&reg, &ctx, "Alice", 11,
        "{\"channel\":\"telegram\",\"chat_id\":\"42\",\"file\":\"sub/my pic.png\","
        "\"message\":\"look\"}");
    assert(r && strstr(r, "queued, outbox id") && strstr(r, "image/png"));
    free(r);
    /* caption, not text: a handler that predates files must fail the row */
    assert(qcount(db,
        "SELECT COUNT(*) FROM channel_outbox WHERE"
        " json_extract(payload,'$.caption')='look'"
        " AND json_extract(payload,'$.text') IS NULL"
        " AND json_extract(payload,'$.file.name')='my_pic.png'"
        " AND json_extract(payload,'$.file.mime')='image/png'"
        " AND json_extract(payload,'$.file.path') LIKE '/tmp/media/telegram/out-%'") == 1);
    char *spool = db_scalar_text(db,
        "SELECT json_extract(payload,'$.file.path') FROM channel_outbox"
        " WHERE json_extract(payload,'$.file') IS NOT NULL AND id > ?;", 0);
    assert(spool);
    unsigned char got[64];
    FILE *sf = fopen(spool, "rb");
    assert(sf);
    size_t gn = fread(got, 1, sizeof(got), sf);
    fclose(sf);
    assert(gn == sizeof(png) && memcmp(got, png, gn) == 0);
    printf("PASS\n");

    /* The agent's own auto-mode chat: text would double-post, a file would
     * not — the reply never carries one. */
    printf("  file_to_own_auto_chat... ");
    r = call(&reg, &ctx, "Alice", 10, "{\"file\":\"sub/my pic.png\"}");
    assert(r && strstr(r, "queued, outbox id"));
    free(r);
    printf("PASS\n");

    /* Outside the workspace, by any spelling: the check is on the file that
     * was opened, so a symlink (to a file, or as a directory on the way) and
     * a plain absolute path are all the same refusal. Nothing is queued. */
    printf("  file_outside_workspace_refused... ");
    int before = qcount(db, "SELECT COUNT(*) FROM channel_outbox");
    const char *outside[] = {
        "{\"chat_id\":\"42\",\"channel\":\"telegram\",\"file\":\"leak.txt\"}",
        "{\"chat_id\":\"42\",\"channel\":\"telegram\",\"file\":\"etc/hostname\"}",
        "{\"chat_id\":\"42\",\"channel\":\"telegram\",\"file\":\"/etc/hostname\"}",
        "{\"chat_id\":\"42\",\"channel\":\"telegram\",\"file\":\"../test_tool_channel_send.db\"}",
    };
    for (size_t i = 0; i < sizeof(outside) / sizeof(outside[0]); i++) {
        failed = 0;
        ctx.session_id = 11;
        r = te->handler(outside[i], te->user_data, &failed);
        assert(r && failed == 1 && strstr(r, "outside your workspace"));
        free(r);
    }
    r = call(&reg, &ctx, "Alice", 11,
        "{\"chat_id\":\"42\",\"channel\":\"telegram\",\"file\":\"empty.bin\"}");
    assert(r && strstr(r, "is empty"));
    free(r);
    r = call(&reg, &ctx, "Alice", 11,
        "{\"chat_id\":\"42\",\"channel\":\"telegram\",\"file\":\"sub\"}");
    assert(r && strstr(r, "not a regular file"));
    free(r);
    r = call(&reg, &ctx, "Alice", 11,
        "{\"chat_id\":\"42\",\"channel\":\"telegram\",\"file\":\"absent.png\"}");
    assert(r && strstr(r, "cannot open"));
    free(r);
    assert(qcount(db, "SELECT COUNT(*) FROM channel_outbox") == before);
    printf("PASS\n");

    printf("  file_over_limit_refused... ");
    assert(sqlite3_exec(db, "INSERT OR REPLACE INTO config(key,value)"
                            " VALUES('channel_send_file_max_bytes','4');",
                        NULL, NULL, NULL) == SQLITE_OK);
    r = call(&reg, &ctx, "Alice", 11,
        "{\"chat_id\":\"42\",\"channel\":\"telegram\",\"file\":\"sub/my pic.png\"}");
    assert(r && strstr(r, "the limit is 4"));
    free(r);
    printf("PASS\n");

    /* The spool copy lives exactly as long as its row can still be sent. */
    printf("  spool_copy_removed_when_row_settles... ");
    ChannelCtx *cc = channel_ctx_open(DB_PATH, "telegram");
    assert(cc && access(spool, F_OK) == 0);
    int64_t fid = db_scalar_i64(db,
        "SELECT MIN(id) FROM channel_outbox"
        " WHERE json_extract(payload,'$.file') IS NOT NULL AND id > ?;", 0, -1);
    int64_t fid2 = db_scalar_i64(db,
        "SELECT MAX(id) FROM channel_outbox"
        " WHERE json_extract(payload,'$.file') IS NOT NULL AND id > ?;", 0, -1);
    char *spool2 = db_scalar_text(db,
        "SELECT json_extract(payload,'$.file.path') FROM channel_outbox WHERE id=?;", fid2);
    assert(fid > 0 && fid2 > fid && spool2 && access(spool2, F_OK) == 0);
    assert(channel_ack_outbox(cc, fid) == 0 && access(spool, F_OK) != 0);
    assert(channel_fail_outbox(cc, fid2, "refused by platform") == 0 &&
           access(spool2, F_OK) != 0);
    channel_ctx_free(cc);
    free(spool); free(spool2);
    printf("PASS\n");
    assert(system("rm -rf /tmp/test_tool_channel_send_ws") == 0);

    tools_free(&reg);
    db_close(db);
    test_db_clean(DB_PATH);
    printf("all channel_send tests passed\n");
    return 0;
}
