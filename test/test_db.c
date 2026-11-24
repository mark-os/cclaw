#define _GNU_SOURCE
#include "cclaw.h"
#include "agent_config.h"
#include "db.h"
#include "config_registry.h"
#include "change_log.h"
#include "util.h"
#include "test_util.h"
#include "advance.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include <unistd.h>

static void test_config_registry(void) {
    const char *path = "/tmp/test_cclaw_registry.sqlite";
    test_db_clean(path);

    sqlite3 *db = test_db_open(path);
    assert(db != NULL);

    /* Unset registered key returns its default */
    assert(config_get_int(db, "web_port") == 8080);

    /* Set override and read back */
    assert(config_set(db, "web_port", "9090") == 0);
    assert(config_get_int(db, "web_port") == 9090);

    /* Clear override (NULL) resets to default */
    assert(config_set(db, "web_port", NULL) == 0);
    assert(config_get_int(db, "web_port") == 8080);

    /* Unregistered key rejected */
    assert(config_set(db, "tg_offset", "1") == -1);

    db_close(db);
    test_db_clean(path);
}

static void test_prune_inbox(void) {
    const char *path = "/tmp/test_cclaw_prune_inbox.sqlite";
    test_db_clean(path);
    sqlite3 *db = test_db_open(path);
    assert(db != NULL);

    /* default inbox_retention_sec = 604800 (7 days). "old" is well beyond it. */
    assert(sqlite3_exec(db,
        "INSERT INTO inbox(session_id,source,payload,consumed,created_at) VALUES"
        " (1,'cli','a',1,unixepoch()-800000),"   /* consumed + old   → pruned */
        " (1,'cli','b',1,unixepoch()),"          /* consumed + fresh → kept   */
        " (1,'cli','c',0,unixepoch()-800000);",  /* old but unconsumed → kept */
        NULL, NULL, NULL) == SQLITE_OK);
    assert(test_qi(db, "SELECT count(*) FROM inbox") == 3);

    db_prune_inbox(db);
    assert(test_qi(db, "SELECT count(*) FROM inbox") == 2);

    db_close(db);
    test_db_clean(path);
}

static void test_prune_outbox(void) {
    const char *path = "/tmp/test_cclaw_prune_outbox.sqlite";
    test_db_clean(path);
    sqlite3 *db = test_db_open(path);
    assert(db != NULL);

    /* terminal = 'delivered' or 'failed%'; aged from COALESCE(acked_at,created_at). */
    assert(sqlite3_exec(db,
        "INSERT INTO channel_outbox(channel_name,session_id,payload,status,created_at,acked_at) VALUES"
        " ('t',1,'a','delivered',unixepoch()-800000,unixepoch()-800000)," /* terminal+old → pruned */
        " ('t',1,'b','failed: 400',unixepoch()-800000,NULL),"             /* failed%+old  → pruned */
        " ('t',1,'c','pending',unixepoch()-800000,NULL),"                 /* not terminal → kept   */
        " ('t',1,'d','delivered',unixepoch(),unixepoch());",              /* terminal+fresh→ kept  */
        NULL, NULL, NULL) == SQLITE_OK);
    assert(test_qi(db, "SELECT count(*) FROM channel_outbox") == 4);

    db_prune_outbox(db);
    assert(test_qi(db, "SELECT count(*) FROM channel_outbox") == 2);

    db_close(db);
    test_db_clean(path);
}

static void test_free_mb(void) {
    const char *path = "/tmp/test_cclaw_free_mb.sqlite";
    test_db_clean(path);
    sqlite3 *db = test_db_open(path);
    assert(db != NULL);

    /* On-disk DB on /tmp: measurable and positive. */
    assert(db_free_mb(db) > 0);

    db_close(db);
    test_db_clean(path);
}

static void set_user_version(sqlite3 *db, int v) {
    char sql[48];
    snprintf(sql, sizeof(sql), "PRAGMA user_version=%d;", v);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
}

