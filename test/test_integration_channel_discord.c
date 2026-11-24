/* Integration test: the real templates/channel_discord.qjs Discord Gateway
 * handler, driven against a loopback WS mock speaking the gateway protocol.
 * Unlike test_integration_channel_conn (which uses a stripped inline handler to
 * prove the transport), this loads the SHIPPED handler and exercises its
 * protocol + policy logic:
 *   HELLO -> handler IDENTIFY (op 2) + heartbeat (op 1) -> ACK (op 11)
 *   -> READY (learn bot id) -> three MESSAGE_CREATEs:
 *       guild, no mention   -> DROPPED by the require_mention gate
 *       DM                  -> emitted (DMs bypass the gate)
 *       guild, @mention     -> emitted
 * Proves: IDENTIFY, heartbeat liveness, mention gating, DM bypass, and that the
 * envelope reaches channel_events. No real network: everything is 127.0.0.1.
 * Skips cleanly where libcurl lacks ws/wss (libcurl-minimal). */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include "db.h"
#include "util.h"          /* curl_ws_available, util_read_file */
#include "test_util.h"
#include "test_ws.h"

#define DB_PATH  "/tmp/test_integ_discord.db"
#define EXT_DIR  "/tmp/test_integ_discord_ext"
#define JS_PATH  EXT_DIR "/channel.qjs"
#define BOT_ID   "999000"
#define FAIL(m)  do { fprintf(stderr, "FAIL: %s\n", m); goto fail; } while (0)

/* ── Mock gateway state ────────────────────────────────────────────── */
static int g_listen_fd = -1;
static volatile int g_identify_seen = 0;
static volatile int g_heartbeat_seen = 0;

