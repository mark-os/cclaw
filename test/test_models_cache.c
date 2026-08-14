/* models_cache: ingest, the size rules, and the TTL — all offline. The network
 * half (models_cache_refresh) is deliberately untested here: unit tests never
 * touch a real host, and the interesting logic all lives on the SQL side of
 * the fetch. */
#include "test_util.h"
#include "models_cache.h"
#include "buf.h"
#include "config_registry.h"

#include <assert.h>

static const char *CATALOG =
    "{\"data\":["
    " {\"id\":\"vendor/alpha\",\"context_length\":128000,"
    "  \"pricing\":{\"prompt\":\"0.000001\",\"completion\":\"0.000002\"},"
    "  \"architecture\":{\"modality\":\"text->text\"},"
    "  \"description\":\"a very long blob we must not store\"},"
    " {\"id\":\"vendor/beta\",\"context_length\":8192,"
    "  \"pricing\":{\"prompt\":\"0\",\"completion\":\"0\"},"
    "  \"architecture\":{\"modality\":\"text+image->text\"}},"
    " {\"context_length\":1}"        /* no id — skipped, not fatal */
    "]}";

static long long row_count(sqlite3 *db, const char *provider) {
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM models_cache WHERE provider_name=?1", -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_text(s, 1, provider, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    long long n = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return n;
}

static void seed_provider(sqlite3 *db, const char *name) {
    char sql[256];
    snprintf(sql, sizeof(sql),
             "INSERT OR REPLACE INTO providers(name, base_url, api_key_env)"
             " VALUES('%s','http://127.0.0.1:1/v1','')", name);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
}

/* Fresh synced_at so the lazy TTL never fires — no test may reach the network. */
static void touch_fresh(sqlite3 *db) {
    assert(sqlite3_exec(db, "UPDATE models_cache SET synced_at=unixepoch()",
                        NULL, NULL, NULL) == SQLITE_OK);
}

static void test_ingest(sqlite3 *db) {
    char err[256] = "";
    assert(models_cache_ingest(db, "openrouter", CATALOG, err, sizeof(err)) == 0);
    assert(row_count(db, "openrouter") == 2);

    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "SELECT context_length, prompt_price, modality FROM models_cache"
        " WHERE provider_name='openrouter' AND id='vendor/alpha'", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_int64(s, 0) == 128000);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "0.000001") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 2), "text->text") == 0);
    sqlite3_finalize(s);

    /* Re-ingest replaces, never accumulates. */
    assert(models_cache_ingest(db, "openrouter",
                               "{\"data\":[{\"id\":\"only\"}]}", err, sizeof(err)) == 0);
    assert(row_count(db, "openrouter") == 1);

    /* Garbage leaves the previous cache alone. */
    assert(models_cache_ingest(db, "openrouter", "<html>nope</html>",
                               err, sizeof(err)) == -1);
    assert(err[0] != '\0');
    assert(row_count(db, "openrouter") == 1);
    assert(models_cache_ingest(db, "openrouter", "{\"models\":[]}",
                               err, sizeof(err)) == -1);
    assert(row_count(db, "openrouter") == 1);

    printf("  PASS ingest\n");
}

static void test_query(sqlite3 *db) {
    char err[256] = "", *out = NULL;
    assert(models_cache_ingest(db, "openrouter", CATALOG, err, sizeof(err)) == 0);
    touch_fresh(db);

    /* Small catalog: listable unfiltered. */
    assert(models_cache_query(db, "openrouter", NULL, 10, 1, 0, &out, err, sizeof(err)) == 0);
    assert(strstr(out, "vendor/alpha") && strstr(out, "vendor/beta"));
    assert(strstr(out, "ctx=128000") && strstr(out, "prompt=$0.000001"));
    assert(strstr(out, "more — narrow") == NULL);
    free(out); out = NULL;

    /* Substring filter. */
    assert(models_cache_query(db, "openrouter", "beta", 10, 1, 0, &out, err, sizeof(err)) == 0);
    assert(strstr(out, "vendor/beta") && !strstr(out, "vendor/alpha"));
    free(out); out = NULL;

    /* Row cap reports the remainder. */
    assert(models_cache_query(db, "openrouter", "vendor", 1, 1, 0, &out, err, sizeof(err)) == 0);
    assert(strstr(out, "(1 more — page=2 or narrow your query)"));
    free(out); out = NULL;

    /* No match is an answer, not an error. */
    assert(models_cache_query(db, "openrouter", "zzz", 10, 1, 0, &out, err, sizeof(err)) == 0);
    assert(strstr(out, "no model matches"));
    free(out); out = NULL;

    printf("  PASS query\n");
}

/* Above MODELS_CACHE_LIST_MAX rows a search term is mandatory — the rule that
 * keeps an aggregator's catalog out of a context window. */
