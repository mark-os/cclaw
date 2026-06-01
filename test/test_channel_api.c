#include "channel_api.h"
#include "daemon.h"
#include "db.h"
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
    unlink(TEST_DB);
    unlink(TEST_DB "-wal");
    unlink(TEST_DB "-shm");
    unlink(TEST_FIFO);
}

static void test_channel_emit(void) {
    cleanup();
    /* Create FIFO so daemon_wake succeeds */
    mkfifo(TEST_FIFO, 0600);
    /* Open read end non-blocking (so write doesn't block) */
    int rfd = open(TEST_FIFO, O_RDONLY | O_NONBLOCK);
    assert(rfd >= 0);

    ChannelCtx *ctx = channel_ctx_open(TEST_DB, "telegram");
    assert(ctx);

    int rc = channel_emit(ctx, "message", "{\"chat_id\":123,\"text\":\"hello\"}");
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

static void test_daemon_wake(void) {
    cleanup();
    /* No FIFO — should return -1 gracefully */
    int rc = daemon_wake(TEST_DB);
    assert(rc == -1);

    /* Create FIFO */
    mkfifo(TEST_FIFO, 0600);
    int rfd = open(TEST_FIFO, O_RDONLY | O_NONBLOCK);
    assert(rfd >= 0);

    rc = daemon_wake(TEST_DB);
    assert(rc == 0);

    /* Read the byte */
    char buf[4];
    ssize_t n = read(rfd, buf, sizeof(buf));
    assert(n == 1);
    assert(buf[0] == 1);

    close(rfd);
}

int main(void) {
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

    printf("test_daemon_wake...");
    test_daemon_wake();
    printf(" OK\n");

    cleanup();
    printf("all channel_api tests passed\n");
    return 0;
}