static void *mock_gateway(void *arg) {
    (void)arg;
    int cfd = ws_accept_upgrade(g_listen_fd);
    if (cfd < 0) return NULL;

    /* Short heartbeat interval so the timer fires within the test window. */
    ws_send_text(cfd, "{\"op\":10,\"d\":{\"heartbeat_interval\":1000}}");

    int dispatched = 0;
    for (;;) {
        char pl[4096]; size_t len = 0;
        int op = ws_read_frame(cfd, pl, sizeof(pl), &len);
        if (op < 0) break;
        if (op == 0x8) break;               /* client close */
        if (op != 0x1) continue;            /* only text */
        if (strstr(pl, "\"op\":2")) g_identify_seen = 1;
        if (strstr(pl, "\"op\":1")) {
            g_heartbeat_seen = 1;
            ws_send_text(cfd, "{\"op\":11}");           /* heartbeat ACK */
        }
        /* Once IDENTIFY lands: READY (teaches the handler its bot id), then
         * three messages exercising the mention gate. */
        if (g_identify_seen && !dispatched) {
            dispatched = 1;
            ws_send_text(cfd,
                "{\"op\":0,\"s\":1,\"t\":\"READY\",\"d\":{\"session_id\":\"sess1\","
                "\"resume_gateway_url\":\"ws://127.0.0.1\",\"user\":{\"id\":\"" BOT_ID
                "\",\"username\":\"cclawbot\"},\"application\":{\"id\":\"app1\"}}}");
            /* Registered slash commands arrive as interactions (type 2).
             * /admin from the operator in a DM is handed on as the text
             * "/admin"; picked in a server it is asked from the operator's
             * own DM channel (opened here), never the room; /approvals from
             * a non-admin is refused privately. */
            ws_send_text(cfd,
                "{\"op\":0,\"s\":20,\"t\":\"INTERACTION_CREATE\",\"d\":{\"id\":\"int1\","
                "\"token\":\"tok1\",\"type\":2,\"channel_id\":\"dmcmd\","
                "\"data\":{\"name\":\"admin\"},"
                "\"user\":{\"id\":\"adminuser\",\"username\":\"mark\"}}}");
            ws_send_text(cfd,
                "{\"op\":0,\"s\":21,\"t\":\"INTERACTION_CREATE\",\"d\":{\"id\":\"int2\","
                "\"token\":\"tok2\",\"type\":2,\"channel_id\":\"chan1\",\"guild_id\":\"g1\","
                "\"data\":{\"name\":\"admin\"},"
                "\"member\":{\"user\":{\"id\":\"admin2\",\"username\":\"ops\"}}}}");
            ws_send_text(cfd,
                "{\"op\":0,\"s\":22,\"t\":\"INTERACTION_CREATE\",\"d\":{\"id\":\"int3\","
                "\"token\":\"tok3\",\"type\":2,\"channel_id\":\"dmcmd\","
                "\"data\":{\"name\":\"approvals\"},"
                "\"user\":{\"id\":\"u1\",\"username\":\"alice\"}}}");
            /* guild, no mention -> dropped by require_mention */
            ws_send_text(cfd,
                "{\"op\":0,\"s\":3,\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"m1\","
                "\"channel_id\":\"chan1\",\"guild_id\":\"g1\",\"content\":\"hello world\","
                "\"author\":{\"id\":\"u1\",\"username\":\"alice\"},\"mentions\":[]}}");
            /* DM -> emitted (bypasses the gate) */
            ws_send_text(cfd,
                "{\"op\":0,\"s\":4,\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"m2\","
                "\"channel_id\":\"dm1\",\"content\":\"hi in dm\","
                "\"author\":{\"id\":\"u1\",\"username\":\"alice\"},\"mentions\":[]}}");
            /* guild, @mention -> emitted */
            ws_send_text(cfd,
                "{\"op\":0,\"s\":5,\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"m3\","
                "\"channel_id\":\"chan1\",\"guild_id\":\"g1\",\"content\":\"<@" BOT_ID
                "> help me\",\"author\":{\"id\":\"u1\",\"username\":\"alice\"},"
                "\"mentions\":[{\"id\":\"" BOT_ID "\"}]}}");
            /* Ambient channel (chan2, debounce_ms=1500): a three-message
             * burst. #1 is the leading edge (quiet chat -> immediate); #2 and
             * #3 buffer and flush as ONE combined envelope. Without debounce
             * this same burst is three envelopes = three turns. */
            ws_send_text(cfd,
                "{\"op\":0,\"s\":6,\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"m4\","
                "\"channel_id\":\"chan2\",\"guild_id\":\"g1\",\"content\":\"first line\","
                "\"author\":{\"id\":\"u1\",\"username\":\"alice\"},\"mentions\":[]}}");
            ws_send_text(cfd,
                "{\"op\":0,\"s\":7,\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"m5\","
                "\"channel_id\":\"chan2\",\"guild_id\":\"g1\",\"content\":\"second line\","
                "\"author\":{\"id\":\"u2\",\"username\":\"bob\"},\"mentions\":[]}}");
            ws_send_text(cfd,
                "{\"op\":0,\"s\":8,\"t\":\"MESSAGE_CREATE\",\"d\":{\"id\":\"m6\","
                "\"channel_id\":\"chan2\",\"guild_id\":\"g1\",\"content\":\"third line\","
                "\"author\":{\"id\":\"u3\",\"username\":\"carol\"},\"mentions\":[]}}");
        }
    }
    close(cfd);
    return NULL;
}

/* ── Mock Discord REST (base_url) ──────────────────────────────────
 * Serves exactly the two calls an admin-notice DM needs:
 *   POST /api/v10/users/@me/channels   -> {"id":"dmchan9"}   (DM open)
 *   POST /api/v10/channels/dmchan9/messages -> {}            (the message)
 * Records what it saw so the test can prove the handler resolved the admin's
 * *user* id to a DM channel instead of POSTing to the user id as a channel. */
static int g_rest_fd = -1;
static volatile int g_rest_stop = 0;
static volatile int g_dm_open_seen = 0;
static volatile int g_cmds_registered = 0;   /* PUT applications/app1/commands */
static volatile int g_int_acks = 0;          /* bitmask: int1, int2, int3 answered privately */
static volatile int g_dm_msg_seen = 0;
static volatile int g_bad_channel_post = 0;   /* POST to the raw user id */
static volatile int g_file_upload_ok = 0;     /* multipart file post, bytes intact */

