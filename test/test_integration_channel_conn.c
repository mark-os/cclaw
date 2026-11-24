/* Integration test: `cclaw --channel` runner + channel.conn.* WebSocket
 * primitive against a loopback WS mock. Drives the Discord-shaped handshake
 * end to end:
 *   server HELLO (op 10) -> handler IDENTIFY (op 2) + heartbeat (op 1)
 *   -> server heartbeat ACK (op 11) -> server MESSAGE_CREATE (op 0)
 *   -> handler channel.emit -> channel_events row.
 * Proves: conn.open handshake, onConnOpen, bidirectional send/recv, control
 * frames consumed by C, onConnMessage reassembly, onTimer, egress refusal of
 * an off-host conn.open, and clean SIGTERM shutdown.
 *
 * The mock WS server runs in-process (a thread), so IDENTIFY / heartbeat
 * receipt is asserted directly on server-side flags; the inbound path is
 * asserted via the DB. No real network: everything is 127.0.0.1. */
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
#include "util.h"          /* curl_ws_available */
#include "test_util.h"
#include "test_ws.h"

/* The channel.conn.* WS transport needs a libcurl whose runtime protocol set
 * includes ws/wss. Amazon Linux's default `libcurl-minimal` strips it (the
 * curl_ws_* symbols still link, but ws:// perform returns "Unsupported
 * protocol"). Skip cleanly there — the test exercises the real transport as
 * soon as a WS-capable libcurl is present. */

#define DB_PATH "/tmp/test_integ_conn.db"
#define EXT_DIR "/tmp/test_integ_conn_ext"
#define JS_PATH EXT_DIR "/channel.qjs"
#define FAIL(m) do { fprintf(stderr, "FAIL: %s\n", m); goto fail; } while (0)

/* ── Fragmented text frames (server side) ── */

/* Send one text message split across `parts` WS fragments (RFC 6455: first
 * frame carries the TEXT opcode with FIN clear, the rest opcode 0, only the
 * last has FIN). Legal on any peer; the runner must rejoin them into a single
 * onConnMessage rather than dispatching the first fragment as a whole message.
 * Payloads here are short, so the 7-bit length form always suffices. */
static int ws_send_text_fragmented(int fd, const char *s, int parts) {
    size_t len = strlen(s);
    if (parts < 2) parts = 2;
    size_t chunk = len / (size_t)parts;
    if (chunk == 0) chunk = 1;
    size_t off = 0;
    for (int i = 0; off < len; i++) {
        size_t n = (i == parts - 1) ? len - off : chunk;
        if (off + n > len) n = len - off;
        int last = (off + n >= len);
        unsigned char hdr[2];
        hdr[0] = (unsigned char)((last ? 0x80 : 0x00) | (i == 0 ? 0x1 : 0x0));
        hdr[1] = (unsigned char)n;              /* n < 126 by construction */
        if (ws_write_all(fd, hdr, 2) != 0) return -1;
        if (ws_write_all(fd, s + off, n) != 0) return -1;
        off += n;
    }
    return 0;
}

/* Send one text message whose opening fragment carries no payload. Legal per
 * RFC 6455 — only the first frame carries the opcode, and its length may be 0 —
 * and emitted by peers that flush a header before the body is ready. The type
 * latch must key off "a message is open", not "bytes buffered so far":
 * otherwise the typeless CONT fragment re-latches the message as binary and the
 * whole thing is dropped. Payload stays under 126 bytes (7-bit length form). */
static int ws_send_text_empty_first(int fd, const char *s) {
    size_t len = strlen(s);
    if (len >= 126) return -1;
    unsigned char hdr[2] = { 0x01, 0x00 };          /* TEXT, FIN clear, len 0 */
    if (ws_write_all(fd, hdr, 2) != 0) return -1;
    hdr[0] = 0x80;                                  /* CONT with FIN set */
    hdr[1] = (unsigned char)len;
    if (ws_write_all(fd, hdr, 2) != 0) return -1;
    return ws_write_all(fd, s, len);
}

/* ── Mock server state (asserted directly by main) ────────────────── */
static int g_listen_fd = -1;
static volatile int g_identify_seen = 0;
static volatile int g_heartbeat_seen = 0;
static volatile int g_handshaked = 0;

