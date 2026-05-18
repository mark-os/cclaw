#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "../vendor/cJSON/cJSON.h"
#include "../vendor/sqlite3/sqlite3.h"

int main(void) {
    /* Test cJSON: parse a simple object */
    const char *json = "{\"key\":\"value\"}";
    cJSON *root = cJSON_Parse(json);
    assert(root != NULL);
    cJSON *key = cJSON_GetObjectItemCaseSensitive(root, "key");
    assert(cJSON_IsString(key));
    assert(strcmp(key->valuestring, "value") == 0);
    cJSON_Delete(root);
    printf("PASS: cJSON\n");

    /* Test sqlite3: open in-memory database */
    sqlite3 *db = NULL;
    int rc = sqlite3_open(":memory:", &db);
    assert(rc == SQLITE_OK);
    assert(db != NULL);
    sqlite3_close(db);
    printf("PASS: sqlite3\n");

    printf("All vendor tests passed.\n");
    return 0;
}
