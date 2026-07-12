/* Unit tests for agent_definition_apply / validate (specs/self-configuration.md) */
#include "agent_config.h"
#include "agent_define.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sqlite3 *g_db;

static void seed_creator(const char *name, const char *profile) {
    db_agent_upsert(g_db, name, NULL, NULL);
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(g_db,
        "UPDATE agents SET sandbox_profile=?2 WHERE name=?1", -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, profile, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
}

static void test_operator_apply(void) {
    char *err = NULL;
    int rc = agent_definition_apply(g_db,
        "{\"name\":\"Scout\",\"description\":\"scouts\","
        "\"system_prompt\":\"be scouty\",\"sandbox_profile\":\"restricted\","
        "\"grants\":{\"tools\":[\"web_fetch\"],\"hosts\":[\".example.com\"]},"
        "\"max_iterations\":7,"
        "\"memory_blocks\":[{\"label\":\"notes\",\"description\":\"d\",\"value\":\"v\"}]}",
        NULL, NULL, &err);
    if (rc != 0) fprintf(stderr, "err: %s\n", err ? err : "?");
    assert(rc == 0);

    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(g_db,
        "SELECT sandbox_profile, max_iterations, description FROM agents WHERE name='Scout'",
        -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "restricted") == 0);
    assert(sqlite3_column_int(s, 1) == 7);
    assert(strcmp((const char *)sqlite3_column_text(s, 2), "scouts") == 0);
    sqlite3_finalize(s);

    /* Declared grants on top of the baseline defaults. */
    assert(grants_contains(g_db, "Scout", "tool", "web_fetch"));
    assert(grants_contains(g_db, "Scout", "host", ".example.com"));
    assert(grants_contains(g_db, "Scout", "tool", "file_read")); /* baseline */

    /* Memory block seeded. */
    MemoryBlock *mb = memory_block_get(g_db, "Scout", "notes");
    assert(mb != NULL);
    memory_block_free(mb);

    /* Duplicate name refused. */
    char *err2 = NULL;
    assert(agent_definition_apply(g_db, "{\"name\":\"Scout\"}", NULL, NULL, &err2) != 0);
    assert(err2 && strstr(err2, "already exists"));
    free(err2);
    printf("  PASS test_operator_apply\n");
}

static void test_profile_escalation_refused(void) {
    seed_creator("Std", "standard");
    char *err = NULL;
    int rc = agent_definition_validate(g_db,
        "{\"name\":\"Escaper\",\"sandbox_profile\":\"trusted\"}", "Std", &err);
    assert(rc != 0);
    assert(err && strstr(err, "looser"));
    free(err);

    /* Equal or tighter is fine. */
    assert(agent_definition_validate(g_db,
        "{\"name\":\"Tight\",\"sandbox_profile\":\"restricted\"}", "Std", NULL) == 0);
    printf("  PASS test_profile_escalation_refused\n");
}

static void test_grants_superset_refused(void) {
    seed_creator("Parent", "standard");
    agent_config_grant(g_db, "Parent", "tool", "web_fetch", 0);
    agent_config_grant(g_db, "Parent", "host", "ok.example.com", 0);

    char *err = NULL;
    int rc = agent_definition_validate(g_db,
        "{\"name\":\"Greedy\",\"grants\":{\"tools\":[\"shell_exec\"]}}", "Parent", &err);
    assert(rc != 0);
    assert(err && strstr(err, "exceeds creator"));
    free(err);

    err = NULL;
    rc = agent_definition_validate(g_db,
        "{\"name\":\"Greedy\",\"grants\":{\"hosts\":[\"evil.example.com\"]}}", "Parent", &err);
    assert(rc != 0);
    free(err);

    /* Subset passes and applies. */
    err = NULL;
    rc = agent_definition_apply(g_db,
        "{\"name\":\"Child\",\"grants\":{\"tools\":[\"web_fetch\"],"
        "\"hosts\":[\"ok.example.com\"]}}", "Parent", NULL, &err);
    if (rc != 0) fprintf(stderr, "err: %s\n", err ? err : "?");
    assert(rc == 0);
    assert(grants_contains(g_db, "Child", "host", "ok.example.com"));
    printf("  PASS test_grants_superset_refused\n");
}