static void *mock_ws_server(void *arg) {
    (void)arg;
    int cfd = ws_accept_upgrade(g_listen_fd);
    if (cfd < 0) return NULL;
    g_handshaked = 1;

    /* HELLO */
    ws_send_text(cfd, "{\"op\":10,\"d\":{\"heartbeat_interval\":45000}}");

    /* Read client frames; drive IDENTIFY -> heartbeat -> ACK -> MESSAGE_CREATE. */
    int sent_message = 0;
    for (;;) {
        char pl[4096]; size_t len = 0;
        int op = ws_read_frame(cfd, pl, sizeof(pl), &len);
        if (op < 0) break;
        if (op == 0x8) break;               /* client close */
        if (op != 0x1) continue;            /* only text matters here */
        /* Gateway op lives in the JSON: crude but sufficient for the fixture. */
        if (strstr(pl, "\"op\":2")) g_identify_seen = 1;
        if (strstr(pl, "\"op\":1")) {
            g_heartbeat_seen = 1;
            ws_send_text(cfd, "{\"op\":11}");           /* heartbeat ACK */
        }
        if (g_identify_seen && g_heartbeat_seen && !sent_message) {
            sent_message = 1;
            ws_send_text(cfd, "{\"op\":0,\"t\":\"MESSAGE_CREATE\","
                              "\"d\":{\"channel_id\":\"999\",\"content\":\"hi from gateway\"}}");
            /* Same event, fragmented: the handler's JSON.parse only succeeds if
             * C rejoined all three fragments before dispatching. */
            ws_send_text_fragmented(cfd,
                "{\"op\":0,\"t\":\"MESSAGE_CREATE\","
                "\"d\":{\"channel_id\":\"888\",\"content\":\"fragment-joined\"}}", 3);
            /* Same again, but the opening fragment is empty. */
            ws_send_text_empty_first(cfd,
                "{\"op\":0,\"t\":\"MESSAGE_CREATE\","
                "\"d\":{\"channel_id\":\"777\",\"content\":\"empty-first-joined\"}}");
        }
    }
    close(cfd);
    return NULL;
}

/* ── Test channel JS: a stripped Discord-shaped gateway handler ────── */
static void write_test_js(void) {
    FILE *f = fopen(JS_PATH, "w");
    fprintf(f,
        "var CID = 0;\n"
        "function onInit() {\n"
        "  CID = channel.conn.open({url: channel.getConfig('gw_url')});\n"
        "  channel.setState('opened', '' + CID);\n"
        "  try {\n"
        "    channel.conn.open({url: 'wss://evil.invalid/gw'});\n"   /* off-host: must throw */
        "    channel.setState('egress', 'ALLOWED');\n"
        "  } catch (e) { channel.setState('egress', 'BLOCKED'); }\n"
        "  return {};\n"
        "}\n"
        "function onConnOpen(id) { channel.setState('onopen', '' + id); }\n"
        "function onConnMessage(id, text) {\n"
        "  var m = JSON.parse(text);\n"
        "  if (m.op === 10) {\n"
        "    channel.conn.send(id, JSON.stringify({op: 2, d: {token: 'x', intents: 33280}}));\n"
        "    channel.conn.send(id, JSON.stringify({op: 1, d: null}));\n"
        "  } else if (m.op === 11) {\n"
        "    channel.setState('acked', '1');\n"
        "  } else if (m.op === 0 && m.t === 'MESSAGE_CREATE') {\n"
        "    channel.emit('message', JSON.stringify({chat_id: '' + m.d.channel_id, text: m.d.content}));\n"
        "  }\n"
        "}\n"
        "function onConnClose(id, code) { channel.setState('closed', '' + code); }\n"
        "function onTimer() { channel.setState('ticked', '1'); }\n");
    fclose(f);
}

static void seed_db(int port) {
    test_db_clean(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    char base_url[64], gw_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", port);
    snprintf(gw_url, sizeof(gw_url), "ws://127.0.0.1:%d/", port);

    sqlite3_exec(db, "INSERT OR REPLACE INTO extensions(name,path)"
        " VALUES('discx','" EXT_DIR "');", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT OR REPLACE INTO channels(name,extension_name)"
        " VALUES('discx','discx');", NULL, NULL, NULL);

    const char *sql = "INSERT OR REPLACE INTO config(key,value,description) VALUES(?,?,'test');";
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, sql, -1, &s, NULL);
    sqlite3_bind_text(s, 1, "discx.base_url", -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, base_url, -1, SQLITE_TRANSIENT);
    sqlite3_step(s); sqlite3_reset(s);
    sqlite3_bind_text(s, 1, "discx.gw_url", -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, gw_url, -1, SQLITE_TRANSIENT);
    sqlite3_step(s);
    sqlite3_finalize(s);
    db_close(db);
}

static char *state_value(sqlite3 *db, const char *key) {
    sqlite3_stmt *stmt;
    char *val = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT value FROM channel_state WHERE channel_name='discx' AND key=?;",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(stmt, 0);
            if (v) val = strdup(v);
        }
        sqlite3_finalize(stmt);
    }
    return val;
}

