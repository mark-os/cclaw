#include "channel_api.h"
#include "wake.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_DB "test_channel_api.db"
#define TEST_FIFO "test_channel_api.pipe"

static void cleanup(void) {
    test_db_clean(TEST_DB);
    unlink(TEST_FIFO);
    /* Re-create schema so channel_ctx_open has tables ready */
    sqlite3 *db = test_db_open(TEST_DB);
    if (db) db_close(db);
}

static void test_channel_emit(void) {
    cleanup();
    /* Create FIFO so wake_external succeeds */
    mkfifo(TEST_FIFO, 0600);
    /* Open read end non-blocking (so write doesn't block) */
    int rfd = open(TEST_FIFO, O_RDONLY | O_NONBLOCK);
    assert(rfd >= 0);

    ChannelCtx *ctx = channel_ctx_open(TEST_DB, "telegram");
    assert(ctx);

    int rc = channel_emit(ctx, "message", "{\"chat_id\":123,\"text\":\"hello\"}", NULL);
    assert(rc == 0);

    /* Verify row in channel_events */
    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(ctx->db,
        "SELECT channel_name, event_type, payload FROM channel_events", -1, &stmt, NULL);
    assert(rc == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "telegram") == 0);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 1), "message") == 0);
    assert(strstr((const char *)sqlite3_column_text(stmt, 2), "hello") != NULL);
    sqlite3_finalize(stmt);

    /* Verify FIFO received 1 byte */
    char buf[16];
    ssize_t n = read(rfd, buf, sizeof(buf));
    assert(n == 1);

    close(rfd);
    channel_ctx_free(ctx);
}

static void test_channel_config(void) {
    cleanup();
    ChannelCtx *ctx = channel_ctx_open(TEST_DB, "telegram");
    assert(ctx);

    /* Initially empty */
    char *val = channel_get_config(ctx, "offset");
    assert(val == NULL);

    /* Set and get */
    int rc = channel_set_config(ctx, "offset", "12345");
    assert(rc == 0);
    val = channel_get_config(ctx, "offset");
    assert(val && strcmp(val, "12345") == 0);
    free(val);

    /* Upsert */
    rc = channel_set_config(ctx, "offset", "99999");
    assert(rc == 0);
    val = channel_get_config(ctx, "offset");
    assert(val && strcmp(val, "99999") == 0);
    free(val);

    channel_ctx_free(ctx);
}

static void test_channel_outbox(void) {
    cleanup();
    ChannelCtx *ctx = channel_ctx_open(TEST_DB, "telegram");
    assert(ctx);

    /* No pending rows */
    ChannelOutboxRow *row = channel_next_outbox(ctx);
    assert(row == NULL);

    /* Insert a pending row */
    sqlite3_exec(ctx->db,
        "INSERT INTO channel_outbox(channel_name, session_id, payload)"
        " VALUES('telegram', 1, '{\"text\":\"hi\"}')", NULL, NULL, NULL);
    sqlite3_exec(ctx->db,
        "INSERT INTO channel_outbox(channel_name, session_id, payload)"
        " VALUES('telegram', 2, '{\"text\":\"second\"}')", NULL, NULL, NULL);

    /* Get oldest */
    row = channel_next_outbox(ctx);
    assert(row);
    assert(row->session_id == 1);
    assert(strstr(row->payload, "hi") != NULL);
    int64_t id1 = row->id;
    channel_outbox_row_free(row);

    /* Ack it */
    int rc = channel_ack_outbox(ctx, id1);
    assert(rc == 0);

    /* Next should be second */
    row = channel_next_outbox(ctx);
    assert(row);
    assert(row->session_id == 2);
    int64_t id2 = row->id;
    channel_outbox_row_free(row);

    /* Fail it */
    rc = channel_fail_outbox(ctx, id2, "timeout");
    assert(rc == 0);

    /* No more pending */
    row = channel_next_outbox(ctx);
    assert(row == NULL);

    channel_ctx_free(ctx);
}