/* An attachment as channel_send spools it: PNG signature, NULs, high bytes. */
static const unsigned char FILE_BYTES[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n',
                                            0x00, 0xFF, 0x00, 0x80, 0xFE };
#define SPOOL_FILE "/tmp/media/discord/out-test-pic.png"

static void *mock_rest(void *arg) {
    (void)arg;
    while (!g_rest_stop) {
        int cfd = accept(g_rest_fd, NULL, NULL);
        if (cfd < 0) { if (g_rest_stop) break; continue; }
        struct timeval tv = {5, 0};
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        char req[4096]; size_t rn = 0;
        while (rn < sizeof(req) - 1) {
            ssize_t r = read(cfd, req + rn, sizeof(req) - 1 - rn);
            if (r <= 0) break;
            rn += (size_t)r; req[rn] = '\0';
            const char *hdr_end = strstr(req, "\r\n\r\n");
            if (!hdr_end) continue;
            const char *cl = strcasestr(req, "Content-Length:");
            size_t want = cl ? (size_t)atoi(cl + 15) : 0;
            if (rn >= (size_t)(hdr_end + 4 - req) + want) break;
        }
        const char *body = "{}";
        if (strstr(req, "PUT /api/v10/applications/app1/commands")) {
            if (strstr(req, "\"name\":\"admin\"") && strstr(req, "\"name\":\"approvals\"") &&
                strcasestr(req, "Authorization: Bot "))
                g_cmds_registered++;
            body = "[]";
        } else if (strstr(req, "POST /api/v10/interactions/int1/tok1/callback")) {
            if (strstr(req, "\"type\":4") && strstr(req, "\"flags\":64")) g_int_acks |= 1;
        } else if (strstr(req, "POST /api/v10/interactions/int2/tok2/callback")) {
            if (strstr(req, "\"flags\":64") && strstr(req, "direct messages")) g_int_acks |= 2;
        } else if (strstr(req, "POST /api/v10/interactions/int3/tok3/callback")) {
            if (strstr(req, "\"flags\":64") && strstr(req, "Only the operator")) g_int_acks |= 4;
        } else if (strstr(req, "POST /api/v10/users/@me/channels")) {
            if (strstr(req, "\"recipient_id\":\"adminuser\"")) g_dm_open_seen = 1;
            body = strstr(req, "\"recipient_id\":\"admin2\"") ? "{\"id\":\"dmchan7\"}"
                                                             : "{\"id\":\"dmchan9\"}";
        } else if (strstr(req, "POST /api/v10/channels/dmchan9/messages")) {
            if (strstr(req, "ping the admin")) g_dm_msg_seen = 1;
        } else if (strstr(req, "POST /api/v10/channels/adminuser/messages")) {
            g_bad_channel_post = 1;
        } else if (strstr(req, "POST /api/v10/channels/filechan/messages")) {
            /* headers are text; the body holds NULs, so search it by length */
#define HAS(lit) (memmem(req, rn, lit, sizeof(lit) - 1) != NULL)
            g_file_upload_ok =
                strcasestr(req, "Content-Type: multipart/form-data") &&
                HAS("name=\"payload_json\"") &&
                HAS("{\"content\":\"a picture\"}") &&
                HAS("name=\"files[0]\"; filename=\"pic.png\"") &&
                HAS("Content-Type: image/png") &&
                memmem(req, rn, FILE_BYTES, sizeof(FILE_BYTES));
        }
        char resp[512];
        int rl = snprintf(resp, sizeof(resp),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
            "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
            strlen(body), body);
        ws_write_all(cfd, resp, (size_t)rl);
        close(cfd);
    }
    return NULL;
}

