/* Integration test: channel_runner + mock server + pipe wake.
 * Verifies: JS loads, long-poll receives message, outbox delivery works. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <assert.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include "channel_api.h"
#include "db.h"
#include "mock_server.h"

#define DB_PATH "/tmp/test_integ_cr.db"
#define JS_PATH "/tmp/test_integ_cr_ext/channel.js"
#define FAIL(m) do { fprintf(stderr, "FAIL: %s\n", m); return 1; } while(0)

/* Write the test channel JS that talks to mock server */
static void write_test_js(void) {
    FILE *f = fopen(JS_PATH, "w");
    fprintf(f,
        "function onInit() {\n"
        "  var token = cclaw.getConfig('bot_token');\n"
        "  var base = cclaw.getConfig('base_url');\n"
        "  cclaw.log('init ok');\n"
        "  return {poll_type: 'http_long_poll',\n"
        "    url: base + '/bot' + token + '/getUpdates?timeout=5'};\n"
        "}\n"
        "function getNextUrl() {\n"
        "  var token = cclaw.getConfig('bot_token');\n"
        "  var base = cclaw.getConfig('base_url');\n"
        "  return base + '/bot' + token + '/getUpdates?timeout=5';\n"
        "}\n"
        "function onNetwork(response) {\n"
        "  var data = JSON.parse(response);\n"
        "  if (!data.ok || !data.result) return;\n"
        "  for (var i = 0; i < data.result.length; i++) {\n"
        "    var msg = data.result[i].message;\n"
        "    if (!msg || !msg.text) continue;\n"
        "    var chatId = '' + msg.chat.id;\n"
        "    cclaw.emit('message', JSON.stringify({channel_id: chatId, text: msg.text}));\n"
        "  }\n"
        "}\n"
        "function onOutbox(item) {\n"
        "  var p = JSON.parse(item.payload);\n"
        "  var token = cclaw.getConfig('bot_token');\n"
        "  var base = cclaw.getConfig('base_url');\n"
        "  var url = base + '/bot' + token + '/sendMessage';\n"
        "  cclaw.httpPost(url, JSON.stringify({chat_id: parseInt(p.chat_id,10), text: p.text}));\n"
        "  cclaw.ackOutbox(item.id);\n"
        "}\n");
    fclose(f);
}

static sqlite3 *setup_db(int port) {
    unlink(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    if (!db) return NULL;

    /* Seed channel_state */
    char base_url[128];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", port);

    /* Seed extensions + channels tables */
    sqlite3_exec(db, "INSERT OR REPLACE INTO extensions(name,path,builtin)"
        " VALUES('test','/tmp','0');", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT OR REPLACE INTO channels(name,extension_name)"
        " VALUES('test','test');", NULL, NULL, NULL);

    /* Override extension path to point at our test JS dir */
    char path_dir[128];
    snprintf(path_dir, sizeof(path_dir), "/tmp");
    sqlite3_exec(db, "UPDATE extensions SET path='/tmp/test_integ_cr_ext' WHERE name='test';",
        NULL, NULL, NULL);

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

int main(void) {
    printf("test_integration_channel_runner:\n");

    /* Start mock server */
    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start");
    mock_tg_enable("test-token");

    /* Enqueue a getUpdates response with one message */
    mock_tg_enqueue_updates(
        "{\"ok\":true,\"result\":[{\"update_id\":1,"
        "\"message\":{\"message_id\":1,\"chat\":{\"id\":42},\"text\":\"hello from test\"}}]}");
    /* Second call returns empty (keeps runner alive briefly) */
    mock_tg_enqueue_updates("{\"ok\":true,\"result\":[]}");
    mock_tg_enqueue_updates("{\"ok\":true,\"result\":[]}");

    /* Enqueue sendMessage success */
    mock_tg_enqueue_send("{\"ok\":true,\"result\":{\"message_id\":2}}");

    /* Setup DB */
    sqlite3 *db = setup_db(port);
    if (!db) { mock_server_stop(); FAIL("db_open"); }
    db_close(db);

    /* Write test JS to extension dir */
    mkdir("/tmp/test_integ_cr_ext", 0755);
    write_test_js();

    /* Spawn channel_runner */
    pid_t pid = fork();
    if (pid == 0) {
        execl("./build/channel_runner", "channel_runner", DB_PATH, "test", JS_PATH, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) { mock_server_stop(); FAIL("fork"); }

    /* Wait for the runner to process the getUpdates response */
    /* Poll DB until channel_events appears or timeout */
    db = test_db_open(DB_PATH);
    if (!db) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); mock_server_stop(); FAIL("db reopen"); }

    int found_event = 0;
    for (int attempt = 0; attempt < 40 && !found_event; attempt++) {
        usleep(100000); /* 100ms */
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
    printf("  PASS: incoming message → channel_events\n");

    /* Test outbox delivery: insert an outbox row + wake the FIFO */
    channel_outbox_insert(db, "test", 1,
        "{\"chat_id\":\"42\",\"text\":\"reply from agent\"}");
    channel_outbox_wake(DB_PATH, "test");

    /* Wait for runner to deliver */
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

    /* Verify outbox row was acked */
    const char *ack_check = "SELECT status FROM channel_outbox WHERE channel_name='test' LIMIT 1;";
    sqlite3_stmt *stmt;
    int acked = 0;
    if (sqlite3_prepare_v2(db, ack_check, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *st = (const char *)sqlite3_column_text(stmt, 0);
            if (st && strcmp(st, "delivered") == 0) acked = 1;
        }
        sqlite3_finalize(stmt);
    }

    if (!acked) {
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        db_close(db); mock_server_stop();
        FAIL("outbox not acked");
    }
    printf("  PASS: outbox wake → sendMessage → ack\n");

    /* Cleanup */
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    db_close(db);
    mock_server_stop();
    unlink(DB_PATH);
    unlink(JS_PATH);
    unlink("/tmp/test_integ_cr_ext");

    printf("all channel_runner integration tests passed\n");
    return 0;
}