/* channel_retry_outbox_mode degrades deliver_mode and re-pends the row. */
static void test_channel_outbox_mode(void) {
    cleanup();
    ChannelCtx *ctx = channel_ctx_open(TEST_DB, "telegram");
    assert(ctx);

    sqlite3_exec(ctx->db,
        "INSERT INTO channel_outbox(channel_name, session_id, payload)"
        " VALUES('telegram', 1, '{\"text\":\"hi\"}')", NULL, NULL, NULL);

    ChannelOutboxRow *row = channel_next_outbox(ctx);
    assert(row);
    assert(row->deliver_mode == 0);   /* first delivery: formatted */
    int64_t id = row->id;
    channel_outbox_row_free(row);
    assert(channel_outbox_mode(ctx, id) == 0);

    /* Simulate a format rejection → re-deliver plain. */
    channel_dispatch_outbox(ctx, id);                  /* 'sending' */
    int rc = channel_retry_outbox_mode(ctx, id, 1);    /* back to pending, plain */
    assert(rc == 0);

    row = channel_next_outbox(ctx);
    assert(row);
    assert(row->id == id);
    assert(row->deliver_mode == 1);   /* re-delivery: plain */
    channel_outbox_row_free(row);
    assert(channel_outbox_mode(ctx, id) == 1);

    channel_ctx_free(ctx);
}

static void test_wake_external(void) {
    cleanup();
    /* No FIFO — should return -1 gracefully */
    int rc = wake_external(TEST_DB);
    assert(rc == -1);

    /* Create FIFO */
    mkfifo(TEST_FIFO, 0600);
    int rfd = open(TEST_FIFO, O_RDONLY | O_NONBLOCK);
    assert(rfd >= 0);

    rc = wake_external(TEST_DB);
    assert(rc == 0);

    /* Read the byte */
    char buf[4];
    ssize_t n = read(rfd, buf, sizeof(buf));
    assert(n == 1);
    assert(buf[0] == 1);

    close(rfd);
}

/* Permanent outbox failure must tell the sending session; a delivered row
 * must not. */
static int64_t count_failure_notices(sqlite3 *db, int64_t sid) {
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "SELECT count(*) FROM inbox WHERE session_id=? AND source='system'"
        " AND payload LIKE '%failed permanently%'", -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    int64_t n = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return n;
}

static void test_outbox_fail_notifies_session(void) {
    cleanup();
    ChannelCtx *ctx = channel_ctx_open(TEST_DB, "telegram");
    assert(ctx);

    assert(sqlite3_exec(ctx->db,
        "INSERT INTO sessions(id, channel_name, chat_id) VALUES(7,'telegram','555');"
        "INSERT INTO channel_outbox(id, channel_name, session_id, payload)"
        " VALUES(1,'telegram',7,'{\"chat_id\":\"555\",\"text\":\"hi\"}');"
        "INSERT INTO channel_outbox(id, channel_name, session_id, payload)"
        " VALUES(2,'telegram',7,'{\"chat_id\":\"555\",\"text\":\"ok\"}');"
        /* session_id 0 = no sender to notify */
        "INSERT INTO channel_outbox(id, channel_name, session_id, payload)"
        " VALUES(3,'telegram',0,'{\"chat_id\":\"555\",\"text\":\"orphan\"}');",
        NULL, NULL, NULL) == SQLITE_OK);

    /* Permanent failure → exactly one notice naming the outbox id + error */
    assert(channel_fail_outbox(ctx, 1, "404 Unknown Channel") == 0);
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(ctx->db,
        "SELECT payload FROM inbox WHERE session_id=7", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    const char *p = (const char *)sqlite3_column_text(s, 0);
    assert(strstr(p, "outbox id 1"));
    assert(strstr(p, "404 Unknown Channel"));
    assert(strstr(p, "telegram"));
    assert(strstr(p, "555"));
    assert(sqlite3_step(s) != SQLITE_ROW);
    sqlite3_finalize(s);

    /* Re-failing the same row must not duplicate the notice */
    channel_fail_outbox(ctx, 1, "404 Unknown Channel");
    assert(count_failure_notices(ctx->db, 7) == 1);

    /* Positive control: delivery is silent */
    assert(channel_ack_outbox(ctx, 2) == 0);
    assert(count_failure_notices(ctx->db, 7) == 1);

    /* Sessionless row: no notice, and no crash */
    assert(channel_fail_outbox(ctx, 3, "gone") == 0);
    assert(count_failure_notices(ctx->db, 0) == 0);

    channel_ctx_free(ctx);
}

int main(void) {
    TEST_INIT();
    alarm(10);

    printf("test_channel_emit...");
    test_channel_emit();
    printf(" OK\n");

    printf("test_channel_config...");
    test_channel_config();
    printf(" OK\n");

    printf("test_channel_outbox...");
    test_channel_outbox();
    printf(" OK\n");

    printf("test_channel_outbox_mode...");
    test_channel_outbox_mode();
    printf(" OK\n");

    printf("test_outbox_fail_notifies_session...");
    test_outbox_fail_notifies_session();
    printf(" OK\n");

    printf("test_wake_external...");
    test_wake_external();
    printf(" OK\n");

    cleanup();
    printf("all channel_api tests passed\n");
    return 0;
}
