#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "channel_api.h"
#include "test_util.h"

#define DB_PATH "/tmp/test_outbox_wake.db"

/* The FIFO sits beside the DB, named for the channel; ".db" is dropped. */
static void test_fifo_path(void) {
    static const struct { const char *db, *chan, *want; } cases[] = {
        { "/home/user/cclaw.db", "telegram", "/home/user/cclaw.telegram.pipe" },
        { "/tmp/test.db",        "discord",  "/tmp/test.discord.pipe" },
        { "/tmp/mydb",           "chan",     "/tmp/mydb.chan.pipe" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *p = channel_outbox_fifo_path(cases[i].db, cases[i].chan);
        if (!p || strcmp(p, cases[i].want) != 0) {
            fprintf(stderr, "row %zu: got %s\n", i, p ? p : "(null)");
            assert(0);
        }
        free(p);
    }
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
}

int main(void) {
    TEST_INIT();
    printf("test_outbox_wake:\n");
    RUN(test_fifo_path);
    RUN(test_fifo_wake);
    return 0;
}
