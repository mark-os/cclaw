/* Integration test — probe-at-apply (config-ax Phase 2A).
 *
 * A request_changes document that moves routing is verified against the real
 * request path at apply time: probe OK → it stands and the receipt names the
 * model that served; probe failure (status, timeout) → the rows it replaced
 * come back and the receipt says so. Documents that don't move routing are
 * never probed. Nothing a probe does may touch the session's entries. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "db.h"
#include "mock_server.h"
#include "test_util.h"
#include "tool_request_config.h"

static int s_port;

static const char *PROBE_RESPONSE =
    "{\"id\":\"chatcmpl-probe\",\"model\":\"served/model-b\","
    "\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"ok\"},"
    "\"finish_reason\":\"stop\"}],"
    "\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":1}}";

/* ── fixture ─────────────────────────────────────────────────────────
 * One provider pointing at the mock, two models on it, an agent routed at
 * model-a. Every test then asks to switch to model-b. */
static int64_t seed(sqlite3 *db, const char *base_url) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "DELETE FROM models; DELETE FROM providers;"
        "INSERT INTO providers(name, base_url, endpoint_type, api_key_env)"
        " VALUES('mockp','%s','openai','');"
        "INSERT INTO models(id, provider_name, model, context_window)"
        " VALUES('model-a@mockp','mockp','model-a',128000),"
        "       ('model-b@mockp','mockp','model-b',128000);",
        base_url);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    test_seed_agent(db, "test");
    assert(sqlite3_exec(db,
        "DELETE FROM agent_models WHERE agent_name='test';"
        "INSERT INTO agent_models(agent_name,model_id,pos)"
        " VALUES('test','model-a@mockp',0);", NULL, NULL, NULL) == SQLITE_OK);
    return session_create(db, "probe", "test", -1, 0);
}

static char *scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK);
    char *out = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(s, 0);
        out = test_strdup_(v ? v : "");
    }
    sqlite3_finalize(s);
    return out;
}

static int count(sqlite3 *db, const char *sql) {
    char *v = scalar(db, sql);
    int n = v ? atoi(v) : -1;
    free(v);
    return n;
}

static const char *SWITCH_DOC =
    "{\"action\":\"request_changes\",\"changes\":"
    "{\"agent\":{\"models\":[\"model-b@mockp\"]}}}";

/* ── tests ───────────────────────────────────────────────────────────── */

static void test_probe_success_applies(void) {
    sqlite3 *db = test_db_open_seeded(":memory:");
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", s_port);
    int64_t sid = seed(db, url);

    mock_server_reset();
    mock_server_enqueue(200, PROBE_RESPONSE);

    char *receipt = NULL;
    assert(request_config_changes_apply(db, "test", SWITCH_DOC, 0, sid, &receipt) == 0);
    assert(receipt);
    /* The receipt reports the model that actually answered, not the intent. */
    assert(strstr(receipt, "probed OK"));
    assert(strstr(receipt, "served/model-b"));
    assert(mock_server_request_count() == 1);

    char *pm = scalar(db, "SELECT model_id FROM agent_models"
                          " WHERE agent_name='test' ORDER BY pos LIMIT 1");
    assert(strcmp(pm, "model-b@mockp") == 0);

    /* A probe is not traffic: it archives, and touches nothing else. */
    assert(count(db, "SELECT COUNT(*) FROM entries") == 0);
    assert(count(db, "SELECT COUNT(*) FROM llm_responses WHERE status='probe_ok'") == 1);
    assert(count(db, "SELECT COALESCE(SUM(total_requests),0) FROM models") == 0);

    free(pm); free(receipt);
    db_close(db);
    printf("  PASS test_probe_success_applies\n");
}

