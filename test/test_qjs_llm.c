/* LLM() end to end, minus a real provider:
 *   - llm_request() (the parent-side core): SQL-built bodies per wire format,
 *     ladder fallback, opts.model narrowing, llm_responses archiving —
 *     against a seeded temp DB and a loopback HTTP mock;
 *   - the bridge: llm_bridge_start() serving a real UDS, driven by real QJS
 *     eval calling the LLM() global (the exact child code path, host mode);
 *   - the call cap (llm_max_calls_per_tool).
 * No real network; providers are loopback. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/stat.h>
#include "db.h"
#include "llm_bridge.h"
#include "llm_proc.h"
#include "qjs_helpers.h"
#include "test_util.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  " name "... "); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

#define DB_PATH "/tmp/test_qjs_llm.db"

/* ── loopback HTTP mock: serves `nconn` connections, one canned response
 * each, capturing each request ── */

#define MOCK_MAX_CONN 4

typedef struct {
    int listen_fd;
    int port;
    int nconn;
    const char *resp[MOCK_MAX_CONN];
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
        ssize_t n = recv(c, m->captured[i], sizeof(m->captured[i]) - 1, 0);
        if (n > 0) m->captured[i][n] = '\0';
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

/* ── DB seeding ── */

static sqlite3 *g_db;

static int seed(const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(g_db, sql, NULL, NULL, &err) != SQLITE_OK) {
        printf("(seed failed: %s) ", err ? err : "?");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

/* Point both providers at the mock and rebuild the routing list. */
static int route_two(int port_a, int port_g) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "DELETE FROM agent_models WHERE agent_name='t';"
        "UPDATE providers SET base_url='http://127.0.0.1:%d/v1' WHERE name='pa';"
        "UPDATE providers SET base_url='http://127.0.0.1:%d' WHERE name='pg';"
        "INSERT INTO agent_models(agent_name,model_id,pos) VALUES"
        " ('t','m-open',0),('t','m-gem',1);", port_a, port_g);
    return seed(sql);
}

static char *req(const char *agent, const char *request_json) {
    return llm_request(g_db, agent, request_json, "js:test", 7, 42);
}

static void test_openai_success(void) {
    TEST("openai_success");
    MockHttp m = { .nconn = 1, .resp = { RESP_OPENAI } };
    pthread_t tid;
    if (mock_http_start(&m, &tid) != 0) { FAIL("mock"); return; }
    route_two(m.port, 1);
    char *r = req("t",
        "{\"messages\":[{\"role\":\"system\",\"content\":\"be brief\"},"
        "{\"role\":\"user\",\"content\":\"say hi\"}],"
        "\"opts\":{\"max_tokens\":64,\"model\":\"open\"}}");
    mock_http_stop(&m, tid);
    if (!r || !strstr(r, "\"ok\":true") || !strstr(r, "\"text\":\"hello world\"") ||
        !strstr(r, "\"model\":\"open-model\"")) {
        FAIL(r ? r : "NULL"); free(r); return;
    }
    const char *c = m.captured[0];
    if (!strstr(c, "Authorization: Bearer k-test") ||
        !strstr(c, "\"model\":\"open-model\"") ||
        !strstr(c, "\"role\":\"system\"") ||
        !strstr(c, "\"max_tokens\":64")) {
        FAIL("request wrong"); printf("    got: %.400s\n", c); free(r); return;
    }
    free(r);
    PASS();
}

static void test_gemini_body_and_extra(void) {
    TEST("gemini_body_and_extra");
    MockHttp m = { .nconn = 1, .resp = { RESP_GEMINI } };
    pthread_t tid;
    if (mock_http_start(&m, &tid) != 0) { FAIL("mock"); return; }
    route_two(1, m.port);
    char *r = req("t",
        "{\"messages\":[{\"role\":\"system\",\"content\":\"ground it\"},"
        "{\"role\":\"user\",\"content\":\"find x\"}],"
        "\"opts\":{\"model\":\"gem\",\"max_tokens\":128,"
        "\"extra\":{\"tools\":[{\"google_search\":{}}]}}}");
    mock_http_stop(&m, tid);
    /* two parts joined with a newline */
    if (!r || !strstr(r, "part one\\npart two")) { FAIL(r ? r : "NULL"); free(r); return; }
    const char *c = m.captured[0];
    if (!strstr(c, "x-goog-api-key: k-test") ||
        !strstr(c, "\"contents\":") ||
        !strstr(c, "\"systemInstruction\":") ||
        !strstr(c, "\"maxOutputTokens\":128") ||
        !strstr(c, "google_search")) {
        FAIL("gemini body wrong"); printf("    got: %.500s\n", c); free(r); return;
    }
    if (strstr(c, "\"messages\":") || strstr(c, "\"max_tokens\":")) {
        FAIL("openai fields leaked into a gemini body"); free(r); return;
    }
    free(r);
    PASS();
}