/* db_seed_defaults runs on every start, not only on an empty providers table.
 * It used to early-return whenever any provider row existed, so a DB created
 * before a catalog entry was added never saw it — the reason DBs predating the
 * curated provider catalog were missing five providers with no way to get them
 * short of deleting the DB. Seed data is row-shaped, so this is idempotent
 * INSERT OR IGNORE, not a schema patch. */
static void test_seed_backfills_and_preserves(void) {
    const char *path = "/tmp/test_cclaw_seed_backfill.sqlite";
    test_db_clean(path);
    sqlite3 *db = test_db_open(path);
    assert(db != NULL);
    assert(db_seed_defaults(db) == 0);

    /* Simulate a DB that predates part of the catalog. */
    assert(sqlite3_exec(db, "DELETE FROM models WHERE provider_name='cerebras';",
                        NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_exec(db, "DELETE FROM providers WHERE name='cerebras';",
                        NULL, NULL, NULL) == SQLITE_OK);
    /* ...and an operator edit that must survive re-seeding. */
    assert(sqlite3_exec(db, "UPDATE providers SET default_model='pinned-by-operator'"
                            " WHERE name='openrouter';", NULL, NULL, NULL) == SQLITE_OK);

    assert(db_seed_defaults(db) == 0);   /* second run — no longer a no-op */

    assert(test_qi(db, "SELECT COUNT(*) FROM providers WHERE name='cerebras'") == 1);

    /* Every catalog provider is routable — a providers row with no models row
     * could never be selected by chat routing. */
    assert(test_qi(db, "SELECT COUNT(*) FROM providers p"
                       " WHERE NOT EXISTS (SELECT 1 FROM models m WHERE m.provider_name=p.name)")
           == 0);

    /* INSERT OR IGNORE: the operator's edit won, not the seed's default. */
    char *dm = test_qs(db, "SELECT default_model FROM providers WHERE name='openrouter'");
    assert(dm && strcmp(dm, "pinned-by-operator") == 0);
    free(dm);

    db_close(db);
    test_db_clean(path);
}

static void test_schema_state(void) {
    const char *path = "/tmp/test_cclaw_schema_state.sqlite";
    test_db_clean(path);

    sqlite3 *db = db_open(path);   /* no schema applied */
    assert(db != NULL);
    int uv = -1;
    assert(db_schema_state(db, &uv) == DB_SCHEMA_FRESH && uv == 0);
    assert(db_schema_compat(db) == 1);   /* fresh is usable */

    assert(db_ensure_schema(db) == 0);
    assert(db_schema_state(db, &uv) == DB_SCHEMA_CURRENT);
    assert(uv == CCLAW_SCHEMA_VERSION);

    set_user_version(db, CCLAW_SCHEMA_VERSION + 1);
    assert(db_schema_state(db, NULL) == DB_SCHEMA_FUTURE);
    assert(db_schema_compat(db) == 0);   /* refused, not "upgraded" */

    set_user_version(db, 10);            /* far below the floor */
    assert(db_schema_state(db, NULL) == DB_SCHEMA_TOO_OLD);
    assert(db_schema_compat(db) == 0);

    set_user_version(db, CCLAW_SCHEMA_MIN - 1);   /* one below — refused */
    assert(db_schema_state(db, NULL) == DB_SCHEMA_TOO_OLD);
    assert(db_schema_compat(db) == 0);

    /* At the floor the patch list has nothing to run: compat succeeds and
     * leaves the version where it was. */
    set_user_version(db, CCLAW_SCHEMA_MIN);
    assert(db_schema_compat(db) == 1);
    assert(db_schema_state(db, &uv) == DB_SCHEMA_CURRENT && uv == CCLAW_SCHEMA_MIN);

    set_user_version(db, 0);             /* v0 but tables exist → pre-freeze */
    assert(db_schema_state(db, NULL) == DB_SCHEMA_TOO_OLD);
    assert(db_schema_compat(db) == 0);

    db_close(db);
    test_db_clean(path);
}

static void test_rate_limit_and_cost(void) {
    const char *path = "/tmp/test_cclaw_db_budget.sqlite";
    test_db_clean(path);
    sqlite3 *db = test_db_open(path);
    assert(db != NULL);

    /* Recent entry inside both windows: 600 tokens, $1.50 */
    assert(sqlite3_exec(db,
        "INSERT INTO entries(session_id, role, content, usage_in, usage_out,"
        " cost_nano, created_at) VALUES"
        " (1, 2, 'recent', 400, 200, 1500000000, unixepoch()-60);",
        NULL, NULL, NULL) == SQLITE_OK);
    /* Old entry outside both windows (25h > 24h cost, > 1h token window) */
    assert(sqlite3_exec(db,
        "INSERT INTO entries(session_id, role, content, usage_in, usage_out,"
        " cost_nano, created_at) VALUES"
        " (1, 2, 'old', 5000, 5000, 9000000000, unixepoch()-90000);",
        NULL, NULL, NULL) == SQLITE_OK);

    /* Global limit enforced (was dead code: NULL provider always passed) */
    assert(rate_limit_check(db, 500) == 0);  /* 600 used >= 500 */
    assert(rate_limit_check(db, 1000) == 1); /* 600 used < 1000 */
    assert(rate_limit_check(db, 0) == 1);    /* 0 = unlimited */

    /* Cost sum covers only the rolling 24h window */
    assert(db_cost_last_24h(db) == 1500000000LL);

    db_close(db);
    test_db_clean(path);
}

/* cache_lifetime on an Anthropic entry: 300 or 3600 only, on insert and on
 * update (also when the model under it changes); a new Anthropic chat entry
 * that says nothing — omitted or NULL — gets 3600; a utility entry and other
 * providers keep what they are given. The change log records it. */
static void test_agent_models_cache_lifetime(void) {
    const char *path = "/tmp/test_cclaw_db_lifetime.sqlite";
    test_db_clean(path);
    sqlite3 *db = test_db_open(path);
    assert(db != NULL);
    assert(sqlite3_exec(db,
        "INSERT INTO agents(name) VALUES('A');"
        "INSERT INTO providers(name, base_url, endpoint_type) VALUES"
        " ('p','http://p','openai'), ('a','http://a','anthropic');"
        "INSERT INTO models(id, provider_name, model) VALUES"
        " ('m@p','p','m'), ('c@a','a','c'), ('d@a','a','d');",
        NULL, NULL, NULL) == SQLITE_OK);
#define LT(sql) sqlite3_exec(db, sql, NULL, NULL, NULL)
    assert(LT("INSERT INTO agent_models(agent_name, model_id, pos) VALUES('A','c@a',0);")
           == SQLITE_OK);
    assert(db_scalar_i64(db, "SELECT cache_lifetime FROM agent_models WHERE pos=0"
                             " AND purpose='chat'", 0, -1) == 3600);
    assert(LT("INSERT INTO agent_models(agent_name, model_id, pos, cache_lifetime)"
              " VALUES('A','d@a',1,NULL);") == SQLITE_OK);
    assert(db_scalar_i64(db, "SELECT cache_lifetime FROM agent_models WHERE pos=1", 0, -1)
           == 3600);
    assert(LT("INSERT INTO agent_models(agent_name, model_id, pos, cache_lifetime)"
              " VALUES('A','c@a',2,300);") == SQLITE_OK);
    assert(db_scalar_i64(db, "SELECT cache_lifetime FROM agent_models WHERE pos=2", 0, -1)
           == 300);
    assert(LT("INSERT INTO agent_models(agent_name, model_id, pos, cache_lifetime)"
              " VALUES('A','c@a',3,600);") != SQLITE_OK);
    assert(strstr(sqlite3_errmsg(db), "cache_lifetime on an Anthropic model is 300 or 3600"));
    assert(LT("INSERT INTO agent_models(agent_name, model_id, pos, cache_lifetime)"
              " VALUES('A','m@p',3,600);") == SQLITE_OK);
    assert(LT("INSERT INTO agent_models(agent_name, model_id, pos) VALUES('A','m@p',4);")
           == SQLITE_OK);
    assert(db_scalar_i64(db, "SELECT cache_lifetime IS NULL FROM agent_models WHERE pos=4",
                         0, -1) == 1);
    assert(LT("INSERT INTO agent_models(agent_name, purpose, model_id, pos)"
              " VALUES('A','utility','c@a',0);") == SQLITE_OK);
    assert(db_scalar_i64(db, "SELECT cache_lifetime IS NULL FROM agent_models"
                             " WHERE purpose='utility'", 0, -1) == 1);
    assert(LT("INSERT INTO agent_models(agent_name, model_id, pos, cache_lifetime)"
              " VALUES('A','m@p',5,0);") != SQLITE_OK);   /* CHECK: above 0 */
    /* Updates: the value, and the model under a value. */
    assert(LT("UPDATE agent_models SET cache_lifetime=1800 WHERE pos=0 AND purpose='chat';")
           != SQLITE_OK);
    assert(LT("UPDATE agent_models SET cache_lifetime=300 WHERE pos=0 AND purpose='chat';")
           == SQLITE_OK);
    assert(LT("UPDATE agent_models SET model_id='c@a' WHERE pos=3;") != SQLITE_OK);
    assert(LT("UPDATE agent_models SET cache_lifetime=NULL WHERE pos=0 AND purpose='chat';")
           == SQLITE_OK);   /* unknown: the plain marker, no CacheExpiring */
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM change_log WHERE table_name='agent_models'"
                             " AND new_values ->> 'cache_lifetime' = 300", 0, -1) >= 1);
