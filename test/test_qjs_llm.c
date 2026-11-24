/* LLM() end to end, minus a real provider:
 *   - llm_request() (the parent-side core): SQL-built bodies per wire format,
 *     ladder fallback, opts.model narrowing, llm_responses archiving —
 *     against a seeded temp DB and a loopback HTTP mock;
 *   - the bridge: llm_bridge_start() serving a real UDS, driven by real QJS
 *     eval calling the LLM() global (the exact child code path, uncontained);
 *   - the call cap (llm_max_calls_per_tool).
 * No real network; providers are loopback. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
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

static void seed(const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(g_db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "seed failed: %s\n", err ? err : "?");
        abort();
    }
}

static void expect_in(const char *hay, const char *needle) {
    if (!hay || !strstr(hay, needle)) {
        fprintf(stderr, "FAIL: \"%s\" not in: %.800s\n", needle, hay ? hay : "(null)");
        abort();
    }
}

static void expect_not_in(const char *hay, const char *needle) {
    if (hay && strstr(hay, needle)) {
        fprintf(stderr, "FAIL: \"%s\" in: %.800s\n", needle, hay);
        abort();
    }
}

static void mock_start(MockHttp *m, pthread_t *tid) {
    assert(mock_http_start(m, tid) == 0);
}

/* Point both providers at the mock and rebuild the routing list. */
static void route_two(int port_a, int port_g) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
        "DELETE FROM agent_models WHERE agent_name='t';"
        "UPDATE providers SET base_url='http://127.0.0.1:%d/v1' WHERE name='pa';"
        "UPDATE providers SET base_url='http://127.0.0.1:%d' WHERE name='pg';"
        "INSERT INTO agent_models(agent_name,model_id,pos) VALUES"
        " ('t','m-open',0),('t','m-gem',1);", port_a, port_g);
    seed(sql);
}

/* chat = m-open only; utility = `utility_sql` (rows for agent 't'), or none. */
static void route_purposes(int port_a, int port_g, const char *utility_sql) {
    route_two(port_a, port_g);
    seed("DELETE FROM agent_models WHERE agent_name='t' AND model_id='m-gem';"
         "UPDATE models SET status='healthy';");
    if (utility_sql) seed(utility_sql);
}

#define UTILITY_GEM "INSERT INTO agent_models(agent_name,purpose,model_id,pos)" \
                    " VALUES('t','utility','m-gem',0);"
#define ASK(opts) "{\"messages\":[{\"role\":\"user\",\"content\":\"q\"}],\"opts\":" opts "}"

static char *req(const char *agent, const char *request_json) {
    return llm_request(g_db, agent, request_json, "js:test", 7, 42);
}

#define BRIDGE_DIR "/tmp/test_qjs_llm_bridge"

/* Evaluate js in real QJS with the LLM() global wired to a fresh bridge for
 * agent 't' (the exact child code path, uncontained). */
static char *bridge_run(const char *source, const char *js, int *is_error) {
    LlmBridge *b = llm_bridge_start(BRIDGE_DIR, DB_PATH, "t", source, 7, 42);
    assert(b);
    setenv("CCLAW_LLM_SOCK", llm_bridge_sock(b), 1);
    *is_error = 0;
    char *r = qjs_eval_run(js, NULL, NULL, NULL, is_error);
    unsetenv("CCLAW_LLM_SOCK");
    llm_bridge_stop(b);
    return r;
}

static void test_openai_success(void) {
    MockHttp m = { .nconn = 1, .resp = { RESP_OPENAI } };
    pthread_t tid;
    mock_start(&m, &tid);
    route_two(m.port, 1);
    char *r = req("t",
        "{\"messages\":[{\"role\":\"system\",\"content\":\"be brief\"},"
        "{\"role\":\"user\",\"content\":\"say hi\"}],"
        "\"opts\":{\"max_tokens\":64,\"model\":\"open\"}}");
    mock_http_stop(&m, tid);
    expect_in(r, "\"ok\":true");
    expect_in(r, "\"text\":\"hello world\"");
    expect_in(r, "\"model\":\"open-model\"");
    const char *c = m.captured[0];
    expect_in(c, "Authorization: Bearer k-test");
    expect_in(c, "\"model\":\"open-model\"");
    expect_in(c, "\"role\":\"system\"");
    expect_in(c, "\"max_tokens\":64");
    free(r);
}

