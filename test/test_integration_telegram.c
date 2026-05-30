/* T129: integration test — mock Telegram API.
 * Verify poll→inbox→agent→deliver cycle end-to-end using mock endpoints. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <curl/curl.h>
#include "mock_server.h"
#include "telegram.h"
#include "db.h"
#include "config.h"
#include "daemon.h"
static int s_port;

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

#define TOKEN "test-token-129"

/* Helper: wait for condition with timeout (ms) */
static int wait_for(int (*check)(void *), void *arg, int timeout_ms) {
    for (int i = 0; i < timeout_ms / 10; i++) {
        if (check(arg)) return 1;
        usleep(10000);
    }
    mock_server_stop();
    return 0;
}

/* T129: Telegram poller receives message → inbox populated */
static int inbox_has_items(void *arg) {
    sqlite3 *db = (sqlite3 *)arg;
    sqlite3_stmt *stmt;
    const char *sql = "SELECT COUNT(*) FROM inbox WHERE consumed=0;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count > 0;
}

static void test_poll_to_inbox(void) {
    TEST(poll_to_inbox);

    mock_server_reset();
    mock_tg_enable(TOKEN);

    /* Enqueue a getUpdates response with one message */
    mock_tg_enqueue_updates(
        "{\"ok\":true,\"result\":[{\"update_id\":100,"
        "\"message\":{\"message_id\":1,\"chat\":{\"id\":12345},"
        "\"text\":\"Hello from Telegram\"}}]}");

    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open failed"); }

    /* Init signal pipe (needed by process_message → daemon_signal_session) */
    daemon_signal_init();

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", s_port);

    Config cfg = {0};
    cfg.telegram_token = TOKEN;
    cfg.telegram_base_url = base_url;
    cfg.db_path = ":memory:";
    cfg.workspace = "./workspace";

    int rc = telegram_start(&cfg, db);
    if (rc != 0) { db_close(db); daemon_signal_close(); FAIL("telegram_start failed"); }

    /* Wait for inbox to be populated */
    if (!wait_for(inbox_has_items, db, 3000)) {
        telegram_stop(); db_close(db); daemon_signal_close();
        FAIL("inbox not populated within timeout");
    }

    /* Verify inbox content */
    sqlite3_stmt *stmt;
    const char *sql = "SELECT payload, source FROM inbox WHERE consumed=0;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        telegram_stop(); db_close(db); daemon_signal_close();
        FAIL("query failed");
    }
    int found = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *payload = (const char *)sqlite3_column_text(stmt, 0);
        const char *source = (const char *)sqlite3_column_text(stmt, 1);
        if (payload && strstr(payload, "Hello from Telegram") &&
            source && strcmp(source, "telegram") == 0) {
            found = 1;
        }
    }
    sqlite3_finalize(stmt);

    telegram_stop();

    if (!found) { db_close(db); daemon_signal_close(); FAIL("inbox payload mismatch"); }

    db_close(db);
    daemon_signal_close();
    PASS();
}

/* T129/V25: Telegram offset persisted in DB across polls */
static int offset_persisted(void *arg) {
    sqlite3 *db = (sqlite3 *)arg;
    char *val = db_kv_get(db, "tg_offset");
    int ok = (val != NULL);
    free(val);
    return ok;
}

