#include "db.h"
#include "test_util.h"
#include "context.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *DB_PATH = "/tmp/test_cclaw_recall.db";

/* agent_name is an enforced FK (v31): seed the parent row before sessions. */
static sqlite3 *open_seeded(const char *path) {
    sqlite3 *db = test_db_open(path);
    if (db) test_seed_agent(db, "default");
    return db;
}

static void add_user_msg(sqlite3 *db, int64_t sid, const char *text) {
    Message m = {.role = ROLE_USER, .content = (char *)text};
    entry_append_with_turn(db, sid, &m, 1);
}

/* Porter stemming: "configure" must match "configured" from another session */
static void test_stemming_match(void) {
    test_db_clean(DB_PATH);
    sqlite3 *db = open_seeded(DB_PATH);
    assert(db);

    int64_t s1 = session_create(db, "a", "default", -1, 0);
    int64_t s2 = session_create(db, "b", "default", -1, 0);
    add_user_msg(db, s1, "yesterday we configured the telegram webhook");

    char *r = context_auto_recall(db, s2, "please configure telegram", 500);
    assert(r);
    assert(strstr(r, "---Possibly relevant context---"));
    assert(strstr(r, "configured the telegram webhook"));
    char tag[64];
    snprintf(tag, sizeof(tag), "[session %lld, user]", (long long)s1);
    assert(strstr(r, tag));
    assert(strstr(r, "---End of context---"));
    free(r);

    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_stemming_match\n");
}

/* Dynamic stopwords: words in >25%% of rows are dropped; recall goes silent
 * when every keyword is corpus-common. */
static void test_dynamic_stopwords_silence(void) {
    test_db_clean(DB_PATH);
    sqlite3 *db = open_seeded(DB_PATH);
    assert(db);

    int64_t s1 = session_create(db, "a", "default", -1, 0);
    int64_t s2 = session_create(db, "b", "default", -1, 0);
    /* 12 of 14 rows contain "evening"/"good" — way over the 25% threshold */
    for (int i = 0; i < 12; i++)
        add_user_msg(db, s1, "good evening friend");
    add_user_msg(db, s1, "telegram webhook setup");
    add_user_msg(db, s1, "telegram token rotated");

    char *r = context_auto_recall(db, s2, "good evening", 500);
    assert(r == NULL);  /* all keywords corpus-common → silent */

    /* Rare keywords still recall */
    r = context_auto_recall(db, s2, "good evening, what about the telegram webhook?", 500);
    assert(r);
    assert(strstr(r, "telegram webhook setup"));
    /* ...and the greeting rows must not have matched */
    assert(!strstr(r, "good evening friend"));
    free(r);

    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_dynamic_stopwords_silence\n");
}

/* No hits at all → NULL, never an empty wrapper */
static void test_no_hits_silent(void) {
    test_db_clean(DB_PATH);
    sqlite3 *db = open_seeded(DB_PATH);
    assert(db);

    int64_t s1 = session_create(db, "a", "default", -1, 0);
    int64_t s2 = session_create(db, "b", "default", -1, 0);
    add_user_msg(db, s1, "telegram webhook setup");

    char *r = context_auto_recall(db, s2, "zebra quantum xylophone", 500);
    assert(r == NULL);

    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_no_hits_silent\n");
}

/* Current session is excluded from results */
static void test_excludes_own_session(void) {
    test_db_clean(DB_PATH);
    sqlite3 *db = open_seeded(DB_PATH);
    assert(db);

    int64_t s1 = session_create(db, "a", "default", -1, 0);
    add_user_msg(db, s1, "telegram webhook setup");

    char *r = context_auto_recall(db, s1, "telegram webhook", 500);
    assert(r == NULL);

    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_excludes_own_session\n");
}

int main(void) {
    TEST_INIT();
    printf("test_auto_recall:\n");
    test_stemming_match();
    test_dynamic_stopwords_silence();
    test_no_hits_silent();
    test_excludes_own_session();
    printf("All auto-recall tests passed.\n");
    return 0;
}