static void test_gemini_body_and_extra(void) {
    MockHttp m = { .nconn = 1, .resp = { RESP_GEMINI } };
    pthread_t tid;
    mock_start(&m, &tid);
    route_two(1, m.port);
    char *r = req("t",
        "{\"messages\":[{\"role\":\"system\",\"content\":\"ground it\"},"
        "{\"role\":\"user\",\"content\":\"find x\"}],"
        "\"opts\":{\"model\":\"gem\",\"max_tokens\":128,"
        "\"extra\":{\"tools\":[{\"google_search\":{}}]}}}");
    mock_http_stop(&m, tid);
    expect_in(r, "part one\\npart two");      /* two parts joined with a newline */
    const char *c = m.captured[0];
    expect_in(c, "x-goog-api-key: k-test");
    expect_in(c, "\"contents\":");
    expect_in(c, "\"systemInstruction\":");
    expect_in(c, "\"maxOutputTokens\":128");
    expect_in(c, "google_search");
    /* no OpenAI fields leak into a Gemini body */
    expect_not_in(c, "\"messages\":");
    expect_not_in(c, "\"max_tokens\":");
    free(r);
}

/* A script's extra may add fields. It may not pick a model the routing list
 * does not hold, nor undo what the operator put on the provider. */
static void test_extra_cannot_widen(void) {
    MockHttp m = { .nconn = 1, .resp = { RESP_OPENAI } };
    pthread_t tid;
    mock_start(&m, &tid);
    route_two(m.port, 1);
    seed("UPDATE providers SET request_extra='{\"provider\":{\"zdr\":true}}' WHERE name='pa';");
    char *r = req("t",
        "{\"messages\":[{\"role\":\"user\",\"content\":\"q\"}],"
        "\"opts\":{\"model\":\"open\",\"extra\":{\"model\":\"other/expensive\","
        "\"provider\":{\"zdr\":false,\"sort\":\"price\"},\"seed\":7}}}");
    mock_http_stop(&m, tid);
    seed("UPDATE providers SET request_extra=NULL WHERE name='pa';");
    const char *c = m.captured[0];
    expect_in(r, "\"ok\":true");
    free(r);
    expect_not_in(c, "other/expensive");      /* extra did not replace the model */
    expect_in(c, "\"model\":\"open-model\"");
    expect_in(c, "\"zdr\":true");             /* nor undo the provider's request_extra */
    expect_not_in(c, "\"zdr\":false");
    expect_in(c, "\"seed\":7");               /* its own fields are kept */
    expect_in(c, "\"sort\":\"price\"");
}

static void test_fallback_walks_ladder(void) {
    MockHttp m = { .nconn = 2, .resp = { RESP_500, RESP_GEMINI } };
    pthread_t tid;
    mock_start(&m, &tid);
    route_two(m.port, m.port);   /* both candidates hit the same mock */
    char *r = req("t", "{\"messages\":[{\"role\":\"user\",\"content\":\"q\"}]}");
    mock_http_stop(&m, tid);
    expect_in(r, "\"ok\":true");
    expect_in(r, "\"id\":\"m-gem\"");
    free(r);
}

/* Requests refused before anything is sent (the providers are unreachable):
 * an opts.model no routed model matches, an agent with no routing, a request
 * that is not JSON, a utility list with nothing routable on it (an error, not
 * a quiet move to the chat models), and an opts.purpose that is not chat or
 * utility: the media lists are not a script's to ask for, nor is anything
 * unknown or not a string. */