static void test_fallback_walks_ladder(void) {
    TEST("fallback_walks_ladder");
    MockHttp m = { .nconn = 2, .resp = { RESP_500, RESP_GEMINI } };
    pthread_t tid;
    if (mock_http_start(&m, &tid) != 0) { FAIL("mock"); return; }
    route_two(m.port, m.port);   /* both candidates hit the same mock */
    char *r = req("t", "{\"messages\":[{\"role\":\"user\",\"content\":\"q\"}]}");
    mock_http_stop(&m, tid);
    if (!r || !strstr(r, "\"ok\":true") || !strstr(r, "\"id\":\"m-gem\"")) {
        FAIL(r ? r : "NULL"); free(r); return;
    }
    free(r);
    PASS();
}

static void test_model_filter_miss(void) {
    TEST("model_filter_miss");
    route_two(1, 1);
    char *r = req("t", "{\"messages\":[{\"role\":\"user\",\"content\":\"q\"}],"
                       "\"opts\":{\"model\":\"nonexistent\"}}");
    if (!r || !strstr(r, "\"ok\":false") || !strstr(r, "no routed model matches")) {
        FAIL(r ? r : "NULL"); free(r); return;
    }
    free(r);
    PASS();
}

static void test_no_routing(void) {
    TEST("no_routing");
    char *r = req("nobody", "{\"messages\":[{\"role\":\"user\",\"content\":\"q\"}]}");
    if (!r || !strstr(r, "no routable model")) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    PASS();
}

static void test_malformed_request(void) {
    TEST("malformed_request");
    char *r = req("t", "not json at all");
    if (!r || !strstr(r, "malformed request")) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    PASS();
}

static void test_failure_trail_and_archive(void) {
    TEST("failure_trail_and_archive");
    MockHttp m = { .nconn = 2, .resp = { RESP_500, RESP_500 } };
    pthread_t tid;
    if (mock_http_start(&m, &tid) != 0) { FAIL("mock"); return; }
    route_two(m.port, m.port);
    char *r = req("t", "{\"messages\":[{\"role\":\"user\",\"content\":\"q\"}]}");
    mock_http_stop(&m, tid);
    if (!r || !strstr(r, "every candidate failed") || !strstr(r, "m-open") ||
        !strstr(r, "500")) {
        FAIL(r ? r : "NULL"); free(r); return;
    }
    free(r);
    /* every attempt archived, stamped with the caller's session + source */
    sqlite3_stmt *s;
    int rows = 0;
    if (sqlite3_prepare_v2(g_db,
            "SELECT count(*) FROM llm_responses WHERE session_id=7"
            " AND status LIKE 'jsllm_%js:test'", -1, &s, NULL) == SQLITE_OK &&
        sqlite3_step(s) == SQLITE_ROW)
        rows = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    if (rows < 2) { FAIL("attempts not archived to llm_responses"); return; }
    PASS();
}

/* ── the bridge + the real LLM() global, over a real UDS ── */

#define BRIDGE_DIR "/tmp/test_qjs_llm_bridge"

static void test_bridge_roundtrip(void) {
    TEST("bridge_roundtrip");
    MockHttp m = { .nconn = 1, .resp = { RESP_OPENAI } };
    pthread_t tid;
    if (mock_http_start(&m, &tid) != 0) { FAIL("mock"); return; }
    route_two(m.port, 1);
    mkdir(BRIDGE_DIR, 0755);
    LlmBridge *b = llm_bridge_start(BRIDGE_DIR, DB_PATH, "t", "js:test", 7, 42);
    if (!b) { FAIL("bridge start"); mock_http_stop(&m, tid); return; }
    setenv("CCLAW_LLM_SOCK", llm_bridge_sock(b), 1);
    int is_error = 0;
    char *r = qjs_eval_run(
        "LLM('say hi', {system:'be brief', model:'open'})",
        NULL, NULL, NULL, &is_error);
    unsetenv("CCLAW_LLM_SOCK");
    llm_bridge_stop(b);
    mock_http_stop(&m, tid);
    if (!r || is_error || strcmp(r, "hello world") != 0) {
        FAIL(r ? r : "NULL"); free(r); return;
    }
    free(r);
    PASS();
}

