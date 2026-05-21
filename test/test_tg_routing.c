#include "db.h"
#include <assert.h>
#include <stdio.h>
#include <unistd.h>

#define TEST_DB "test_tg_routing.db"

static void test_get_unmapped(void) {
    sqlite3 *db = db_open(TEST_DB);
    assert(db);
    assert(db_tg_get_session(db, 12345) == -1);
    db_close(db);
}

static void test_set_and_get(void) {
    sqlite3 *db = db_open(TEST_DB);
    assert(db);
    int64_t sid = session_create(db, "tg_99");
    assert(sid > 0);
    assert(db_tg_set_session(db, 99, sid) == 0);
    assert(db_tg_get_session(db, 99) == sid);
    db_close(db);
}

static void test_overwrite(void) {
    sqlite3 *db = db_open(TEST_DB);
    assert(db);
    int64_t s1 = session_create(db, "tg_a");
    int64_t s2 = session_create(db, "tg_b");
    assert(s1 > 0 && s2 > 0);
    assert(db_tg_set_session(db, 200, s1) == 0);
    assert(db_tg_get_session(db, 200) == s1);
    assert(db_tg_set_session(db, 200, s2) == 0);
    assert(db_tg_get_session(db, 200) == s2);
    db_close(db);
}

static void test_multiple_chats(void) {
    sqlite3 *db = db_open(TEST_DB);
    assert(db);
    int64_t s1 = session_create(db, "tg_c1");
    int64_t s2 = session_create(db, "tg_c2");
    assert(db_tg_set_session(db, 1001, s1) == 0);
    assert(db_tg_set_session(db, 1002, s2) == 0);
    assert(db_tg_get_session(db, 1001) == s1);
    assert(db_tg_get_session(db, 1002) == s2);
    db_close(db);
}

int main(void) {
    unlink(TEST_DB);

    printf("test_get_unmapped...");
    test_get_unmapped();
    printf(" OK\n");

    unlink(TEST_DB);
    printf("test_set_and_get...");
    test_set_and_get();
    printf(" OK\n");

    unlink(TEST_DB);
    printf("test_overwrite...");
    test_overwrite();
    printf(" OK\n");

    unlink(TEST_DB);
    printf("test_multiple_chats...");
    test_multiple_chats();
    printf(" OK\n");

    unlink(TEST_DB);
    printf("all tg_routing tests passed\n");
    return 0;
}