static void test_refused_requests(void) {
    static const struct { const char *agent, *utility, *request, *keyword; } cases[] = {
        { "t", NULL, ASK("{\"model\":\"nonexistent\"}"), "no routed model matches" },
        { "nobody", NULL, ASK("{}"), "no routable model" },
        { "t", NULL, "not json at all", "malformed request" },
        { "t", UTILITY_GEM "UPDATE models SET status='disabled' WHERE id='m-gem';",
          ASK("{\"purpose\":\"utility\"}"), "utility list has no routable model" },
        { "t", UTILITY_GEM, ASK("{\"purpose\":\"image\"}"), "opts.purpose" },
        { "t", UTILITY_GEM, ASK("{\"purpose\":\"audio\"}"), "opts.purpose" },
        { "t", UTILITY_GEM, ASK("{\"purpose\":\"Utility\"}"), "opts.purpose" },
        { "t", UTILITY_GEM, ASK("{\"purpose\":\"\"}"), "opts.purpose" },
        { "t", UTILITY_GEM, ASK("{\"purpose\":7}"), "opts.purpose" },
        { "t", UTILITY_GEM, ASK("{\"purpose\":null}"), "opts.purpose" },
        { "t", UTILITY_GEM, ASK("{\"purpose\":[\"utility\"]}"), "opts.purpose" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        route_purposes(1, 1, cases[i].utility);
        char *r = req(cases[i].agent, cases[i].request);
        if (!r || !strstr(r, "\"ok\":false") || !strstr(r, cases[i].keyword)) {
            fprintf(stderr, "FAIL cases[%zu]: %s\n", i, r ? r : "(null)");
            abort();
        }
        free(r);
    }
    seed("UPDATE models SET status='healthy';");
}

static void test_failure_trail_and_archive(void) {
    MockHttp m = { .nconn = 2, .resp = { RESP_500, RESP_500 } };
    pthread_t tid;
    mock_start(&m, &tid);
    route_two(m.port, m.port);
    char *r = req("t", "{\"messages\":[{\"role\":\"user\",\"content\":\"q\"}]}");
    mock_http_stop(&m, tid);
    expect_in(r, "every candidate failed");
    expect_in(r, "m-open");
    expect_in(r, "500");
    free(r);
    /* every attempt archived, stamped with the caller's session + source */
    assert(test_qi(g_db, "SELECT count(*) FROM llm_responses WHERE session_id=7"
                         " AND status LIKE 'jsllm_%js:test'") >= 2);
}

/* ── the bridge + the real LLM() global, over a real UDS ── */

static void test_bridge_roundtrip(void) {
    MockHttp m = { .nconn = 1, .resp = { RESP_OPENAI } };
    pthread_t tid;
    mock_start(&m, &tid);
    route_two(m.port, 1);
    mkdir(BRIDGE_DIR, 0755);
    int is_error;
    char *r = bridge_run("js:test", "LLM('say hi', {system:'be brief', model:'open'})", &is_error);
    mock_http_stop(&m, tid);
    assert(r && !is_error && strcmp(r, "hello world") == 0);
    free(r);
}

static void test_bridge_full_result(void) {
    MockHttp m = { .nconn = 1, .resp = { RESP_OPENAI } };
    pthread_t tid;
    mock_start(&m, &tid);
    route_two(m.port, 1);
    int is_error;
    char *r = bridge_run("js:test",
        "var f = LLM('q', {model:'open', full:true});"
        "f.text + '|' + f.model + '|' + f.id + '|' + f.status + '|' +"
        "f.body.choices[0].message.content", &is_error);
    mock_http_stop(&m, tid);
    assert(r && !is_error && strcmp(r, "hello world|open-model|m-open|200|hello world") == 0);
    free(r);
}

/* ── opts.purpose: which of the agent's lists is walked ── */

static void test_purpose_utility_list(void) {
    MockHttp m = { .nconn = 2, .resp = { RESP_GEMINI, RESP_OPENAI } };
    pthread_t tid;
    mock_start(&m, &tid);
    route_purposes(m.port, m.port, UTILITY_GEM);
    /* utility walks the utility list, and only it… */
    char *r = req("t", ASK("{\"purpose\":\"utility\"}"));
    /* …while chat, named or defaulted, still walks the chat list. */
    char *c = req("t", ASK("{\"purpose\":\"chat\"}"));
    mock_http_stop(&m, tid);
    expect_in(r, "\"ok\":true");
    expect_in(r, "\"id\":\"m-gem\"");
    expect_in(c, "\"ok\":true");
    expect_in(c, "\"id\":\"m-open\"");
    free(r); free(c);
    /* opts.model narrows within the chosen list, never across lists. */
    r = req("t", ASK("{\"purpose\":\"utility\",\"model\":\"open\"}"));
    expect_in(r, "no routed model matches");
    free(r);
}

static void test_purpose_utility_falls_back(void) {
    MockHttp m = { .nconn = 1, .resp = { RESP_OPENAI } };
    pthread_t tid;
    mock_start(&m, &tid);
    route_purposes(m.port, 1, NULL);            /* the agent never made a utility list */
    char *r = req("t", ASK("{\"purpose\":\"utility\"}"));
    mock_http_stop(&m, tid);
    expect_in(r, "\"ok\":true");
    expect_in(r, "\"id\":\"m-open\"");
    free(r);
}

/* The script's side: LLM(prompt, {purpose}) reaches the list, and a refusal
 * is a thrown error the script can read. */
static void test_purpose_via_bridge(void) {
    MockHttp m = { .nconn = 1, .resp = { RESP_GEMINI } };
    pthread_t tid;
    mock_start(&m, &tid);
    route_purposes(1, m.port, UTILITY_GEM);
    int is_error;
    char *r = bridge_run("hook:test",
        "var f = LLM('q', {purpose:'utility', full:true});"
        "var e = ''; try { LLM('q', {purpose:'audio'}); } catch (x) { e = String(x.message); }"
        "f.id + '|' + e", &is_error);
    mock_http_stop(&m, tid);
    assert(r && !is_error);
    expect_in(r, "m-gem|");
    expect_in(r, "opts.purpose");
    free(r);
}

/* An agent directory too deep for a socket address: the bridge is refused
 * (never bound at a truncated path) and the reason is logged. Runs last:
 * it turns the stderr tee on. */
static void test_bridge_long_path_is_loud(void) {
    char dir[160] = "/tmp/test_qjs_llm_long_";
    memset(dir + strlen(dir), 'x', 110 - strlen(dir));     /* > sun_path with the name */
    mkdir(dir, 0755);
    char logf[] = "/tmp/test_qjs_llm_log_XXXXXX";
    int fd = mkstemp(logf), saved = dup(STDERR_FILENO);
    assert(fd >= 0 && saved >= 0);
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
    assert(!b);                               /* not started on an over-long path */
    expect_in(got, "llm_bridge: not started for hook:review");
    expect_in(got, dir);
    /* A path that fits still starts. */
    b = llm_bridge_start(BRIDGE_DIR, DB_PATH, "t", "js:test", 7, 42);
    assert(b);
    llm_bridge_stop(b);
}

static void test_call_cap(void) {
    seed("INSERT OR REPLACE INTO config(key,value) VALUES('llm_max_calls_per_tool','1');");
    MockHttp m = { .nconn = 1, .resp = { RESP_OPENAI } };
    pthread_t tid;
    mock_start(&m, &tid);
    route_two(m.port, 1);
    int is_error;
    char *r = bridge_run("js:test",
        "var one = LLM('q', {model:'open'});"
        "var msg = '';"
        "try { LLM('again', {model:'open'}); } catch (e) { msg = String(e.message || e); }"
        "one + '|' + msg", &is_error);
    mock_http_stop(&m, tid);
    seed("DELETE FROM config WHERE key='llm_max_calls_per_tool';");
    assert(r && !is_error);
    expect_in(r, "hello world|");
    expect_in(r, "call cap");
    free(r);
}

/* llm_bridge_stop runs on the event-loop thread at child reap, so it must
 * wake the accept thread rather than wait out its 300 ms poll. The thread gets
 * 30 ms to be inside poll(); a stop that only waits then takes ~270 ms every
 * time. Best-of-three keeps a busy box from failing a working wake. */
static void test_bridge_stop_is_prompt(void) {
    mkdir(BRIDGE_DIR, 0755);
    double best = 1e9;
    for (int i = 0; i < 3; i++) {
        LlmBridge *b = llm_bridge_start(BRIDGE_DIR, DB_PATH, "t", "js:test", 7, 42);
        assert(b);
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
        assert(b);
        llm_bridge_stop(b);
    }
    assert(best < 120.0);                     /* did not wait out the poll */
}

/* ── opts.images ── */

#define IMG_PATH "/tmp/test_qjs_llm.png"
#define NOT_IMG_PATH "/tmp/test_qjs_llm.txt"
/* base64 of the 8-byte PNG signature begins with this */
#define PNG_B64_HEAD "iVBORw0KGgo"

static void write_file(const char *path, const void *d, size_t n) {
    FILE *f = fopen(path, "wb");
    assert(f);
    fwrite(d, 1, n, f);
    fclose(f);
}

static int archived_request_has(const char *needle) {
    char *sql = sqlite3_mprintf("SELECT instr(json(request_body), %Q) > 0 FROM llm_responses"
                                " ORDER BY id DESC LIMIT 1", needle);
    int has = (int)test_qi(g_db, sql);
    sqlite3_free(sql);
    return has == 1;
}

#define IMG_REQ(opts) "{\"messages\":[{\"role\":\"user\",\"content\":\"earlier\"}," \
    "{\"role\":\"assistant\",\"content\":\"ok\"},{\"role\":\"user\",\"content\":\"describe\"}]," \
    opts "\"images\":[{\"mime\":\"image/jpeg\",\"data\":\"QUJD\"}]}"
#define EARLIER "{\"role\":\"user\",\"content\":\"earlier\"}"
#define DESCRIBE "{\"type\":\"text\",\"text\":\"describe\"}"

/* Images on each wire format. Gemini takes the whole path a script takes:
 * LLM(prompt, {images:[file]}) in real QJS, the stub reads and encodes the
 * file, the bridge, then inlineData beside the prompt. OpenAI: content parts
 * with a data: URL. Anthropic: a base64 image block. A message that is not the
 * last user message (plain string content) is passed through as it was. The
 * archived request keeps the conversation but not the image bytes. */
static const struct {
    const char *provider;       /* pa, pg, or pc (Anthropic, added here) */
    const char *js, *request;   /* through the bridge, else straight to llm_request */
    const char *reply, *result;
    const char *want[3], *never, *bytes, *kept;
} images[] = {
    { "pg", "LLM('what is this', {model:'gem', images:['" IMG_PATH "']})", NULL,
      RESP_GEMINI, "part one",
      { "{\"text\":\"what is this\"}",
        "\"inlineData\":{\"mimeType\":\"image/png\",\"data\":\"" PNG_B64_HEAD, NULL },
      NULL, PNG_B64_HEAD, "what is this" },
    { "pa", NULL, IMG_REQ("\"opts\":{\"model\":\"open\"},"),
      RESP_OPENAI, "\"ok\":true",
      { EARLIER, DESCRIBE,
        "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/jpeg;base64,QUJD\"}}" },
      NULL, NULL, NULL },
    { "pc", NULL, IMG_REQ(""),
      "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
      "{\"content\":[{\"type\":\"text\",\"text\":\"a cat\"}]}", "\"text\":\"a cat\"",
      { EARLIER, DESCRIBE,
        "{\"type\":\"image\",\"source\":{\"type\":\"base64\","
        "\"media_type\":\"image/jpeg\",\"data\":\"QUJD\"}}" },
      "image_url", "QUJD", NULL },
};

static void test_images_per_format(void) {
    static const unsigned char png[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n',
                                         0x00, 0x00, 0xFF, 0xFE, 0x80 };
    write_file(IMG_PATH, png, sizeof(png));
    for (size_t i = 0; i < sizeof(images) / sizeof(images[0]); i++) {
        printf("    images[%zu] %s\n", i, images[i].provider);
        MockHttp m = { .nconn = 1, .resp = { images[i].reply } };
        pthread_t tid;
        mock_start(&m, &tid);
        if (strcmp(images[i].provider, "pc") == 0) {
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
        } else if (strcmp(images[i].provider, "pg") == 0) {
            route_two(1, m.port);
        } else {
            route_two(m.port, 1);
        }
        int is_error = 0;
        char *r = images[i].js ? bridge_run("js:test", images[i].js, &is_error)
                               : req("t", images[i].request);
        mock_http_stop(&m, tid);
        assert(!is_error);
        expect_in(r, images[i].result);
        free(r);
        for (int k = 0; k < 3; k++)
            if (images[i].want[k]) expect_in(m.captured[0], images[i].want[k]);
        if (images[i].never) expect_not_in(m.captured[0], images[i].never);
        if (images[i].bytes) {
            assert(!archived_request_has(images[i].bytes));
            assert(archived_request_has("base64 chars]"));
        }
        if (images[i].kept) assert(archived_request_has(images[i].kept));
    }
    unlink(IMG_PATH);
}

/* Refusals: a file that is not an image (child side, by its bytes), and a
 * request whose image fields could not have come from the stub (parent side:
 * the child is sandboxed code, so the parent checks again). */
static void test_images_refused(void) {
    write_file(NOT_IMG_PATH, "just text", 9);
    route_two(1, 1);
    int e1, e2;
    char *r1 = bridge_run("js:test", "LLM('q', {images:['" NOT_IMG_PATH "']})", &e1);
    char *r2 = bridge_run("js:test", "LLM('q', {images:['/tmp/test_qjs_llm_absent.png']})", &e2);
    char *r3 = req("t", "{\"messages\":[{\"role\":\"user\",\"content\":\"q\"}],"
                        "\"images\":[{\"mime\":\"text/html\",\"data\":\"QUJD\"}]}");
    char *r4 = req("t", "{\"messages\":[{\"role\":\"user\",\"content\":\"q\"}],"
                        "\"images\":[{\"mime\":\"image/png\",\"data\":\"a\\\"b\"}]}");
    assert(e1 && e2);
    expect_in(r1, "not a JPEG");
    expect_in(r2, "cannot read");
    expect_in(r3, "\"ok\":false");
    expect_in(r3, "opts.images");
    expect_in(r4, "\"ok\":false");
    expect_in(r4, "opts.images");
    free(r1); free(r2); free(r3); free(r4);
    unlink(NOT_IMG_PATH);
}

int main(void) {
    TEST_INIT();
    printf("test_qjs_llm:\n");

    test_db_clean(DB_PATH);
    g_db = test_db_open(DB_PATH);
    assert(g_db);
    setenv("TEST_LLM_KEY", "k-test", 1);
    seed("INSERT INTO agents(name) VALUES('t');"
         "INSERT INTO providers(name, base_url, endpoint_type, api_key_env)"
         " VALUES ('pa','http://127.0.0.1:1/v1','openai','TEST_LLM_KEY'),"
         "        ('pg','http://127.0.0.1:1','gemini','TEST_LLM_KEY');"
         "INSERT INTO models(id, provider_name, model) VALUES"
         " ('m-open','pa','open-model'),('m-gem','pg','gemini-test');");

    RUN(test_openai_success);
    RUN(test_gemini_body_and_extra);
    RUN(test_extra_cannot_widen);
    RUN(test_fallback_walks_ladder);
    RUN(test_refused_requests);
    RUN(test_failure_trail_and_archive);
    RUN(test_bridge_roundtrip);
    RUN(test_bridge_stop_is_prompt);
    RUN(test_bridge_full_result);
    RUN(test_call_cap);
    RUN(test_images_per_format);
    RUN(test_images_refused);
    RUN(test_purpose_utility_list);
    RUN(test_purpose_utility_falls_back);
    RUN(test_purpose_via_bridge);
    RUN(test_bridge_long_path_is_loud);

    sqlite3_close(g_db);
    test_db_clean(DB_PATH);
    return 0;
}
