#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "channel_api.h"
#include "db.h"

#define DB_PATH "/tmp/test_outbox_wake.db"

static void test_fifo_path(void) {
    char *p = channel_outbox_fifo_path("/home/user/cclaw.db", "telegram");
    assert(p);
    assert(strcmp(p, "/home/user/cclaw.telegram.pipe") == 0);
    free(p);

    p = channel_outbox_fifo_path("/tmp/test.db", "discord");
    assert(p);
    assert(strcmp(p, "/tmp/test.discord.pipe") == 0);
    free(p);

    /* No .db suffix */
    p = channel_outbox_fifo_path("/tmp/mydb", "chan");
    assert(p);
    assert(strcmp(p, "/tmp/mydb.chan.pipe") == 0);
    free(p);

    printf("  PASS: test_fifo_path\n");
}

static void test_fifo_wake(void) {
    /* Open read end */
    int fd = channel_outbox_fifo_open(DB_PATH, "testchan");
    assert(fd >= 0);

    /* Wake it */
    assert(channel_outbox_wake(DB_PATH, "testchan") == 0);

    /* Should be readable */
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int r = poll(&pfd, 1, 100);
    assert(r == 1);

    char buf[4];
    ssize_t n = read(fd, buf, sizeof(buf));
    assert(n == 1);

    channel_outbox_fifo_close(fd, DB_PATH, "testchan");
    printf("  PASS: test_fifo_wake\n");
}

static void test_outbox_insert(void) {
    unlink(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);

    {   sqlite3_stmt *s;
        assert(sqlite3_prepare_v2(db, "INSERT INTO channel_outbox(channel_name,session_id,payload) VALUES(?,?,?)", -1, &s, NULL) == SQLITE_OK);
        sqlite3_bind_text(s, 1, "telegram", -1, SQLITE_STATIC);
        sqlite3_bind_int64(s, 2, 42);
        sqlite3_bind_text(s, 3, "{\"chat_id\":\"123\",\"text\":\"hello\"}", -1, SQLITE_STATIC);
        assert(sqlite3_step(s) == SQLITE_DONE);
        sqlite3_finalize(s);
    }

    /* Verify via channel_next_outbox */
    ChannelCtx *ctx = channel_ctx_open(DB_PATH, "telegram");
    assert(ctx);
    ChannelOutboxRow *row = channel_next_outbox(ctx);
    assert(row);
    assert(row->session_id == 42);
    assert(strstr(row->payload, "hello") != NULL);
    channel_outbox_row_free(row);
    channel_ctx_free(ctx);

    db_close(db);
    unlink(DB_PATH);
    printf("  PASS: test_outbox_insert\n");
}

int main(void) {
    printf("test_outbox_wake:\n");
    test_fifo_path();
    test_fifo_wake();
    test_outbox_insert();
    printf("all outbox_wake tests passed\n");
    return 0;
}