static void test_query_required(sqlite3 *db) {
    Buf b = {0};
    buf_append_str(&b, "{\"data\":[");
    for (int i = 0; i < MODELS_CACHE_LIST_MAX + 5; i++)
        buf_appendf(&b, "%s{\"id\":\"m%03d\",\"context_length\":100}", i ? "," : "", i);
    buf_append_str(&b, "]}");
    char *big = buf_take(&b);
    assert(big != NULL);

    char err[256] = "", *out = NULL;
    assert(models_cache_ingest(db, "aggregator", big, err, sizeof(err)) == 0);
    free(big);
    assert(row_count(db, "aggregator") == MODELS_CACHE_LIST_MAX + 5);
    touch_fresh(db);

    err[0] = '\0';
    assert(models_cache_query(db, "aggregator", NULL, 10, 1, 0, &out, err, sizeof(err)) == -1);
    assert(out == NULL);
    assert(strstr(err, "supply a search term"));

    /* With a term it answers, capped. */
    assert(models_cache_query(db, "aggregator", "m0", 10, 1, 0, &out, err, sizeof(err)) == 0);
    assert(strstr(out, "more — page=2 or narrow your query"));
    free(out);

    printf("  PASS query_required\n");
}

/* Paging is plain OFFSET over the same ordering: page 2 continues where page 1
 * stopped, and a page past the end says so rather than erroring. */
static void test_paging(sqlite3 *db) {
    char err[256] = "", *out = NULL;
    assert(models_cache_ingest(db, "pager",
        "{\"data\":[{\"id\":\"a\"},{\"id\":\"b\"},{\"id\":\"c\"}]}",
        err, sizeof(err)) == 0);
    touch_fresh(db);

    assert(models_cache_query(db, "pager", NULL, 2, 1, 0, &out, err, sizeof(err)) == 0);
    assert(strstr(out, "\na  ") && strstr(out, "\nb  ") && !strstr(out, "\nc  "));
    assert(strstr(out, "(1 more — page=2 or narrow your query)"));
    free(out); out = NULL;

    assert(models_cache_query(db, "pager", NULL, 2, 2, 0, &out, err, sizeof(err)) == 0);
    assert(strstr(out, "\nc  ") && !strstr(out, "\na  "));
    assert(strstr(out, "more —") == NULL);
    free(out); out = NULL;

    assert(models_cache_query(db, "pager", NULL, 2, 9, 0, &out, err, sizeof(err)) == 0);
    assert(strstr(out, "no models listed on page 9"));
    free(out);

    printf("  PASS paging\n");
}

/* A stale row is what makes the lazy refresh fire; a fresh one must not. The
 * probe here can only fail (the seeded base_url is a dead port), so a stale
 * cache surfaces as an error and a fresh cache as a clean listing. */
static void test_ttl(sqlite3 *db) {
    char err[256] = "", *out = NULL;
    seed_provider(db, "openrouter");
    assert(models_cache_ingest(db, "openrouter", CATALOG, err, sizeof(err)) == 0);

    touch_fresh(db);
    assert(models_cache_query(db, "openrouter", NULL, 10, 1, 0, &out, err, sizeof(err)) == 0);
    free(out); out = NULL;

    assert(sqlite3_exec(db, "UPDATE models_cache SET synced_at = unixepoch() - 99999",
                        NULL, NULL, NULL) == SQLITE_OK);
    err[0] = '\0';
    assert(models_cache_query(db, "openrouter", NULL, 10, 1, 0, &out, err, sizeof(err)) == -1);
    assert(strstr(err, "failed") != NULL);
    assert(out == NULL);
    /* The failed probe wrote nothing — the stale rows are still there. */
    assert(row_count(db, "openrouter") == 2);

    printf("  PASS ttl\n");
}

/* Mark's call: search_models is granted-only. The gate is the ordinary one —
 * a tool is callable iff a grants row names it — so the whole decision is the
 * absence of the name from the default grant list. */
static void test_not_default_granted(sqlite3 *db) {
    char *defaults = config_get(db, "agent_default_tools");
    assert(defaults != NULL);
    assert(strstr(defaults, "search_config") != NULL);   /* list really loaded */
    assert(strstr(defaults, "search_models") == NULL);
    free(defaults);

    char *worker = config_get(db, "worker_tools");
    assert(worker != NULL && strstr(worker, "search_models") == NULL);
    free(worker);

    printf("  PASS not_default_granted\n");
}

int main(void) {
    TEST_INIT();
    const char *path = "/tmp/test_cclaw_models_cache.sqlite";
    test_db_clean(path);
    sqlite3 *db = test_db_open(path);
    assert(db != NULL);

    printf("models_cache tests\n");
    test_ingest(db);
    test_query(db);
    test_query_required(db);
    test_paging(db);
    test_ttl(db);
    test_not_default_granted(db);

    sqlite3_close(db);
    test_db_clean(path);
    printf("ALL MODELS_CACHE TESTS PASSED\n");
    return 0;
}