static void seed_db(int port, int rest_port) {
    test_db_clean(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    char base_url[64], gw_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", rest_port);
    snprintf(gw_url, sizeof(gw_url), "ws://127.0.0.1:%d/", port);

    /* An owner marks the row as not the shipped bundle (owner NULL since
     * v59), so the runner's builtin install leaves this test copy alone. */
    sqlite3_exec(db, "INSERT OR IGNORE INTO agents(name) VALUES('Tester');"
        "INSERT OR REPLACE INTO extensions(name,path,owner_agent)"
        " VALUES('discord','" EXT_DIR "','Tester');", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT OR REPLACE INTO channels(name,extension_name)"
        " VALUES('discord','discord');", NULL, NULL, NULL);

    const char *sql = "INSERT OR REPLACE INTO config(key,value,description) VALUES(?,?,'test');";
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, sql, -1, &s, NULL);
    struct { const char *k, *v; } kv[] = {
        { "discord.bot_token",      "test-token" },
        { "discord.base_url",       base_url },
        { "discord.gateway_url",    gw_url },
        { "discord.require_mention","1" },
        { "discord.admin_ids",      "adminuser,admin2" },
        { "discord.ambient_channels","chan2" },
        { "discord.debounce_ms",     "1500" },
    };
    for (size_t i = 0; i < sizeof(kv)/sizeof(*kv); i++) {
        sqlite3_bind_text(s, 1, kv[i].k, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, kv[i].v, -1, SQLITE_TRANSIENT);
        sqlite3_step(s); sqlite3_reset(s);
    }
    sqlite3_finalize(s);
    db_close(db);
}

/* Count channel_events whose payload contains `needle`. */
static int event_count(sqlite3 *db, const char *needle) {
    sqlite3_stmt *st;
    int n = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT COUNT(*) FROM channel_events WHERE channel_name='discord'"
            " AND payload LIKE '%' || ?1 || '%';", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, needle, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return n;
}