static void test_bridge_full_result(void) {
    TEST("bridge_full_result");
    MockHttp m = { .nconn = 1, .resp = { RESP_OPENAI } };
    pthread_t tid;
    if (mock_http_start(&m, &tid) != 0) { FAIL("mock"); return; }
    route_two(m.port, 1);
    LlmBridge *b = llm_bridge_start(BRIDGE_DIR, DB_PATH, "t", "js:test", 7, 42);
    if (!b) { FAIL("bridge start"); mock_http_stop(&m, tid); return; }
    setenv("CCLAW_LLM_SOCK", llm_bridge_sock(b), 1);
    int is_error = 0;
    char *r = qjs_eval_run(
        "var f = LLM('q', {model:'open', full:true});"
        "f.text + '|' + f.model + '|' + f.id + '|' + f.status + '|' +"
        "f.body.choices[0].message.content",
        NULL, NULL, NULL, &is_error);
    unsetenv("CCLAW_LLM_SOCK");
    llm_bridge_stop(b);
    mock_http_stop(&m, tid);
    if (!r || is_error ||
        strcmp(r, "hello world|open-model|m-open|200|hello world") != 0) {
        FAIL(r ? r : "NULL"); free(r); return;
    }
    free(r);
    PASS();
}

static void test_bridge_error_throws(void) {
    TEST("bridge_error_throws");
    route_two(1, 1);
    LlmBridge *b = llm_bridge_start(BRIDGE_DIR, DB_PATH, "t", "js:test", 7, 42);
    if (!b) { FAIL("bridge start"); return; }
    setenv("CCLAW_LLM_SOCK", llm_bridge_sock(b), 1);
    int is_error = 0;
    char *r = qjs_eval_run("LLM('q', {model:'nonexistent'})",
                           NULL, NULL, NULL, &is_error);
    unsetenv("CCLAW_LLM_SOCK");
    llm_bridge_stop(b);
    if (!r || !is_error || !strstr(r, "no routed model matches")) {
        FAIL(r ? r : "NULL"); free(r); return;
    }
    free(r);
    PASS();
}

static void test_no_sock_throws(void) {
    TEST("no_sock_throws");
    unsetenv("CCLAW_LLM_SOCK");
    int is_error = 0;
    char *r = qjs_eval_run("LLM('hi')", NULL, NULL, NULL, &is_error);
    if (!r || !is_error || !strstr(r, "not available")) {
        FAIL(r ? r : "NULL"); free(r); return;
    }
    free(r);
    PASS();
}

static void test_call_cap(void) {
    TEST("call_cap");
    seed("INSERT OR REPLACE INTO config(key,value) VALUES('llm_max_calls_per_tool','1');");
    MockHttp m = { .nconn = 1, .resp = { RESP_OPENAI } };
    pthread_t tid;
    if (mock_http_start(&m, &tid) != 0) { FAIL("mock"); return; }
    route_two(m.port, 1);
    LlmBridge *b = llm_bridge_start(BRIDGE_DIR, DB_PATH, "t", "js:test", 7, 42);
    if (!b) { FAIL("bridge start"); mock_http_stop(&m, tid); return; }
    setenv("CCLAW_LLM_SOCK", llm_bridge_sock(b), 1);
    int is_error = 0;
    char *r = qjs_eval_run(
        "var one = LLM('q', {model:'open'});"
        "var msg = '';"
        "try { LLM('again', {model:'open'}); } catch (e) { msg = String(e.message || e); }"
        "one + '|' + msg",
        NULL, NULL, NULL, &is_error);
    unsetenv("CCLAW_LLM_SOCK");
    llm_bridge_stop(b);
    mock_http_stop(&m, tid);
    seed("DELETE FROM config WHERE key='llm_max_calls_per_tool';");
    if (!r || is_error || !strstr(r, "hello world|") ||
        !strstr(r, "call cap reached")) {
        FAIL(r ? r : "NULL"); free(r); return;
    }
    free(r);
    PASS();
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_qjs_llm:\n");

    unlink(DB_PATH); unlink(DB_PATH "-wal"); unlink(DB_PATH "-shm");
    g_db = test_db_open(DB_PATH);
    if (!g_db) { printf("FAIL: db open\n"); return 1; }
    setenv("TEST_LLM_KEY", "k-test", 1);
    if (seed("INSERT INTO agents(name) VALUES('t');"
             "INSERT INTO providers(name, base_url, endpoint_type, api_key_env)"
             " VALUES ('pa','http://127.0.0.1:1/v1','openai','TEST_LLM_KEY'),"
             "        ('pg','http://127.0.0.1:1','gemini','TEST_LLM_KEY');"
             "INSERT INTO models(id, provider_name, model) VALUES"
             " ('m-open','pa','open-model'),('m-gem','pg','gemini-test');") != 0) {
        printf("FAIL: seed\n"); return 1;
    }

    test_openai_success();
    test_gemini_body_and_extra();
    test_fallback_walks_ladder();
    test_model_filter_miss();
    test_no_routing();
    test_malformed_request();
    test_failure_trail_and_archive();
    test_bridge_roundtrip();
    test_bridge_full_result();
    test_bridge_error_throws();
    test_no_sock_throws();
    test_call_cap();

    sqlite3_close(g_db);
    unlink(DB_PATH); unlink(DB_PATH "-wal"); unlink(DB_PATH "-shm");
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
