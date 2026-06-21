/* Integration test: channel_runner + mock server + pipe wake + request UDS.
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
#define JS_PATH "/tmp/test_integ_cr_ext/channel.mjs"
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
        "  channel.send({method: 'POST', url: base + '/bot' + token + '/sendMessage',\n"
        "    body: JSON.stringify({chat_id: parseInt(p.chat_id,10), text: p.text}),\n"
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

    pid_t pid = fork();
    if (pid == 0) {
        execl("./build/channel_runner", "channel_runner", DB_PATH, "test", (char *)NULL);
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

    /* 2. Outbox → cclaw.send shape → C executes → auto-ack */
    sqlite3_exec(db,
        "INSERT INTO channel_outbox(channel_name, session_id, payload) VALUES"
        "('test', 1, '{\"chat_id\":\"42\",\"text\":\"reply from agent\"}')",
        NULL, NULL, NULL);
    channel_outbox_wake(DB_PATH, "test");

    int sends = 0;
    for (int attempt = 0; attempt < 40 && sends < 1; attempt++) {
        usleep(100000);
        sends = mock_tg_send_count();
    }
    if (sends < 1) {
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        db_close(db); mock_server_stop();
        FAIL("sendMessage not called");
    }

    int acked = 0;
    for (int attempt = 0; attempt < 40 && !acked; attempt++) {
        usleep(100000);
        const char *ack_check = "SELECT status FROM channel_outbox WHERE channel_name='test' LIMIT 1;";
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, ack_check, -1, &stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *st = (const char *)sqlite3_column_text(stmt, 0);
                if (st && strcmp(st, "delivered") == 0) acked = 1;
            }
            sqlite3_finalize(stmt);
        }
    }
    if (!acked) {
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        db_close(db); mock_server_stop();
        FAIL("outbox not auto-acked");
    }
    printf("  PASS: outbox wake -> shape send -> auto-ack\n");

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
    waitpid(pid, NULL, 0);
    db_close(db);
    mock_server_stop();
    unlink(DB_PATH);
    unlink(JS_PATH);
    rmdir("/tmp/test_integ_cr_ext");

    printf("all channel_runner integration tests passed\n");
    return 0;
}