/* Poll up to ~6s for a channel_events payload containing `needle`. */
static int wait_event(sqlite3 *db, const char *needle) {
    for (int i = 0; i < 60; i++) {
        usleep(100000);
        sqlite3_stmt *st;
        int n = 0;
        if (sqlite3_prepare_v2(db,
                "SELECT COUNT(*) FROM channel_events WHERE channel_name='discx'"
                " AND payload LIKE '%' || ?1 || '%';", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, needle, -1, SQLITE_STATIC);
            if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
            sqlite3_finalize(st);
        }
        if (n > 0) return 1;
    }
    return 0;
}

/* Poll up to ~6s for channel_state key == want. */
static int wait_state(sqlite3 *db, const char *key, const char *want) {
    for (int i = 0; i < 60; i++) {
        usleep(100000);
        char *v = state_value(db, key);
        int hit = v && strcmp(v, want) == 0;
        free(v);
        if (hit) return 1;
    }
    return 0;
}

int main(void) {
    TEST_INIT();
    printf("test_integration_channel_conn:\n");

    if (!curl_ws_available()) {
        printf("  SKIP: runtime libcurl cannot carry the WS transport "
               "(no ws/wss, or older than 8.13.0) — conn.* untestable here\n");
        return 0;
    }

    (void)!system("rm -f /tmp/test_integ_conn.db* /tmp/test_integ_conn.discx.*");

    int port = 0;
    g_listen_fd = ws_listen_loopback(4, 10, &port);
    if (g_listen_fd < 0) { fprintf(stderr, "FAIL: listen\n"); return 1; }

    pthread_t th;
    if (pthread_create(&th, NULL, mock_ws_server, NULL) != 0) { fprintf(stderr, "FAIL: thread\n"); return 1; }

    mkdir(EXT_DIR, 0755);
    write_test_js();
    seed_db(port);

    pid_t pid = fork();
    if (pid == 0) {
        setenv("CCLAW_DB_PATH", DB_PATH, 1);
        execl("./build/cclaw", "cclaw", "--channel", "discx", (char *)NULL);
        _exit(127);
    }
    if (pid < 0) { fprintf(stderr, "FAIL: fork\n"); return 1; }

    sqlite3 *db = test_db_open(DB_PATH);
    int rc = 1;

    /* 1. onConnOpen fired with the id conn.open returned. */
    if (!wait_state(db, "onopen", "1")) FAIL("onConnOpen(1) did not fire");

    /* 2. Egress: off-host conn.open threw (refused before any connect). */
    if (!wait_state(db, "egress", "BLOCKED")) FAIL("off-host conn.open was not refused");

    /* 3. Inbound MESSAGE_CREATE -> channel.emit -> channel_events. */
    if (!wait_event(db, "hi from gateway")) FAIL("MESSAGE_CREATE did not reach channel_events");

    /* 3b. A fragmented message must arrive whole. Before the CURLWS_CONT fix
     * the first fragment was dispatched as a complete message (truncated JSON
     * -> JSON.parse throws) and the remainder was silently dropped. */
    if (!wait_event(db, "fragment-joined")) FAIL("fragmented MESSAGE_CREATE was not reassembled");

    /* 3c. Same, but the message opens with a zero-length TEXT fragment. Latching
     * the message type on an empty buffer instead of on "message started" reads
     * the type off the typeless CONT frame and drops the message as binary. */
    if (!wait_event(db, "empty-first-joined")) FAIL("message opening with a zero-length fragment was dropped");

    /* 4. Bidirectional proof: server saw IDENTIFY + heartbeat; handler saw ACK. */
    if (!g_identify_seen) FAIL("server never received IDENTIFY");
    if (!g_heartbeat_seen) FAIL("server never received heartbeat");
    if (!wait_state(db, "acked", "1")) FAIL("handler never received heartbeat ACK");

    /* 5. onTimer ticked. */
    if (!wait_state(db, "ticked", "1")) FAIL("onTimer never fired");

    rc = 0;
fail:
    if (rc != 0 && db) {
        fprintf(stderr, "  -- diagnostics --\n");
        fprintf(stderr, "  handshaked=%d identify=%d heartbeat=%d\n",
                g_handshaked, g_identify_seen, g_heartbeat_seen);
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db, "SELECT key,value FROM channel_state WHERE channel_name='discx';",
                               -1, &st, NULL) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW)
                fprintf(stderr, "  state[%s] = %s\n",
                        sqlite3_column_text(st, 0), sqlite3_column_text(st, 1));
            sqlite3_finalize(st);
        }
    }
    kill(pid, SIGTERM);
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    if (db) db_close(db);
    close(g_listen_fd);
    pthread_join(th, NULL);

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
    if (rc == 0) printf("all channel_conn integration tests passed\n");
    return rc;
}
