/* Smoke: the CLI agent picker lists agents, sorted. */
#include "db.h"
#include "test_util.h"
#include <assert.h>

static void test_agent_list(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int count = 0;
    assert(db_agent_list(db, &count) == NULL && count == 0);

    db_agent_upsert(db, "zebra", NULL, NULL);
    db_agent_upsert(db, "alpha", NULL, NULL);
    char **agents = db_agent_list(db, &count);
    assert(agents && count == 2);
    assert(strcmp(agents[0], "alpha") == 0);
    assert(strcmp(agents[1], "zebra") == 0);
    for (int i = 0; i < count; i++) free(agents[i]);
    free(agents);
    db_close(db);
}

int main(void) {
    TEST_INIT();
    RUN(test_agent_list);
    return 0;
}