static void test_probe_failure_reverts(void) {
    sqlite3 *db = test_db_open_seeded(":memory:");
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", s_port);
    int64_t sid = seed(db, url);

    mock_server_reset();
    mock_server_enqueue(404, "{\"error\":{\"message\":\"no such model\"}}");

    char *receipt = NULL;
    assert(request_config_changes_apply(db, "test", SWITCH_DOC, 0, sid, &receipt) == -1);
    assert(receipt);
    assert(strstr(receipt, "probe failed"));
    assert(strstr(receipt, "http 404"));
    assert(strstr(receipt, "reverted to models=model-a@mockp"));

    char *pm = scalar(db, "SELECT model_id FROM agent_models"
                          " WHERE agent_name='test' ORDER BY pos LIMIT 1");
    assert(strcmp(pm, "model-a@mockp") == 0);
    assert(count(db, "SELECT COUNT(*) FROM entries") == 0);
    assert(count(db, "SELECT COUNT(*) FROM llm_responses"
                     " WHERE status='probe_http_404'") == 1);

    free(pm); free(receipt);
    db_close(db);
    printf("  PASS test_probe_failure_reverts\n");
}

/* A model registration whose provider points somewhere that accepts the
 * connection and then says nothing: the probe's 15s ceiling must treat it as
 * a failure, not wait for the provider's own (minutes-long) timeout. */
static void *blackhole_thread(void *arg) {
    int lfd = *(int *)arg;
    int c = accept(lfd, NULL, NULL);
    if (c >= 0) { sleep(20); close(c); }
    return NULL;
}

static void test_probe_timeout_is_failure(void) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    assert(lfd >= 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(lfd, (struct sockaddr *)&a, sizeof(a)) == 0);
    assert(listen(lfd, 1) == 0);
    socklen_t alen = sizeof(a);
    assert(getsockname(lfd, (struct sockaddr *)&a, &alen) == 0);
    struct timeval tv = {25, 0};
    setsockopt(lfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    pthread_t th;
    assert(pthread_create(&th, NULL, blackhole_thread, &lfd) == 0);

    sqlite3 *db = test_db_open_seeded(":memory:");
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", ntohs(a.sin_port));
    int64_t sid = seed(db, url);

    char *receipt = NULL;
    assert(request_config_changes_apply(db, "test", SWITCH_DOC, 0, sid, &receipt) == -1);
    assert(receipt);
    assert(strstr(receipt, "probe failed: timed out"));
    assert(strstr(receipt, "reverted to models=model-a@mockp"));
    char *pm = scalar(db, "SELECT model_id FROM agent_models"
                          " WHERE agent_name='test' ORDER BY pos LIMIT 1");
    assert(strcmp(pm, "model-a@mockp") == 0);
    assert(count(db, "SELECT COUNT(*) FROM llm_responses"
                     " WHERE status='probe_timeout'") == 1);

    free(pm); free(receipt);
    db_close(db);
    pthread_join(th, NULL);
    close(lfd);
    printf("  PASS test_probe_timeout_is_failure\n");
}

/* Grants and config values don't change who serves a request — no probe, and
 * therefore no way for a provider outage to block an unrelated grant. */
static void test_grants_only_does_not_probe(void) {
    sqlite3 *db = test_db_open_seeded(":memory:");
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", s_port);
    int64_t sid = seed(db, url);

    mock_server_reset();
    char *receipt = NULL;
    const char *doc = "{\"action\":\"request_changes\",\"changes\":"
                      "{\"grants\":{\"tools\":[\"shell_exec\"]}}}";
    assert(request_config_changes_apply(db, "test", doc, 0, sid, &receipt) == 0);
    assert(receipt && strstr(receipt, "grants now"));
    assert(!strstr(receipt, "probed"));
    assert(mock_server_request_count() == 0);
    assert(count(db, "SELECT COUNT(*) FROM llm_responses") == 0);

    free(receipt);
    db_close(db);
    printf("  PASS test_grants_only_does_not_probe\n");
}

int main(void) {
    TEST_INIT();
    s_port = mock_server_start();
    if (s_port < 0) { printf("FAIL: mock server\n"); return 1; }

    printf("config probe integration tests:\n");
    test_probe_success_applies();
    test_probe_failure_reverts();
    test_probe_timeout_is_failure();
    test_grants_only_does_not_probe();

    mock_server_stop();
    printf("All config probe tests passed.\n");
    return 0;
}