int main(void) {
    TEST_INIT();
    printf("test_integration_channel_discord:\n");

    if (!curl_ws_available()) {
        printf("  SKIP: runtime libcurl cannot carry the WS transport "
               "(no ws/wss, or older than 8.13.0) — Discord gateway untestable here\n");
        return 0;
    }

    (void)!system("rm -f /tmp/test_integ_discord.db* /tmp/test_integ_discord.discord.*");

    int port = 0, rest_port = 0;
    g_listen_fd = ws_listen_loopback(4, 10, &port);
    /* The mock REST API (base_url). Short accept timeout: the mock loops on
     * it, so shutdown joins fast. */
    g_rest_fd = ws_listen_loopback(8, 1, &rest_port);
    if (g_listen_fd < 0 || g_rest_fd < 0) { fprintf(stderr, "FAIL: listen\n"); return 1; }

    pthread_t th, rth;
    if (pthread_create(&th, NULL, mock_gateway, NULL) != 0) { fprintf(stderr, "FAIL: thread\n"); return 1; }
    if (pthread_create(&rth, NULL, mock_rest, NULL) != 0) { fprintf(stderr, "FAIL: rest thread\n"); return 1; }

    /* Stage the SHIPPED handler (copied from templates/) as the extension. */
    mkdir(EXT_DIR, 0755);
    char *js = util_read_file("templates/channel_discord.qjs", NULL);
    if (!js) { fprintf(stderr, "FAIL: cannot read templates/channel_discord.qjs\n"); return 1; }
    FILE *jf = fopen(JS_PATH, "w");
    if (!jf) { fprintf(stderr, "FAIL: cannot write %s\n", JS_PATH); return 1; }
    fputs(js, jf); fclose(jf); free(js);
    seed_db(port, rest_port);

    pid_t pid = fork();
    if (pid == 0) {
        setenv("CCLAW_DB_PATH", DB_PATH, 1);
        execl("./build/cclaw", "cclaw", "--channel", "discord", (char *)NULL);
        _exit(127);
    }
    if (pid < 0) { fprintf(stderr, "FAIL: fork\n"); return 1; }

    sqlite3 *db = test_db_open(DB_PATH);
    int rc = 1;

    /* Poll up to ~8s for the @mention message to be emitted — it is sent last,
     * so once it lands the guild-no-mention drop has already been decided. */
    int got_mention = 0;
    for (int i = 0; i < 80 && !got_mention; i++) {
        usleep(100000);
        if (event_count(db, "help me") > 0) got_mention = 1;
    }
    if (!got_mention) FAIL("@mention guild message never reached channel_events");

    if (!g_identify_seen) FAIL("server never received IDENTIFY");

    if (event_count(db, "hi in dm") < 1) FAIL("DM message was not emitted");

    if (event_count(db, "hello world") != 0) FAIL("unmentioned guild message was NOT dropped");

    /* Heartbeats are timer-driven (first at ~interval/2, tick ~1s), so they
     * can lag the message burst — poll rather than check instantly. */
    for (int i = 0; i < 50 && !g_heartbeat_seen; i++) usleep(100000);
    if (!g_heartbeat_seen) FAIL("server never received a heartbeat");

    /* Slash commands: registered once with Discord; each interaction is
     * answered privately; only the DM /admin becomes a message for the
     * daemon, as the text a person would have typed. */
    for (int i = 0; i < 50 && (!g_cmds_registered || g_int_acks != 7); i++) usleep(100000);
    if (g_cmds_registered != 1) FAIL("slash commands were not registered exactly once");
    if (g_int_acks != 7) FAIL("a slash-command interaction was not answered privately");
    for (int i = 0; i < 50 && event_count(db, "dmchan7") < 1; i++) usleep(100000);
    if (test_qi(db,
            "SELECT COUNT(*) FROM channel_events WHERE channel_name='discord'"
            " AND json_extract(payload,'$.text')='/admin'"
            " AND json_extract(payload,'$.chat_id')='dmcmd'"
            " AND json_extract(payload,'$.chat_type')='dm'"
            " AND json_extract(payload,'$.sender_id')='adminuser';") != 1)
        FAIL("/admin picked in a DM did not reach the daemon as the text /admin");
    if (test_qi(db,
            "SELECT COUNT(*) FROM channel_events WHERE channel_name='discord'"
            " AND json_extract(payload,'$.text')='/admin'"
            " AND json_extract(payload,'$.chat_id')='dmchan7'"
            " AND json_extract(payload,'$.chat_type')='dm'"
            " AND json_extract(payload,'$.sender_id')='admin2';") != 1)
        FAIL("/admin picked in a server was not asked from the operator's DM channel");
    if (test_qi(db,
            "SELECT COUNT(*) FROM channel_events WHERE channel_name='discord'"
            " AND json_extract(payload,'$.text')='/admin'"
            " AND json_extract(payload,'$.chat_id')='chan1';") != 0)
        FAIL("/admin picked in a server was forwarded for the room");
    if (test_qi(db,
            "SELECT COUNT(*) FROM channel_state WHERE channel_name='discord'"
            " AND key='disc_cmds' AND value LIKE 'app1:%';") != 1)
        FAIL("the registered command set was not remembered");

    /* Admin notice (to_user=1): the destination is a *user* id, so the handler
     * must open a DM channel first and send there. */
    if (sqlite3_exec(db,
            "INSERT INTO channel_outbox(channel_name, session_id, payload)"
            " VALUES('discord', 0, json_object('chat_id','adminuser',"
            " 'text','ping the admin', 'to_user', 1));",
            NULL, NULL, NULL) != SQLITE_OK)
        FAIL("could not queue the admin outbox row");
    for (int i = 0; i < 80 && !g_dm_msg_seen; i++) usleep(100000);
    if (!g_dm_open_seen) FAIL("handler never opened a DM channel for the admin user id");
    if (!g_dm_msg_seen) FAIL("admin notice never delivered to the opened DM channel");
    if (g_bad_channel_post) FAIL("handler POSTed to the admin user id as if it were a channel");

    /* A file row (channel_send's `file`): one multipart post with the caption
     * and the file's exact bytes, read from the media spool by C; delivered,
     * and the spool copy removed. A row naming a file outside the spool is
     * failed without a request — the handler cannot make the runner read
     * arbitrary host files. */
    mkdir("/tmp/media", 0700);
    mkdir("/tmp/media/discord", 0700);
    FILE *sp = fopen(SPOOL_FILE, "wb");
    if (!sp || fwrite(FILE_BYTES, 1, sizeof(FILE_BYTES), sp) != sizeof(FILE_BYTES) ||
        fclose(sp) != 0)
        FAIL("could not write the spool file");
    if (sqlite3_exec(db,
            "INSERT INTO channel_outbox(channel_name, session_id, payload) VALUES"
            " ('discord', 0, json_object('chat_id','filechan','caption','a picture',"
            "   'file', json_object('path','" SPOOL_FILE "','name','pic.png',"
            "                       'mime','image/png'))),"
            " ('discord', 0, json_object('chat_id','filechan',"
            "   'file', json_object('path','/etc/hostname','name','h','mime','text/plain')));",
            NULL, NULL, NULL) != SQLITE_OK)
        FAIL("could not queue the file outbox rows");
    for (int i = 0; i < 80; i++) {
        usleep(100000);
        if (test_qi(db,
                "SELECT COUNT(*) FROM channel_outbox WHERE status IN ('pending','sending');") == 0)
            break;
    }
    if (!g_file_upload_ok) FAIL("file was not uploaded as multipart with its bytes intact");
    if (test_qi(db,
            "SELECT COUNT(*) FROM channel_outbox WHERE status='delivered'"
            " AND json_extract(payload,'$.file.name')='pic.png';") != 1)
        FAIL("file row was not acked");
    if (access(SPOOL_FILE, F_OK) == 0) FAIL("spool copy was not removed after delivery");
    if (test_qi(db,
            "SELECT COUNT(*) FROM channel_outbox WHERE status LIKE 'failed:%media spool%'"
            " AND json_extract(payload,'$.file.path')='/etc/hostname';") != 1)
        FAIL("a file outside the media spool was not refused");

    /* Ambient debounce: leading edge emits alone, the rest of the burst
     * arrives as ONE combined envelope carrying both lines with per-line
     * sender attribution. Pre-feature this was three separate events. */
    int combined = 0;
    for (int i = 0; i < 80 && !combined; i++) {
        usleep(100000);
        combined = test_qi(db,
            "SELECT COUNT(*) FROM channel_events WHERE channel_name='discord'"
            " AND payload LIKE '%bob: second line%'"
            " AND payload LIKE '%carol: third line%';");
    }
    if (combined != 1) FAIL("burst was not combined into one ambient envelope");
    if (event_count(db, "first line") != 1) FAIL("leading-edge message was not emitted alone");
    if (test_qi(db,
            "SELECT COUNT(*) FROM channel_events WHERE channel_name='discord'"
            " AND payload LIKE '%chan2%';") != 2)
        FAIL("ambient burst of 3 did not collapse to 2 envelopes");

    rc = 0;
fail:
    if (rc != 0 && db) {
        fprintf(stderr, "  -- diagnostics --\n");
        fprintf(stderr, "  identify=%d heartbeat=%d\n", g_identify_seen, g_heartbeat_seen);
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db, "SELECT payload FROM channel_events WHERE channel_name='discord';",
                               -1, &st, NULL) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW)
                fprintf(stderr, "  event: %s\n", sqlite3_column_text(st, 0));
            sqlite3_finalize(st);
        }
    }
    kill(pid, SIGTERM);
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    if (db) db_close(db);
    close(g_listen_fd);
    pthread_join(th, NULL);
    g_rest_stop = 1;
    pthread_join(rth, NULL);
    close(g_rest_fd);

    if (rc == 0) {
        if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
            if (WIFSIGNALED(wstatus))
                fprintf(stderr, "  runner killed by signal %d\n", WTERMSIG(wstatus));
            else
                fprintf(stderr, "  runner exited with status %d\n", WEXITSTATUS(wstatus));
            rc = 1;
        } else {
        }
    }

    test_db_clean(DB_PATH);
    unlink(JS_PATH);
    rmdir(EXT_DIR);
    if (rc == 0) printf("all channel_discord integration tests passed\n");
    return rc;
}