#undef LT
    db_close(db);
    test_db_clean(path);
}

/* reasoning_display: 'summary' unless said otherwise, 'none' allowed,
 * nothing else — NULL included — and the change log records it. */
static void test_agent_models_reasoning_display(void) {
    const char *path = "/tmp/test_cclaw_db_display.sqlite";
    test_db_clean(path);
    sqlite3 *db = test_db_open(path);
    assert(db != NULL);
    assert(sqlite3_exec(db,
        "INSERT INTO agents(name) VALUES('A');"
        "INSERT INTO providers(name, base_url) VALUES ('p','http://p');"
        "INSERT INTO models(id, provider_name, model) VALUES ('m@p','p','m');",
        NULL, NULL, NULL) == SQLITE_OK);
#define RD(sql) sqlite3_exec(db, sql, NULL, NULL, NULL)
    assert(RD("INSERT INTO agent_models(agent_name, model_id, pos) VALUES('A','m@p',0);")
           == SQLITE_OK);
    assert(db_scalar_i64(db, "SELECT reasoning_display='summary' FROM agent_models"
                             " WHERE pos=0", 0, -1) == 1);
    assert(RD("INSERT INTO agent_models(agent_name, model_id, pos, reasoning_display)"
              " VALUES('A','m@p',1,'none');") == SQLITE_OK);
    assert(RD("INSERT INTO agent_models(agent_name, model_id, pos, reasoning_display)"
              " VALUES('A','m@p',2,'full');") != SQLITE_OK);
    assert(RD("INSERT INTO agent_models(agent_name, model_id, pos, reasoning_display)"
              " VALUES('A','m@p',2,NULL);") != SQLITE_OK);
    assert(RD("UPDATE agent_models SET reasoning_display='Summary' WHERE pos=0;") != SQLITE_OK);
    assert(RD("UPDATE agent_models SET reasoning_display='none' WHERE pos=0;") == SQLITE_OK);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM change_log WHERE table_name='agent_models'"
                             " AND new_values ->> 'reasoning_display' = 'none'", 0, -1) >= 1);
