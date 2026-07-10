/* Integration test: `cclaw --channel` runner + mock server + pipe wake + request UDS.
 * Verifies: JS loads, long-poll receives message, shape-based outbox
 * delivery auto-acks, proxied requests over UDS reach onRequest. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include "channel_api.h"
#include "db.h"
#include "test_util.h"
#include "mock_server.h"

#define DB_PATH "/tmp/test_integ_cr.db"
#define JS_PATH "/tmp/test_integ_cr_ext/channel.qjs"
#define FAIL(m) do { fprintf(stderr, "FAIL: %s\n", m); return 1; } while(0)

/* Test channel JS exercising the new contract: poll shapes, cclaw.send
 * with outbox auto-ack, onRequest for proxied HTTP. */
static void write_test_js(void) {
    FILE *f = fopen(JS_PATH, "w");
    fprintf(f,
        "var base, token;\n"
        "function shape() {\n"
        "  return {method: 'GET', url: base + '/bot' + token + '/getUpdates?timeout=5'};\n"
        "}\n"
        "function onInit() {\n"
        "  token = channel.getConfig('bot_token');\n"
        "  base = channel.getConfig('base_url');\n"
        "  channel.log('init ok');\n"
        "  return {poll: shape()};\n"
        "}\n"
        "function onPoll(result) {\n"
        "  if (!result.error) {\n"
        "    var data = JSON.parse(result.body);\n"
        "    if (data.ok && data.result) {\n"
        "      for (var i = 0; i < data.result.length; i++) {\n"
        "        var msg = data.result[i].message;\n"
        "        if (!msg || !msg.text) continue;\n"
        "        channel.emit('message', JSON.stringify({channel_id: '' + msg.chat.id, text: msg.text}));\n"
        "      }\n"
        "    }\n"
        "  }\n"
        "  return {poll: shape()};\n"
        "}\n"
        "function onOutbox(item) {\n"
        "  var p = JSON.parse(item.payload);\n"
        "  if (p.evil_url) {\n"
        "    channel.send({method: 'POST', url: p.evil_url,\n"
        "      body: JSON.stringify({chat_id: parseInt(p.chat_id,10), text: p.text}),\n"
        "      outbox_id: item.id, final: 1});\n"
        "    return;\n"
        "  }\n"
        /* Mode ladder mirroring channel_telegram.qjs: 0=rich, 1=html, 2=plain.
         * Length check is chars, not bytes — byte-exactness is the template's
         * job (test_telegram_js); this exercises the C loop's mode plumbing. */
        "  var mode = item.mode || 0;\n"
        "  if (mode === 0 && channel.getConfig('rich_disabled') !== '1' &&\n"
        "      p.text.length <= 32768) {\n"
        "    channel.send({method: 'POST', url: base + '/bot' + token + '/sendRichMessage',\n"
        "      body: JSON.stringify({chat_id: parseInt(p.chat_id,10),\n"
        "                            rich_message: {markdown: p.text}}),\n"
        "      outbox_id: item.id, final: 1});\n"
        "    return;\n"
        "  }\n"
        "  channel.send({method: 'POST', url: base + '/bot' + token + '/sendMessage',\n"
        "    body: JSON.stringify({chat_id: parseInt(p.chat_id,10), text: p.text,\n"
        "                          html: mode === 1}),\n"
        "    outbox_id: item.id, final: 1});\n"
        "}\n"
        "function onRequest(req) {\n"
        "  if ((req.headers['X-Test'] || '') !== '1') return {status: 401, body: 'no'};\n"
        "  channel.emit('message', JSON.stringify({channel_id: 'uds', text: req.body}));\n"
        "  return {status: 201, body: 'got-it'};\n"
        "}\n");
    fclose(f);
}

static sqlite3 *setup_db(int port) {
    unlink(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    if (!db) return NULL;

    char base_url[128];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", port);

    sqlite3_exec(db, "INSERT OR REPLACE INTO extensions(name,path,builtin)"
        " VALUES('test','/tmp/test_integ_cr_ext','0');", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT OR REPLACE INTO channels(name,extension_name)"
        " VALUES('test','test');", NULL, NULL, NULL);

    const char *sql = "INSERT OR REPLACE INTO channel_state(channel_name,key,value) VALUES(?,?,?);";
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, sql, -1, &s, NULL);
    sqlite3_bind_text(s, 1, "test", -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, "bot_token", -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, "test-token", -1, SQLITE_STATIC);
    sqlite3_step(s); sqlite3_reset(s);
    sqlite3_bind_text(s, 1, "test", -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, "base_url", -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, base_url, -1, SQLITE_STATIC);
    sqlite3_step(s);
    sqlite3_finalize(s);
    return db;
}