static void test_offset_persistence(void) {
    TEST(offset_persistence);

    mock_server_reset();
    mock_tg_enable(TOKEN);

    /* Enqueue update with update_id=200 */
    mock_tg_enqueue_updates(
        "{\"ok\":true,\"result\":[{\"update_id\":200,"
        "\"message\":{\"message_id\":2,\"chat\":{\"id\":99},"
        "\"text\":\"test offset\"}}]}");

    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open failed"); }
    daemon_signal_init();

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", s_port);

    Config cfg = {0};
    cfg.telegram_token = TOKEN;
    cfg.telegram_base_url = base_url;
    cfg.db_path = ":memory:";
    cfg.workspace = "./workspace";

    telegram_start(&cfg, db);

    if (!wait_for(offset_persisted, db, 3000)) {
        telegram_stop(); db_close(db); daemon_signal_close();
        FAIL("offset not persisted within timeout");
    }

    /* Offset should be update_id + 1 = 201 */
    char *val = db_kv_get(db, "tg_offset");
    int ok = (val && strcmp(val, "201") == 0);
    free(val);

    telegram_stop();
    db_close(db);
    daemon_signal_close();

    if (!ok) FAIL("offset should be 201");
    PASS();
}

/* T129: sendMessage delivery via telegram_send_message */
static void test_send_message_delivery(void) {
    TEST(send_message_delivery);

    mock_server_reset();
    mock_tg_enable(TOKEN);

    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open failed"); }
    daemon_signal_init();

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", s_port);

    /* Need to start telegram so g_cfg is set (tg_url uses it) */
    Config cfg = {0};
    cfg.telegram_token = TOKEN;
    cfg.telegram_base_url = base_url;
    cfg.db_path = ":memory:";
    cfg.workspace = "./workspace";

    telegram_start(&cfg, db);

    /* Send a message */
    telegram_send_message(TOKEN, 12345, "Agent response text");

    /* Give mock server time to process */
    usleep(100000);

    if (mock_tg_send_count() < 1) {
        telegram_stop(); db_close(db); daemon_signal_close();
        FAIL("sendMessage not called");
    }

    const char *body = mock_tg_last_send_body();
    if (!body || !strstr(body, "Agent response text")) {
        telegram_stop(); db_close(db); daemon_signal_close();
        FAIL("sendMessage body mismatch");
    }
    if (!strstr(body, "12345")) {
        telegram_stop(); db_close(db); daemon_signal_close();
        FAIL("sendMessage chat_id mismatch");
    }

    telegram_stop();
    db_close(db);
    daemon_signal_close();
    PASS();
}

/* T129: chat_id → session routing (new session created) */
static void test_chat_session_routing(void) {
    TEST(chat_session_routing);

    mock_server_reset();
    mock_tg_enable(TOKEN);

    /* Two messages from same chat_id */
    mock_tg_enqueue_updates(
        "{\"ok\":true,\"result\":["
        "{\"update_id\":300,\"message\":{\"message_id\":1,\"chat\":{\"id\":777},\"text\":\"msg1\"}},"
        "{\"update_id\":301,\"message\":{\"message_id\":2,\"chat\":{\"id\":777},\"text\":\"msg2\"}}"
        "]}");

    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open failed"); }
    daemon_signal_init();

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d", s_port);

    Config cfg = {0};
    cfg.telegram_token = TOKEN;
    cfg.telegram_base_url = base_url;
    cfg.db_path = ":memory:";
    cfg.workspace = "./workspace";

    telegram_start(&cfg, db);

    /* Wait for both messages to arrive */
    for (int i = 0; i < 300; i++) {
        sqlite3_stmt *stmt;
        int count = 0;
        if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM inbox WHERE consumed=0;", -1, &stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
        }
        if (count >= 2) break;
        usleep(10000);
    }

    telegram_stop();

    /* Both should route to same session */
    sqlite3_stmt *stmt;
    int session_count = 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(DISTINCT session_id) FROM inbox;", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) session_count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }

    db_close(db);
    daemon_signal_close();

    if (session_count != 1) FAIL("expected both messages in same session");
    PASS();
}

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    s_port = mock_server_start();
    printf("--- test_integration_telegram (T129) ---\n");
    test_poll_to_inbox();
    test_offset_persistence();
    test_send_message_delivery();
    test_chat_session_routing();
    printf("%d/%d passed\n", tests_passed, tests_run);
    curl_global_cleanup();
    return tests_passed == tests_run ? 0 : 1;
}
