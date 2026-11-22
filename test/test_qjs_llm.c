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
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "db.h"
#include "llm_bridge.h"
#include "llm_proc.h"
#include "log.h"
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

/* A script's extra may add fields. It may not pick a model the routing list
 * does not hold, nor undo what the operator put on the provider. */
static void test_extra_cannot_widen(void) {
    TEST("extra_cannot_widen");
    MockHttp m = { .nconn = 1, .resp = { RESP_OPENAI } };
    pthread_t tid;
    if (mock_http_start(&m, &tid) != 0) { FAIL("mock"); return; }
    route_two(m.port, 1);
    seed("UPDATE providers SET request_extra="
         "'{\"provider\":{\"zdr\":true}}' WHERE name='pa';");
    char *r = req("t",
        "{\"messages\":[{\"role\":\"user\",\"content\":\"q\"}],"
        "\"opts\":{\"model\":\"open\",\"extra\":{\"model\":\"other/expensive\","
        "\"provider\":{\"zdr\":false,\"sort\":\"price\"},\"seed\":7}}}");
    mock_http_stop(&m, tid);
    seed("UPDATE providers SET request_extra=NULL WHERE name='pa';");
    const char *c = m.captured[0];
    if (!r || !strstr(r, "\"ok\":true")) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    if (strstr(c, "other/expensive") || !strstr(c, "\"model\":\"open-model\"")) {
        FAIL("extra replaced the model"); printf("    got: %.400s\n", c); return;
    }
    if (!strstr(c, "\"zdr\":true") || strstr(c, "\"zdr\":false")) {
        FAIL("extra undid the provider's request_extra"); printf("    got: %.400s\n", c); return;
    }
    if (!strstr(c, "\"seed\":7") || !strstr(c, "\"sort\":\"price\"")) {
        FAIL("extra's own fields were dropped"); printf("    got: %.400s\n", c); return;
    }
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

/* ── opts.purpose: which of the agent's lists is walked ── */

/* chat = m-open only; utility = `utility_sql` (rows for agent 't'), or none. */
static int route_purposes(int port_a, int port_g, const char *utility_sql) {
    if (route_two(port_a, port_g) != 0) return -1;
    if (seed("DELETE FROM agent_models WHERE agent_name='t' AND model_id='m-gem';"
             "UPDATE models SET status='healthy';") != 0) return -1;
    return utility_sql ? seed(utility_sql) : 0;
}

#define UTILITY_GEM "INSERT INTO agent_models(agent_name,purpose,model_id,pos)" \
                    " VALUES('t','utility','m-gem',0);"
#define ASK(opts) "{\"messages\":[{\"role\":\"user\",\"content\":\"q\"}],\"opts\":" opts "}"

static void test_purpose_utility_list(void) {
    TEST("purpose_utility_list");
    MockHttp m = { .nconn = 2, .resp = { RESP_GEMINI, RESP_OPENAI } };
    pthread_t tid;
    if (mock_http_start(&m, &tid) != 0) { FAIL("mock"); return; }
    route_purposes(m.port, m.port, UTILITY_GEM);
    /* utility walks the utility list, and only it… */
    char *r = req("t", ASK("{\"purpose\":\"utility\"}"));
    /* …while chat, named or defaulted, still walks the chat list. */
    char *c = req("t", ASK("{\"purpose\":\"chat\"}"));
    mock_http_stop(&m, tid);
    if (!r || !strstr(r, "\"ok\":true") || !strstr(r, "\"id\":\"m-gem\"")) {
        FAIL(r ? r : "NULL"); free(r); free(c); return;
    }
    if (!c || !strstr(c, "\"ok\":true") || !strstr(c, "\"id\":\"m-open\"")) {
        FAIL(c ? c : "NULL"); free(r); free(c); return;
    }
    free(r); free(c);
    /* opts.model narrows within the chosen list, never across lists. */
    r = req("t", ASK("{\"purpose\":\"utility\",\"model\":\"open\"}"));
    if (!r || !strstr(r, "no routed model matches")) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    PASS();
}

static void test_purpose_utility_falls_back(void) {
    TEST("purpose_utility_falls_back_to_chat");
    MockHttp m = { .nconn = 1, .resp = { RESP_OPENAI } };
    pthread_t tid;
    if (mock_http_start(&m, &tid) != 0) { FAIL("mock"); return; }
    route_purposes(m.port, 1, NULL);            /* the agent never made a utility list */
    char *r = req("t", ASK("{\"purpose\":\"utility\"}"));
    mock_http_stop(&m, tid);
    if (!r || !strstr(r, "\"ok\":true") || !strstr(r, "\"id\":\"m-open\"")) {
        FAIL(r ? r : "NULL"); free(r); return;
    }
    free(r);
    PASS();
}

/* A utility list the agent does have, with nothing routable on it, is an
 * error — not a quiet move to the chat models. */
static void test_purpose_utility_unroutable(void) {
    TEST("purpose_utility_unroutable");
    route_purposes(1, 1, UTILITY_GEM "UPDATE models SET status='disabled' WHERE id='m-gem';");
    char *r = req("t", ASK("{\"purpose\":\"utility\"}"));
    seed("UPDATE models SET status='healthy';");
    if (!r || !strstr(r, "\"ok\":false") || !strstr(r, "utility list has no routable model")) {
        FAIL(r ? r : "NULL"); free(r); return;
    }
    free(r);
    PASS();
}

static void test_purpose_refused(void) {
    TEST("purpose_refused");
    route_purposes(1, 1, UTILITY_GEM);
    /* The media lists are not a script's to ask for; nor is anything unknown
     * or not a string. Nothing is sent (the providers are unreachable). */
    const char *bad[] = { "\"image\"", "\"audio\"", "\"Utility\"", "\"\"", "7", "null",
                          "[\"utility\"]" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        char body[256];
        snprintf(body, sizeof(body), ASK("{\"purpose\":%s}"), bad[i]);
        char *r = req("t", body);
        if (!r || !strstr(r, "\"ok\":false") ||
            !strstr(r, "opts.purpose must be \\\"chat\\\" or \\\"utility\\\"")) {
            printf("(%s) ", bad[i]); FAIL(r ? r : "NULL"); free(r); return;
        }
        free(r);
    }
    PASS();
}

/* The script's side: LLM(prompt, {purpose}) reaches the list, and a refusal
 * is a thrown error the script can read. */
static void test_purpose_via_bridge(void) {
    TEST("purpose_via_bridge");
    MockHttp m = { .nconn = 1, .resp = { RESP_GEMINI } };
    pthread_t tid;
    if (mock_http_start(&m, &tid) != 0) { FAIL("mock"); return; }
    route_purposes(1, m.port, UTILITY_GEM);
    LlmBridge *b = llm_bridge_start(BRIDGE_DIR, DB_PATH, "t", "hook:test", 7, 42);
    if (!b) { FAIL("bridge start"); mock_http_stop(&m, tid); return; }
    setenv("CCLAW_LLM_SOCK", llm_bridge_sock(b), 1);
    int is_error = 0;
    char *r = qjs_eval_run(
        "var f = LLM('q', {purpose:'utility', full:true});"
        "var e = ''; try { LLM('q', {purpose:'audio'}); } catch (x) { e = String(x.message); }"
        "f.id + '|' + e",
        NULL, NULL, NULL, &is_error);
    unsetenv("CCLAW_LLM_SOCK");
    llm_bridge_stop(b);
    mock_http_stop(&m, tid);
    if (!r || is_error ||
        strcmp(r, "m-gem|LLM: opts.purpose must be \"chat\" or \"utility\"") != 0) {
        FAIL(r ? r : "NULL"); free(r); return;
    }
    free(r);
    PASS();
}

/* An agent directory too deep for a socket address: the bridge is refused
 * (never bound at a truncated path) and the reason is logged. Runs last —
 * it turns the stderr tee on. */
static void test_bridge_long_path_is_loud(void) {
    TEST("bridge_long_path_is_loud");
    char dir[160] = "/tmp/test_qjs_llm_long_";
    memset(dir + strlen(dir), 'x', 110 - strlen(dir));     /* > sun_path with the name */
    mkdir(dir, 0755);
    char logf[] = "/tmp/test_qjs_llm_log_XXXXXX";
    int fd = mkstemp(logf), saved = dup(STDERR_FILENO);
    if (fd < 0 || saved < 0) { FAIL("capture"); return; }
    dup2(fd, STDERR_FILENO);
    close(fd);
    cclaw_log_init(1);
    LlmBridge *b = llm_bridge_start(dir, DB_PATH, "t", "hook:review", 7, 42);
    dup2(saved, STDERR_FILENO);
    close(saved);
    char got[1024] = "";
    FILE *f = fopen(logf, "r");
    if (f) { size_t n = fread(got, 1, sizeof(got) - 1, f); got[n] = '\0'; fclose(f); }
    unlink(logf);
    rmdir(dir);
    if (b) { llm_bridge_stop(b); FAIL("started on an over-long path"); return; }
    if (!strstr(got, "llm_bridge: not started for hook:review") ||
        !strstr(got, "the limit is 107") || !strstr(got, dir)) {
        FAIL(got); return;
    }
    /* A path that fits still starts. */
    b = llm_bridge_start(BRIDGE_DIR, DB_PATH, "t", "js:test", 7, 42);
    if (!b) { FAIL("short path refused"); return; }
    llm_bridge_stop(b);
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

/* ── opts.images ── */

#define IMG_PATH "/tmp/test_qjs_llm.png"
#define NOT_IMG_PATH "/tmp/test_qjs_llm.txt"
/* base64 of the 8-byte PNG signature begins with this */
#define PNG_B64_HEAD "iVBORw0KGgo"

static void write_file(const char *path, const void *d, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(d, 1, n, f);
    fclose(f);
}

static int archived_request_has(const char *needle) {
    sqlite3_stmt *s;
    int has = 0;
    if (sqlite3_prepare_v2(g_db,
            "SELECT instr(json(request_body), ?1) > 0 FROM llm_responses"
            " ORDER BY id DESC LIMIT 1", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, needle, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) has = sqlite3_column_int(s, 0);
    }
    sqlite3_finalize(s);
    return has;
}

/* The whole path a script takes: LLM(prompt, {images:[file]}) in real QJS →
 * the stub reads and encodes the file → bridge → a Gemini body with the
 * image as inlineData beside the prompt. The archived request keeps the
 * conversation but not the image bytes. */
static void test_images_gemini_via_bridge(void) {
    TEST("images_gemini_via_bridge");
    static const unsigned char png[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n',
                                         0x00, 0x00, 0xFF, 0xFE, 0x80 };
    write_file(IMG_PATH, png, sizeof(png));
    MockHttp m = { .nconn = 1, .resp = { RESP_GEMINI } };
    pthread_t tid;
    if (mock_http_start(&m, &tid) != 0) { FAIL("mock"); return; }
    route_two(1, m.port);
    LlmBridge *b = llm_bridge_start(BRIDGE_DIR, DB_PATH, "t", "js:test", 7, 42);
    if (!b) { FAIL("bridge start"); mock_http_stop(&m, tid); return; }
    setenv("CCLAW_LLM_SOCK", llm_bridge_sock(b), 1);
    int is_error = 0;
    char *r = qjs_eval_run(
        "LLM('what is this', {model:'gem', images:['" IMG_PATH "']})",
        NULL, NULL, NULL, &is_error);
    unsetenv("CCLAW_LLM_SOCK");
    llm_bridge_stop(b);
    mock_http_stop(&m, tid);
    if (!r || is_error || !strstr(r, "part one")) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    const char *c = m.captured[0];
    if (!strstr(c, "{\"text\":\"what is this\"}") ||
        !strstr(c, "\"inlineData\":{\"mimeType\":\"image/png\",\"data\":\"" PNG_B64_HEAD)) {
        FAIL("gemini image body wrong"); printf("    got: %.700s\n", c); return;
    }
    if (archived_request_has(PNG_B64_HEAD) || !archived_request_has("base64 chars]") ||
        !archived_request_has("what is this")) {
        FAIL("archive should keep the prompt and drop the image bytes"); return;
    }
    PASS();
}

/* The same images on an OpenAI-format provider: content parts with a data:
 * URL; a message that is not the last user message is left as it was. */
static void test_images_openai_body(void) {
    TEST("images_openai_body");
    MockHttp m = { .nconn = 1, .resp = { RESP_OPENAI } };
    pthread_t tid;
    if (mock_http_start(&m, &tid) != 0) { FAIL("mock"); return; }
    route_two(m.port, 1);
    char *r = req("t",
        "{\"messages\":[{\"role\":\"user\",\"content\":\"earlier\"},"
        "{\"role\":\"assistant\",\"content\":\"ok\"},"
        "{\"role\":\"user\",\"content\":\"describe\"}],"
        "\"opts\":{\"model\":\"open\"},"
        "\"images\":[{\"mime\":\"image/jpeg\",\"data\":\"QUJD\"}]}");
    mock_http_stop(&m, tid);
    if (!r || !strstr(r, "\"ok\":true")) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    const char *c = m.captured[0];
    if (!strstr(c, "{\"role\":\"user\",\"content\":\"earlier\"}") ||
        !strstr(c, "{\"type\":\"text\",\"text\":\"describe\"}") ||
        !strstr(c, "{\"type\":\"image_url\",\"image_url\":"
                   "{\"url\":\"data:image/jpeg;base64,QUJD\"}}")) {
        FAIL("openai image body wrong"); printf("    got: %.700s\n", c); return;
    }
    PASS();
}

/* And on the Anthropic Messages API: a base64 image block. A message with
 * plain string content is passed through as it was. */
static void test_images_anthropic_body(void) {
    TEST("images_anthropic_body");
    MockHttp m = { .nconn = 1, .resp = {
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
        "{\"content\":[{\"type\":\"text\",\"text\":\"a cat\"}]}" } };
    pthread_t tid;
    if (mock_http_start(&m, &tid) != 0) { FAIL("mock"); return; }
    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT OR REPLACE INTO providers(name, base_url, endpoint_type, api_key_env)"
        " VALUES('pc','http://127.0.0.1:%d/v1','anthropic','TEST_LLM_KEY');"
        "INSERT OR REPLACE INTO models(id, provider_name, model)"
        " VALUES('m-ant','pc','claude-test');"
        "DELETE FROM agent_models WHERE agent_name='t';"
        "INSERT INTO agent_models(agent_name,model_id,pos) VALUES('t','m-ant',0);",
        m.port);
    seed(sql);
    char *r = req("t",
        "{\"messages\":[{\"role\":\"user\",\"content\":\"earlier\"},"
        "{\"role\":\"assistant\",\"content\":\"ok\"},"
        "{\"role\":\"user\",\"content\":\"describe\"}],"
        "\"images\":[{\"mime\":\"image/jpeg\",\"data\":\"QUJD\"}]}");
    mock_http_stop(&m, tid);
    if (!r || !strstr(r, "\"text\":\"a cat\"")) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    const char *c = m.captured[0];
    if (!strstr(c, "{\"role\":\"user\",\"content\":\"earlier\"}") ||
        !strstr(c, "{\"type\":\"text\",\"text\":\"describe\"}") ||
        !strstr(c, "{\"type\":\"image\",\"source\":{\"type\":\"base64\","
                   "\"media_type\":\"image/jpeg\",\"data\":\"QUJD\"}}") ||
        strstr(c, "image_url")) {
        FAIL("anthropic image body wrong"); printf("    got: %.800s\n", c); return;
    }
    if (archived_request_has("QUJD") || !archived_request_has("base64 chars]")) {
        FAIL("archive should drop the image bytes"); return;
    }
    PASS();
}

/* llm_bridge_stop runs on the event-loop thread at child reap, so it must
 * wake the accept thread rather than wait out its 300 ms poll. The thread gets
 * 30 ms to be inside poll(); a stop that only waits then takes ~270 ms every
 * time. Best-of-three keeps a busy box from failing a working wake. */
static void test_bridge_stop_is_prompt(void) {
    TEST("bridge_stop_is_prompt");
    mkdir(BRIDGE_DIR, 0755);
    double best = 1e9;
    for (int i = 0; i < 3; i++) {
        LlmBridge *b = llm_bridge_start(BRIDGE_DIR, DB_PATH, "t", "js:test", 7, 42);
        if (!b) { FAIL("bridge start"); return; }
        usleep(30 * 1000);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        llm_bridge_stop(b);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double dt = (double)(t1.tv_sec - t0.tv_sec) * 1e3 +
                    (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
        if (dt < best) best = dt;
    }
    /* Stop racing a thread that may not have reached poll() yet. */
    for (int i = 0; i < 20; i++) {
        LlmBridge *b = llm_bridge_start(BRIDGE_DIR, DB_PATH, "t", "js:test", 7, 42);
        if (!b) { FAIL("bridge start"); return; }
        llm_bridge_stop(b);
    }
    if (best >= 120.0) { FAIL("llm_bridge_stop waited out the poll"); return; }
    PASS();
}

/* Refusals: a file that is not an image (child side, by its bytes), and a
 * request whose image fields could not have come from the stub (parent side
 * — the child is sandboxed code, so the parent checks again). */
static void test_images_refused(void) {
    TEST("images_refused");
    write_file(NOT_IMG_PATH, "just text", 9);
    route_two(1, 1);
    LlmBridge *b = llm_bridge_start(BRIDGE_DIR, DB_PATH, "t", "js:test", 7, 42);
    if (!b) { FAIL("bridge start"); return; }
    setenv("CCLAW_LLM_SOCK", llm_bridge_sock(b), 1);
    int e1 = 0, e2 = 0;
    char *r1 = qjs_eval_run("LLM('q', {images:['" NOT_IMG_PATH "']})",
                            NULL, NULL, NULL, &e1);
    char *r2 = qjs_eval_run("LLM('q', {images:['/tmp/test_qjs_llm_absent.png']})",
                            NULL, NULL, NULL, &e2);
    unsetenv("CCLAW_LLM_SOCK");
    llm_bridge_stop(b);
    char *r3 = req("t", "{\"messages\":[{\"role\":\"user\",\"content\":\"q\"}],"
                        "\"images\":[{\"mime\":\"text/html\",\"data\":\"QUJD\"}]}");
    char *r4 = req("t", "{\"messages\":[{\"role\":\"user\",\"content\":\"q\"}],"
                        "\"images\":[{\"mime\":\"image/png\",\"data\":\"a\\\"b\"}]}");
    int ok = r1 && e1 && strstr(r1, "is not a JPEG") &&
             r2 && e2 && strstr(r2, "cannot read") &&
             r3 && strstr(r3, "\"ok\":false") && strstr(r3, "opts.images") &&
             r4 && strstr(r4, "\"ok\":false") && strstr(r4, "opts.images");
    if (!ok) printf("\n    r1=%s\n    r2=%s\n    r3=%s\n    r4=%s\n    ",
                    r1 ? r1 : "NULL", r2 ? r2 : "NULL", r3 ? r3 : "NULL", r4 ? r4 : "NULL");
    free(r1); free(r2); free(r3); free(r4);
    unlink(NOT_IMG_PATH);
    if (ok) PASS(); else FAIL("not refused");
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
    test_extra_cannot_widen();
    test_fallback_walks_ladder();
    test_model_filter_miss();
    test_no_routing();
    test_malformed_request();
    test_failure_trail_and_archive();
    test_bridge_roundtrip();
    test_bridge_stop_is_prompt();
    test_bridge_full_result();
    test_bridge_error_throws();
    test_no_sock_throws();
    test_call_cap();
    test_images_gemini_via_bridge();
    test_images_openai_body();
    test_images_refused();
    test_images_anthropic_body();
    unlink(IMG_PATH);
    test_purpose_utility_list();
    test_purpose_utility_falls_back();
    test_purpose_utility_unroutable();
    test_purpose_refused();
    test_purpose_via_bridge();
    test_bridge_long_path_is_loud();

    sqlite3_close(g_db);
    unlink(DB_PATH); unlink(DB_PATH "-wal"); unlink(DB_PATH "-shm");
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