/* Poll ~4s until the outbox row whose payload contains `marker` reaches a
 * status starting with `want`. Returns 1 on success, 0 on timeout. */
static int wait_outbox_status(sqlite3 *db, const char *marker, const char *want) {
    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT status FROM channel_outbox WHERE channel_name='test'"
        " AND payload LIKE '%%%s%%' LIMIT 1;", marker);
    for (int attempt = 0; attempt < 40; attempt++) {
        usleep(100000);
        sqlite3_stmt *stmt;
        int hit = 0;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *st = (const char *)sqlite3_column_text(stmt, 0);
                if (st && strncmp(st, want, strlen(want)) == 0) hit = 1;
            }
            sqlite3_finalize(stmt);
        }
        if (hit) return 1;
    }
    return 0;
}

/* deliver_mode of the outbox row whose payload contains `marker`, or -1. */
static int outbox_mode(sqlite3 *db, const char *marker) {
    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT deliver_mode FROM channel_outbox WHERE channel_name='test'"
        " AND payload LIKE '%%%s%%' LIMIT 1;", marker);
    int mode = -1;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) mode = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return mode;
}

/* channel_state value for the test channel, or NULL. Caller frees. */
static char *state_value(sqlite3 *db, const char *key) {
    sqlite3_stmt *stmt;
    char *val = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT value FROM channel_state WHERE channel_name='test' AND key=?;",
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

/* Send a request envelope over the runner's UDS, return "status\nbody"
 * reply (caller frees) or NULL. */
static char *uds_request(const char *envelope) {
    char *path = channel_uds_path(DB_PATH, "test");
    if (!path) return NULL;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { free(path); return NULL; }
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    free(path);
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { close(fd); return NULL; }
    size_t elen = strlen(envelope), off = 0;
    while (off < elen) {
        ssize_t n = write(fd, envelope + off, elen - off);
        if (n <= 0) { close(fd); return NULL; }
        off += (size_t)n;
    }
    shutdown(fd, SHUT_WR);
    char *reply = malloc(8192);
    size_t rlen = 0;
    for (;;) {
        ssize_t n = read(fd, reply + rlen, 8191 - rlen);
        if (n > 0) { rlen += (size_t)n; continue; }
        break;
    }
    close(fd);
    reply[rlen] = '\0';
    return reply;
}

int main(void) {
    /* Line-buffer stdout: a timeout-killed run must not lose progress output */
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_integration_channel_runner:\n");

    /* Stale debris from a killed previous run (wal/shm/pipe/sock) makes
     * this test hang nondeterministically — clean the whole family */
    (void)system("rm -f /tmp/test_integ_cr.db* /tmp/test_integ_cr.test.*");

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start");
    mock_tg_enable("test-token");

    mock_tg_enqueue_updates(
        "{\"ok\":true,\"result\":[{\"update_id\":1,"
        "\"message\":{\"message_id\":1,\"chat\":{\"id\":42},\"text\":\"hello from test\"}}]}");
    mock_tg_enqueue_updates("{\"ok\":true,\"result\":[]}");
    mock_tg_enqueue_updates("{\"ok\":true,\"result\":[]}");
    mock_tg_enqueue_send("{\"ok\":true,\"result\":{\"message_id\":2}}");

    sqlite3 *db = setup_db(port);
    if (!db) { mock_server_stop(); FAIL("db_open"); }
    db_close(db);

    mkdir("/tmp/test_integ_cr_ext", 0755);
    write_test_js();

    /* Channels now run as `cclaw --channel <name>` (the daemon fork+execs this;
     * no separate channel_runner binary). Point it at the test DB via env. */
    pid_t pid = fork();
    if (pid == 0) {
        setenv("CCLAW_DB_PATH", DB_PATH, 1);
        execl("./build/cclaw", "cclaw", "--channel", "test", (char *)NULL);
        _exit(127);
    }
    if (pid < 0) { mock_server_stop(); FAIL("fork"); }

    db = test_db_open(DB_PATH);
    if (!db) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); mock_server_stop(); FAIL("db reopen"); }

    /* 1. Long-poll → onPoll → emit */
    int found_event = 0;
    for (int attempt = 0; attempt < 40 && !found_event; attempt++) {
        usleep(100000);
        const char *check = "SELECT payload FROM channel_events WHERE channel_name='test' LIMIT 1;";
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, check, -1, &stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *p = (const char *)sqlite3_column_text(stmt, 0);
                if (p && strstr(p, "hello from test")) found_event = 1;
            }
            sqlite3_finalize(stmt);
        }
    }
    if (!found_event) {
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        db_close(db); mock_server_stop();
        FAIL("channel_events not populated");
    }
    printf("  PASS: incoming message -> channel_events\n");

    /* 2. Outbox mode 0 → one sendRichMessage carrying the raw markdown → auto-ack */
    sqlite3_exec(db,
        "INSERT INTO channel_outbox(channel_name, session_id, payload) VALUES"
        "('test', 1, '{\"chat_id\":\"42\",\"text\":\"**reply** from agent\"}')",
        NULL, NULL, NULL);
    channel_outbox_wake(DB_PATH, "test");

    if (!wait_outbox_status(db, "reply", "delivered")) {
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        db_close(db); mock_server_stop();
        FAIL("rich outbox not auto-acked");
    }
    if (mock_tg_rich_count() != 1) {
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        db_close(db); mock_server_stop();
        FAIL("expected exactly one sendRichMessage request");
    }
    {
        const char *rb = mock_tg_last_rich_body();
        if (!rb || !strstr(rb, "\"rich_message\":{\"markdown\":\"**reply** from agent\"}")) {
            fprintf(stderr, "  rich body was: %s\n", rb ? rb : "(null)");
            kill(pid, SIGTERM); waitpid(pid, NULL, 0);
            db_close(db); mock_server_stop();
            FAIL("rich_message.markdown not raw markdown");
        }
    }
    if (mock_tg_send_count() != 0) {
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        db_close(db); mock_server_stop();
        FAIL("rich delivery must not also hit sendMessage");
    }
    printf("  PASS: outbox wake -> sendRichMessage raw markdown -> auto-ack\n");

    /* 2a. Rich 400 → same row re-delivered as HTML (deliver_mode=1) */
    mock_tg_enqueue_rich(400,
        "{\"ok\":false,\"error_code\":400,\"description\":\"Bad Request: rich message invalid\"}");
    sqlite3_exec(db,
        "INSERT INTO channel_outbox(channel_name, session_id, payload) VALUES"
        "('test', 1, '{\"chat_id\":\"42\",\"text\":\"rich400row\"}')",
        NULL, NULL, NULL);
    channel_outbox_wake(DB_PATH, "test");

    if (!wait_outbox_status(db, "rich400row", "delivered")) {
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        db_close(db); mock_server_stop();
        FAIL("rich-400 row not re-delivered");
    }
    if (outbox_mode(db, "rich400row") != 1) {
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        db_close(db); mock_server_stop();
        FAIL("rich-400 row did not degrade to deliver_mode=1");
    }
    {
        const char *sb = mock_tg_last_send_body();
        if (!sb || !strstr(sb, "rich400row") || !strstr(sb, "\"html\":true")) {
            fprintf(stderr, "  send body was: %s\n", sb ? sb : "(null)");
            kill(pid, SIGTERM); waitpid(pid, NULL, 0);
            db_close(db); mock_server_stop();
            FAIL("rich-400 re-delivery did not arrive as HTML sendMessage");
        }
    }
    {
        char *latch = state_value(db, "rich_disabled");
        if (latch) {
            free(latch);
            kill(pid, SIGTERM); waitpid(pid, NULL, 0);
            db_close(db); mock_server_stop();
            FAIL("a plain 400 must not latch rich_disabled");
        }
    }
    printf("  PASS: rich 400 -> re-delivered as HTML, no latch\n");

    /* 2b. >32KB text → no rich attempt, straight to sendMessage */
    {
        int rich_before = mock_tg_rich_count();
        /* hex(zeroblob(16500)) = 33000 chars of '0' — over the 32768 cap */
        sqlite3_exec(db,
            "INSERT INTO channel_outbox(channel_name, session_id, payload) VALUES"
            "('test', 1, json_object('chat_id','42','text', hex(zeroblob(16500)) || 'bigrow'))",
            NULL, NULL, NULL);
        channel_outbox_wake(DB_PATH, "test");

        if (!wait_outbox_status(db, "bigrow", "delivered")) {
            kill(pid, SIGTERM); waitpid(pid, NULL, 0);
            db_close(db); mock_server_stop();
            FAIL("oversize row not delivered");
        }
        if (mock_tg_rich_count() != rich_before) {
            kill(pid, SIGTERM); waitpid(pid, NULL, 0);
            db_close(db); mock_server_stop();
            FAIL("oversize row must not attempt sendRichMessage");
        }
    }
    printf("  PASS: >32KB text skips rich, delivered chunked\n");

    /* 2c. Rich 404 "method not found" → HTML re-delivery + rich_disabled
     * latch; the NEXT row skips rich entirely. */
    mock_tg_enqueue_rich(404,
        "{\"ok\":false,\"error_code\":404,\"description\":\"Not Found: method not found\"}");
    sqlite3_exec(db,
        "INSERT INTO channel_outbox(channel_name, session_id, payload) VALUES"
        "('test', 1, '{\"chat_id\":\"42\",\"text\":\"rich404row\"}')",
        NULL, NULL, NULL);
    channel_outbox_wake(DB_PATH, "test");

    if (!wait_outbox_status(db, "rich404row", "delivered")) {
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        db_close(db); mock_server_stop();
        FAIL("rich-404 row not re-delivered");
    }
    {
        char *latch = state_value(db, "rich_disabled");
        int latched = latch && strcmp(latch, "1") == 0;
        free(latch);
        if (!latched) {
            kill(pid, SIGTERM); waitpid(pid, NULL, 0);
            db_close(db); mock_server_stop();
            FAIL("rich 404 did not latch rich_disabled=1");
        }
    }
    {
        int rich_before = mock_tg_rich_count();
        sqlite3_exec(db,
            "INSERT INTO channel_outbox(channel_name, session_id, payload) VALUES"
            "('test', 1, '{\"chat_id\":\"42\",\"text\":\"afterlatch\"}')",
            NULL, NULL, NULL);
        channel_outbox_wake(DB_PATH, "test");
        if (!wait_outbox_status(db, "afterlatch", "delivered")) {
            kill(pid, SIGTERM); waitpid(pid, NULL, 0);
            db_close(db); mock_server_stop();
            FAIL("post-latch row not delivered");
        }
        if (mock_tg_rich_count() != rich_before) {
            kill(pid, SIGTERM); waitpid(pid, NULL, 0);
            db_close(db); mock_server_stop();
            FAIL("post-latch row still attempted sendRichMessage");
        }
    }
    printf("  PASS: rich 404 latches rich_disabled; later rows skip rich\n");

    /* 2b. base_url pin: an outbox item whose send URL points at a DIFFERENT
     * host than the channel's configured base_url must be refused before any
     * curl request is made (url_host_allowed → make_easy NULL → synchronous
     * channel_fail_outbox). The mock server's send count must not increase and
     * the row must go terminally failed (a host mismatch is not transient, so
     * it is not retried). 10.255.255.1 is a different host from 127.0.0.1. */
    int sends_before = mock_tg_send_count();
    sqlite3_exec(db,
        "INSERT INTO channel_outbox(channel_name, session_id, payload) VALUES"
        "('test', 1, '{\"evil_url\":\"http://10.255.255.1:1/evil\",\"chat_id\":\"42\",\"text\":\"pinrow\"}')",
        NULL, NULL, NULL);
    channel_outbox_wake(DB_PATH, "test");

    int failed = 0;
    for (int attempt = 0; attempt < 40 && !failed; attempt++) {
        usleep(100000);
        const char *fc = "SELECT status FROM channel_outbox WHERE channel_name='test'"
                         " AND payload LIKE '%pinrow%' LIMIT 1;";
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, fc, -1, &stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *st = (const char *)sqlite3_column_text(stmt, 0);
                if (st && strncmp(st, "failed", 6) == 0) failed = 1;
            }
            sqlite3_finalize(stmt);
        }
    }
    if (!failed) {
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        db_close(db); mock_server_stop();
        FAIL("off-host outbox send was not failed");
    }
    if (mock_tg_send_count() != sends_before) {
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        db_close(db); mock_server_stop();
        FAIL("off-host outbox send reached the mock server");
    }
    printf("  PASS: base_url pin refuses off-host outbox send\n");

    /* 2c. Transient failure → retry (not permanent fail). A send to the pinned
     * host but an unreachable port (127.0.0.1:1, connection refused) is a
     * transient error: the row must be rescheduled to 'pending' with attempts
     * incremented (channel_retry_outbox), NOT dropped/failed on first try.
     * The send never reaches the mock (wrong port), so send_count is unchanged. */
    sends_before = mock_tg_send_count();
    sqlite3_exec(db,
        "INSERT INTO channel_outbox(channel_name, session_id, payload) VALUES"
        "('test', 1, '{\"evil_url\":\"http://127.0.0.1:1/evil\",\"chat_id\":\"42\",\"text\":\"retryrow\"}')",
        NULL, NULL, NULL);
    channel_outbox_wake(DB_PATH, "test");

    int retried = 0;
    for (int attempt = 0; attempt < 40 && !retried; attempt++) {
        usleep(100000);
        const char *rc = "SELECT status, attempts FROM channel_outbox"
                         " WHERE channel_name='test' AND payload LIKE '%retryrow%' LIMIT 1;";
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, rc, -1, &stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *st = (const char *)sqlite3_column_text(stmt, 0);
                int att = sqlite3_column_int(stmt, 1);
                /* Retrying: bumped attempt count, back in 'pending' (a terminal
                 * fail would take MAX_OUTBOX_ATTEMPTS ≈ 31s, impossible here). */
                if (att >= 1 && st && strcmp(st, "pending") == 0) retried = 1;
            }
            sqlite3_finalize(stmt);
        }
    }
    if (!retried) {
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        db_close(db); mock_server_stop();
        FAIL("transient outbox failure was not retried");
    }
    if (mock_tg_send_count() != sends_before) {
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        db_close(db); mock_server_stop();
        FAIL("retry-path outbox send reached the mock server");
    }
    printf("  PASS: transient outbox failure is retried with backoff\n");

    /* 3. Proxied request over UDS → onRequest → reply + emit */
    char *reply = uds_request(
        "{\"method\":\"POST\",\"path\":\"/hook/test\","
        "\"headers\":{\"X-Test\":\"1\"},\"body\":\"webhook-payload\"}");
    if (!reply) {
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        db_close(db); mock_server_stop();
        FAIL("uds request failed");
    }
    if (strncmp(reply, "201\ngot-it", 10) != 0) {
        fprintf(stderr, "  reply was: %s\n", reply);
        free(reply);
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        db_close(db); mock_server_stop();
        FAIL("unexpected uds reply");
    }
    free(reply);

    int uds_event = 0;
    for (int attempt = 0; attempt < 40 && !uds_event; attempt++) {
        usleep(100000);
        const char *check = "SELECT payload FROM channel_events WHERE payload LIKE '%webhook-payload%' LIMIT 1;";
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, check, -1, &stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) uds_event = 1;
            sqlite3_finalize(stmt);
        }
    }
    if (!uds_event) {
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        db_close(db); mock_server_stop();
        FAIL("uds request event not emitted");
    }

    /* Bad auth handled by channel JS, not C */
    reply = uds_request(
        "{\"method\":\"POST\",\"path\":\"/hook/test\",\"headers\":{},\"body\":\"x\"}");
    if (!reply || strncmp(reply, "401\n", 4) != 0) {
        fprintf(stderr, "  reply was: %s\n", reply ? reply : "(null)");
        free(reply);
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        db_close(db); mock_server_stop();
        FAIL("expected 401 from JS verification");
    }
    free(reply);
    printf("  PASS: UDS request -> onRequest -> reply + emit + JS auth\n");

    kill(pid, SIGTERM);
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    db_close(db);
    mock_server_stop();

    /* The runner must exit cleanly (0) after SIGTERM — a GC assertion
     * failure would show as signal 6 (SIGABRT) / exit code 134. */
    if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
        if (WIFSIGNALED(wstatus))
            fprintf(stderr, "  runner killed by signal %d\n", WTERMSIG(wstatus));
        else
            fprintf(stderr, "  runner exited with status %d\n", WEXITSTATUS(wstatus));
        FAIL("channel runner did not exit 0 after SIGTERM");
    }
    printf("  PASS: clean shutdown after SIGTERM (exit 0)\n");

    unlink(DB_PATH);
    unlink(JS_PATH);
    rmdir("/tmp/test_integ_cr_ext");

    printf("all channel_runner integration tests passed\n");
    return 0;
}