#undef RD
    db_close(db);
    test_db_clean(path);
}

static void test_agent_models_capability_gate(void) {
    const char *path = "/tmp/test_cclaw_db_capgate.sqlite";
    test_db_clean(path);
    sqlite3 *db = test_db_open(path);
    assert(db != NULL);
    assert(sqlite3_exec(db,
        "INSERT INTO agents(name) VALUES('A');"
        "INSERT INTO providers(name, base_url) VALUES('p','http://p');"
        "INSERT INTO models(id, provider_name, model, capabilities) VALUES"
        " ('plain@p','p','plain','[]'), ('eyes@p','p','eyes','[\"image\",\"chat\"]'),"
        " ('ears@p','p','ears','[\"audio\"]'), ('junk@p','p','junk','not json'),"
        " ('none@p','p','none',NULL);", NULL, NULL, NULL) == SQLITE_OK);
    const char *ins = "INSERT INTO agent_models(agent_name, purpose, model_id, pos)"
                      " VALUES('A', ?1, ?2, ?3)";
    struct { const char *purpose, *model; int pos, ok; } cases[] = {
        { "chat",  "plain@p", 0, 1 },   /* chat never checks capabilities */
        { "image", "plain@p", 0, 0 },
        { "image", "junk@p",  0, 0 },   /* malformed JSON is no capability */
        { "image", "none@p",  0, 0 },
        { "image", "ears@p",  0, 0 },
        { "image", "eyes@p",  0, 1 },
        { "audio", "eyes@p",  0, 0 },
        { "audio", "ears@p",  0, 1 },
        { "video", "eyes@p",  1, 0 },   /* not a purpose (CHECK) */
        { "utility", "plain@p", 0, 1 }, /* utility never checks capabilities */
        { "utility", "junk@p",  1, 1 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        sqlite3_stmt *st;
        assert(sqlite3_prepare_v2(db, ins, -1, &st, NULL) == SQLITE_OK);
        sqlite3_bind_text(st, 1, cases[i].purpose, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, cases[i].model, -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 3, cases[i].pos);
        int r = sqlite3_step(st);
        if ((r == SQLITE_DONE) != cases[i].ok)
            fprintf(stderr, "capgate case %zu (%s %s): rc=%d %s\n", i, cases[i].purpose,
                    cases[i].model, r, sqlite3_errmsg(db));
        assert((r == SQLITE_DONE) == cases[i].ok);
        if (!cases[i].ok && r == SQLITE_CONSTRAINT &&
            strcmp(cases[i].purpose, "video") != 0)
            assert(strstr(sqlite3_errmsg(db), "capabilities do not include"));
        sqlite3_finalize(st);
    }
    /* Updates cannot sneak around it either. */
    assert(sqlite3_exec(db, "UPDATE agent_models SET model_id='plain@p'"
        " WHERE purpose='audio';", NULL, NULL, NULL) != SQLITE_OK);
    assert(sqlite3_exec(db, "UPDATE agent_models SET purpose='audio'"
        " WHERE purpose='chat';", NULL, NULL, NULL) != SQLITE_OK);
    assert(sqlite3_exec(db, "UPDATE agent_models SET pos=3"
        " WHERE purpose='audio';", NULL, NULL, NULL) == SQLITE_OK);
    /* One list per purpose: the same pos in two purposes is two rows. */
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM agent_models WHERE agent_name='A';",
        0, -1) == 5);
    db_close(db);
    test_db_clean(path);
}

int main(void) {
    TEST_INIT();
    RUN(test_config_registry);
    RUN(test_prune_inbox);
    RUN(test_prune_outbox);
    RUN(test_free_mb);
    RUN(test_seed_backfills_and_preserves);
    RUN(test_schema_state);
    RUN(test_agent_models_capability_gate);
    RUN(test_agent_models_cache_lifetime);
    RUN(test_agent_models_reasoning_display);
    RUN(test_rate_limit_and_cost);
    return 0;
}
