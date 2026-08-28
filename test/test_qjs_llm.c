/* llm() in the sandboxed JS tier, both halves:
 *   - parent: llm_wire_json() renders the agent's routing list into candidate
 *     descriptors (url, full auth header, format, host, merged extra);
 *   - child: the llm() global builds the right body per wire format, walks
 *     the candidate ladder on failure, honors opts.model narrowing, and
 *     throws with a per-candidate trail when everything fails.
 * Host mode (no fork, no sandbox); HTTP goes to loopback mocks only. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "db.h"
#include "llm_proc.h"
#include "qjs_llm.h"
#include "test_util.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  " name "... "); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

/* ── loopback HTTP mock: serves `nconn` connections, one canned response
 * each, capturing each request. ── */

#define MOCK_MAX_CONN 4

typedef struct {
    int listen_fd;
    int port;
    int nconn;
    const char *resp[MOCK_MAX_CONN];   /* full HTTP response per connection */
    char captured[MOCK_MAX_CONN][8192];
} MockHttp;

static void *mock_http_thread(void *arg) {
    MockHttp *m = arg;
    struct timeval tv = { .tv_sec = 8 };
    setsockopt(m->listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    for (int i = 0; i < m->nconn; i++) {
        int c = accept(m->listen_fd, NULL, NULL);
        if (c < 0) return NULL;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        /* Read until the request body is plausibly complete (single recv is
         * enough for these sizes on loopback; retry once for slow paths). */
        ssize_t n = recv(c, m->captured[i], sizeof(m->captured[i]) - 1, 0);
        if (n > 0) {
            m->captured[i][n] = '\0';
            if (!strstr(m->captured[i], "}") && n < (ssize_t)sizeof(m->captured[i]) - 1) {
                ssize_t n2 = recv(c, m->captured[i] + n,
                                  sizeof(m->captured[i]) - 1 - (size_t)n, 0);
                if (n2 > 0) m->captured[i][n + n2] = '\0';
            }
        }
        send(c, m->resp[i], strlen(m->resp[i]), 0);
        close(c);
    }
    return NULL;
}

static int mock_http_start(MockHttp *m, pthread_t *tid) {
    m->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (m->listen_fd < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET,
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (bind(m->listen_fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        listen(m->listen_fd, 4) != 0) { close(m->listen_fd); return -1; }
    socklen_t alen = sizeof(a);
    getsockname(m->listen_fd, (struct sockaddr *)&a, &alen);
    m->port = ntohs(a.sin_port);
    return pthread_create(tid, NULL, mock_http_thread, m);
}

static void mock_http_stop(MockHttp *m, pthread_t tid) {
    pthread_join(tid, NULL);
    close(m->listen_fd);
}

/* Close-delimited bodies — no Content-Length to get wrong. */
#define RESP_OPENAI \
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n" \
    "Connection: close\r\n\r\n" \
    "{\"choices\":[{\"message\":{\"content\":\"hello world\"}}]}"
#define RESP_GEMINI \
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n" \
    "Connection: close\r\n\r\n" \
    "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"part one\"},{\"text\":\"part two\"}]}}]}"
#define RESP_500 \
    "HTTP/1.1 500 Internal Server Error\r\n" \
    "Connection: close\r\n\r\n{\"error\":\"boo\"}"

/* Run js code with the given candidates JSON set. Drives qjs_eval_run
 * directly (not the wire harness, which would overwrite the setter with the
 * request's own NULL llm_json — production sets it from the blob). */
static char *run_with_llm(const char *llm_json, const char *code) {
    qjs_host_set_llm(llm_json);
    int is_error = 0;
    char *r = qjs_eval_run(code, NULL, NULL, NULL, &is_error);
    qjs_host_set_llm(NULL);
    return r;
}

static void test_no_candidates_throws(void) {
    TEST("no_candidates_throws");
    char *r = run_with_llm(NULL, "llm('hi')");
    if (!r || !strstr(r, "no routable model")) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    PASS();
}

static void test_openai_success(void) {
    TEST("openai_success");
    MockHttp m = { .nconn = 1, .resp = { RESP_OPENAI } };
    pthread_t tid;
    if (mock_http_start(&m, &tid) != 0) { FAIL("mock"); return; }
    char cands[512];
    snprintf(cands, sizeof(cands),
        "[{\"id\":\"m1\",\"model\":\"test-model\","
        "\"url\":\"http://127.0.0.1:%d/chat/completions\","
        "\"auth\":\"Authorization: Bearer k-one\",\"format\":\"openai\","
        "\"host\":\"127.0.0.1\",\"extra\":{}}]", m.port);
    char *r = run_with_llm(cands,
        "llm('say hi', {system:'be brief', max_tokens: 64})");
    mock_http_stop(&m, tid);
    if (!r || strcmp(r, "hello world") != 0) { FAIL(r ? r : "NULL"); free(r); return; }
    const char *c = m.captured[0];
    if (!strstr(c, "Authorization: Bearer k-one") ||
        !strstr(c, "\"model\":\"test-model\"") ||
        !strstr(c, "\"role\":\"system\"") ||
        !strstr(c, "\"max_tokens\":64")) {
        FAIL("request body/headers wrong"); printf("    got: %.400s\n", c);
        free(r); return;
    }
    free(r);
    PASS();
}

static void test_gemini_success_and_extra(void) {
    TEST("gemini_success_and_extra");
    MockHttp m = { .nconn = 1, .resp = { RESP_GEMINI } };
    pthread_t tid;
    if (mock_http_start(&m, &tid) != 0) { FAIL("mock"); return; }
    char cands[512];
    snprintf(cands, sizeof(cands),
        "[{\"id\":\"g1\",\"model\":\"gemini-test\","
        "\"url\":\"http://127.0.0.1:%d/models/gemini-test:generateContent\","
        "\"auth\":\"x-goog-api-key: gk\",\"format\":\"gemini\","
        "\"host\":\"127.0.0.1\",\"extra\":{}}]", m.port);
    char *r = run_with_llm(cands,
        "llm('find x', {system:'ground it', "
        "extra:{tools:[{google_search:{}}]}})");
    mock_http_stop(&m, tid);
    /* Two parts joined with a newline; eval result prints both lines. */
    if (!r || !strstr(r, "part one") || !strstr(r, "part two")) {
        FAIL(r ? r : "NULL"); free(r); return;
    }
    const char *c = m.captured[0];
    if (!strstr(c, "x-goog-api-key: gk") ||
        !strstr(c, "\"contents\":") ||
        !strstr(c, "\"systemInstruction\":") ||
        !strstr(c, "google_search")) {
        FAIL("gemini body wrong"); printf("    got: %.400s\n", c);
        free(r); return;
    }
    if (strstr(c, "\"messages\":")) {
        FAIL("openai shape sent to a gemini endpoint"); free(r); return;
    }
    free(r);
    PASS();
}

static void test_fallback_walks_ladder(void) {
    TEST("fallback_walks_ladder");
    MockHttp m = { .nconn = 2, .resp = { RESP_500, RESP_OPENAI } };
    pthread_t tid;
    if (mock_http_start(&m, &tid) != 0) { FAIL("mock"); return; }
    char cands[1024];
    snprintf(cands, sizeof(cands),
        "[{\"id\":\"bad\",\"model\":\"bad-model\","
        "\"url\":\"http://127.0.0.1:%d/v1\",\"auth\":\"Authorization: Bearer a\","
        "\"format\":\"openai\",\"host\":\"127.0.0.1\",\"extra\":{}},"
        "{\"id\":\"good\",\"model\":\"good-model\","
        "\"url\":\"http://127.0.0.1:%d/v1\",\"auth\":\"Authorization: Bearer b\","
        "\"format\":\"openai\",\"host\":\"127.0.0.1\",\"extra\":{}}]",
        m.port, m.port);
    char *r = run_with_llm(cands, "llm('q')");
    mock_http_stop(&m, tid);
    if (!r || strcmp(r, "hello world") != 0) { FAIL(r ? r : "NULL"); free(r); return; }
    if (!strstr(m.captured[1], "good-model")) {
        FAIL("second candidate was not the one that served"); free(r); return;
    }
    free(r);
    PASS();
}

static void test_model_filter(void) {
    TEST("model_filter");
    MockHttp m = { .nconn = 1, .resp = { RESP_OPENAI } };
    pthread_t tid;
    if (mock_http_start(&m, &tid) != 0) { FAIL("mock"); return; }
    char cands[1024];
    snprintf(cands, sizeof(cands),
        "[{\"id\":\"first\",\"model\":\"alpha\",\"url\":\"http://127.0.0.1:1/v1\","
        "\"auth\":\"Authorization: Bearer a\",\"format\":\"openai\","
        "\"host\":\"127.0.0.1\",\"extra\":{}},"
        "{\"id\":\"second\",\"model\":\"beta-pro\","
        "\"url\":\"http://127.0.0.1:%d/v1\",\"auth\":\"Authorization: Bearer b\","
        "\"format\":\"openai\",\"host\":\"127.0.0.1\",\"extra\":{}}]", m.port);
    /* Matches only the second candidate — the first (dead port) must be
     * skipped without an attempt, or this test times out. */
    char *r = run_with_llm(cands, "llm('q', {model:'beta'})");
    mock_http_stop(&m, tid);
    if (!r || strcmp(r, "hello world") != 0) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    PASS();
}

static void test_model_filter_miss_throws(void) {
    TEST("model_filter_miss_throws");
    const char *cands =
        "[{\"id\":\"only\",\"model\":\"alpha\",\"url\":\"http://127.0.0.1:1/v1\","
        "\"auth\":\"Authorization: Bearer a\",\"format\":\"openai\","
        "\"host\":\"127.0.0.1\",\"extra\":{}}]";
    char *r = run_with_llm(cands, "llm('q', {model:'nonexistent'})");
    if (!r || !strstr(r, "no routed model matches")) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    PASS();
}

static void test_all_fail_names_candidates(void) {
    TEST("all_fail_names_candidates");
    MockHttp m = { .nconn = 1, .resp = { RESP_500 } };
    pthread_t tid;
    if (mock_http_start(&m, &tid) != 0) { FAIL("mock"); return; }
    char cands[512];
    snprintf(cands, sizeof(cands),
        "[{\"id\":\"only\",\"model\":\"alpha\","
        "\"url\":\"http://127.0.0.1:%d/v1\",\"auth\":\"Authorization: Bearer a\","
        "\"format\":\"openai\",\"host\":\"127.0.0.1\",\"extra\":{}}]", m.port);
    char *r = run_with_llm(cands, "llm('q')");
    mock_http_stop(&m, tid);
    if (!r || !strstr(r, "every candidate failed") || !strstr(r, "only") ||
        !strstr(r, "500")) {
        FAIL(r ? r : "NULL"); free(r); return;
    }
    free(r);
    PASS();
}

static void test_full_result(void) {
    TEST("full_result");
    MockHttp m = { .nconn = 1, .resp = { RESP_OPENAI } };
    pthread_t tid;
    if (mock_http_start(&m, &tid) != 0) { FAIL("mock"); return; }
    char cands[512];
    snprintf(cands, sizeof(cands),
        "[{\"id\":\"m1\",\"model\":\"test-model\","
        "\"url\":\"http://127.0.0.1:%d/v1\",\"auth\":\"Authorization: Bearer a\","
        "\"format\":\"openai\",\"host\":\"127.0.0.1\",\"extra\":{}}]", m.port);
    char *r = run_with_llm(cands,
        "var f = llm('q', {full:true}); "
        "f.text + '|' + f.model + '|' + f.id + '|' + f.status + '|' + "
        "f.body.choices[0].message.content");
    mock_http_stop(&m, tid);
    if (!r || strcmp(r, "hello world|test-model|m1|200|hello world") != 0) {
        FAIL(r ? r : "NULL"); free(r); return;
    }
    free(r);
    PASS();
}

/* llm's parsed descriptors — auth included — must not be reachable from JS.
 * The only visible surface is the llm function itself. */
static void test_no_key_leak_into_js(void) {
    TEST("no_key_leak_into_js");
    const char *cands =
        "[{\"id\":\"m1\",\"model\":\"x\",\"url\":\"http://127.0.0.1:1/v1\","
        "\"auth\":\"Authorization: Bearer SUPERSECRET\",\"format\":\"openai\","
        "\"host\":\"127.0.0.1\",\"extra\":{}}]";
    char *r = run_with_llm(cands,
        "var found = ''; "
        "for (var k of Object.getOwnPropertyNames(globalThis)) { "
        "  try { var s = JSON.stringify(globalThis[k]); "
        "        if (s && s.indexOf('SUPERSECRET') >= 0) found = k; } "
        "  catch (e) {} } "
        "found === '' ? 'clean' : 'LEAK:' + found");
    if (!r || strcmp(r, "clean") != 0) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    PASS();
}

/* ── parent side: llm_wire_json over a seeded DB ── */

static void test_wire_json(void) {
    TEST("wire_json");
    const char *path = "/tmp/test_qjs_llm.db";
    unlink(path);
    char wal[256], shm[256];
    snprintf(wal, sizeof(wal), "%s-wal", path); unlink(wal);
    snprintf(shm, sizeof(shm), "%s-shm", path); unlink(shm);
    sqlite3 *db = test_db_open(path);
    if (!db) { FAIL("db open"); return; }
    setenv("TEST_LLM_KEY", "k-wire-1", 1);
    const char *seed =
        "INSERT INTO agents(name) VALUES('wiretest');"
        "INSERT INTO providers(name, base_url, endpoint_type, api_key_env,"
        "  request_extra) VALUES"
        "  ('prov-a','https://api.prov-a.test/v1','openai','TEST_LLM_KEY',"
        "   '{\"provider\":{\"order\":[\"x\"]}}'),"
        "  ('prov-g','https://gem.test/v1beta','gemini','TEST_LLM_KEY',NULL);"
        "INSERT INTO models(id, provider_name, model) VALUES"
        "  ('a-fast','prov-a','fast-1'),"
        "  ('g-flash','prov-g','gemini-2.5-flash');"
        "INSERT INTO agent_models(agent_name, model_id, pos) VALUES"
        "  ('wiretest','a-fast',0),('wiretest','g-flash',1);";
    char *err = NULL;
    if (sqlite3_exec(db, seed, NULL, NULL, &err) != SQLITE_OK) {
        printf("FAIL: seed: %s\n", err ? err : "?");
        sqlite3_free(err); sqlite3_close(db); return;
    }
    char *json = llm_wire_json(db, "wiretest");
    if (!json) { FAIL("NULL wire json"); sqlite3_close(db); return; }
    int ok = 1;
    /* candidate 1: openai — /chat/completions appended, bearer auth,
     * request_extra carried into extra. */
    if (!strstr(json, "\"url\":\"https://api.prov-a.test/v1/chat/completions\"")) ok = 0;
    if (!strstr(json, "Bearer k-wire-1")) ok = 0;
    if (!strstr(json, "\"host\":\"api.prov-a.test\"")) ok = 0;
    if (!strstr(json, "\"order\":[\"x\"]")) ok = 0;
    /* candidate 2: gemini — generateContent URL, x-goog auth. */
    if (!strstr(json, ":generateContent\"")) ok = 0;
    if (!strstr(json, "x-goog-api-key: k-wire-1")) ok = 0;
    if (!strstr(json, "\"host\":\"gem.test\"")) ok = 0;
    /* order preserved: a-fast first */
    const char *p1 = strstr(json, "a-fast"), *p2 = strstr(json, "g-flash");
    if (!p1 || !p2 || p1 > p2) ok = 0;
    if (!ok) { FAIL("wire json shape"); printf("    got: %s\n", json); }
    else PASS();
    if (json) { memset(json, 0, strlen(json)); free(json); }
    /* no routing → NULL */
    tests_run++; printf("  wire_json_empty... ");
    char *none = llm_wire_json(db, "nobody");
    if (none) { FAIL("expected NULL for unrouted agent"); free(none); }
    else PASS();
    sqlite3_close(db);
    unlink(path); unlink(wal); unlink(shm);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_qjs_llm:\n");

    test_no_candidates_throws();
    test_openai_success();
    test_gemini_success_and_extra();
    test_fallback_walks_ladder();
    test_model_filter();
    test_model_filter_miss_throws();
    test_all_fail_names_candidates();
    test_full_result();
    test_no_key_leak_into_js();
    test_wire_json();

    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