static void test_extension_gate(void) {
    seed_creator("Extender", "standard");
    sqlite3_exec(g_db,
        "INSERT INTO extensions(name, path, owner_agent, published, enabled)"
        " VALUES('privext','/x','Someone',0,1),"
        "       ('pubext','/y','Someone',1,1),"
        "       ('ownext','/z','Extender',0,1)", NULL, NULL, NULL);

    char *err = NULL;
    assert(agent_definition_validate(g_db,
        "{\"name\":\"ExtKid\",\"extensions\":[\"privext\"]}", "Extender", &err) != 0);
    free(err);
    assert(agent_definition_validate(g_db,
        "{\"name\":\"ExtKid\",\"extensions\":[\"missing\"]}", "Extender", NULL) != 0);
    assert(agent_definition_validate(g_db,
        "{\"name\":\"ExtKid\",\"extensions\":[\"pubext\",\"ownext\"]}", "Extender", NULL) == 0);

    err = NULL;
    int rc = agent_definition_apply(g_db,
        "{\"name\":\"ExtKid\",\"extensions\":[\"pubext\"]}", "Extender", NULL, &err);
    if (rc != 0) fprintf(stderr, "err: %s\n", err ? err : "?");
    assert(rc == 0);
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(g_db,
        "SELECT 1 FROM agent_extensions WHERE agent_name='ExtKid'"
        " AND extension_name='pubext' AND enabled=1", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    sqlite3_finalize(s);
    printf("  PASS test_extension_gate\n");
}

static void test_clone_from(void) {
    seed_creator("Template", "restricted");
    agent_config_grant(g_db, "Template", "tool", "web_fetch", 0);
    sqlite3_exec(g_db,
        "UPDATE agents SET description='tmpl', system_prompt='sp',"
        " max_iterations=11 WHERE name='Template'", NULL, NULL, NULL);

    char *err = NULL;
    int rc = agent_definition_apply(g_db,
        "{\"name\":\"Copy\",\"clone_from\":\"Template\",\"description\":\"copied\"}",
        NULL, NULL, &err);
    if (rc != 0) fprintf(stderr, "err: %s\n", err ? err : "?");
    assert(rc == 0);

    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(g_db,
        "SELECT description, system_prompt, max_iterations, sandbox_profile"
        " FROM agents WHERE name='Copy'", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "copied") == 0);   /* overlay */
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "sp") == 0);       /* cloned */
    assert(sqlite3_column_int(s, 2) == 11);                                    /* cloned */
    assert(strcmp((const char *)sqlite3_column_text(s, 3), "restricted") == 0);
    sqlite3_finalize(s);
    assert(grants_contains(g_db, "Copy", "tool", "web_fetch"));

    /* Clone laundering refused: creator without the source's grants. */
    seed_creator("Poor", "standard");
    char *err2 = NULL;
    assert(agent_definition_validate(g_db,
        "{\"name\":\"Laundered\",\"clone_from\":\"Template\"}", "Poor", &err2) != 0);
    assert(err2 && strstr(err2, "exceeds creator"));
    free(err2);

    /* Missing clone source refused. */
    assert(agent_definition_validate(g_db,
        "{\"name\":\"NoSrc\",\"clone_from\":\"Ghost\"}", NULL, NULL) != 0);
    printf("  PASS test_clone_from\n");
}

static void test_bad_input(void) {
    assert(agent_definition_validate(g_db, "not json", NULL, NULL) != 0);
    assert(agent_definition_validate(g_db, "{}", NULL, NULL) != 0);
    assert(agent_definition_validate(g_db, "{\"name\":\"bad name\"}", NULL, NULL) != 0);
    assert(agent_definition_validate(g_db,
        "{\"name\":\"X\",\"sandbox_profile\":\"ultra\"}", NULL, NULL) != 0);
    printf("  PASS test_bad_input\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_agent_define:\n");
    g_db = test_db_open(":memory:");
    assert(g_db);
    test_operator_apply();
    test_profile_escalation_refused();
    test_grants_superset_refused();
    test_extension_gate();
    test_clone_from();
    test_bad_input();
    db_close(g_db);
    printf("\nAll agent_define tests passed.\n");
    return 0;
}
