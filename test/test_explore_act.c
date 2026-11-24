/* The data explorer's actions (src/explore_act.c, specs/explorer.md
 * "Actions"): every action's success path and each of its refusals, against
 * a real schema; the provider checks against a loopback mock (good key, bad
 * key, nothing listening); the change log's record of what the dashboard
 * handle did; and that a secret value submitted through an action appears in
 * no response, no change_log row and no explorer read. */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "approval.h"
#include "db.h"
#include "explore.h"
#include "explore_act.h"
#include "test_util.h"

#define DB "test_explore_act.db"
#define MARK "QQSECRETMARK5521"
#define GOODKEY "goodkey-" MARK
#define PORT 19893

static const uint8_t TEST_KEY[32] = {
    9, 8, 7, 6, 5, 4, 3, 2, 1, 10, 11, 12, 13, 14, 15, 16,
    17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32
};

static sqlite3 *g_db;
static int g_answers;
static char *g_all;   /* every answer, concatenated, for the secret check */

static void exec(const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(g_db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "exec failed: %s\n%s\n", err, sql);
        assert(0);
    }
}

static int qeq(const char *sql, const char *want) {
    char *s = test_qs(g_db, sql);
    int ok = s && strcmp(s, want) == 0;
    if (!ok) fprintf(stderr, "got [%s] want [%s] for %s\n", s ? s : "(null)", want, sql);
    free(s);
    return ok;
}

#define qi(sql) test_qi(g_db, sql)

static void collect(const char *out) {
    size_t a = g_all ? strlen(g_all) : 0, b = strlen(out);
    g_all = realloc(g_all, a + b + 2);
    memcpy(g_all + a, out, b);
    g_all[a + b] = '\n';
    g_all[a + b + 1] = '\0';
}

/* Run an action; assert its status; return the answer (static, overwritten). */
static const char *act(int want, const char *json) {
    static char *last;
    free(last);
    last = NULL;
    int s = explore_act(g_db, json, &last);
    g_answers++;
    collect(last);
    if (s != want) fprintf(stderr, "status %d (want %d) for %s\n -> %s\n", s, want, json, last);
    assert(s == want);
    if (want == 200) assert(strstr(last, "\"ok\":true") && strstr(last, "\"message\""));
    else assert(strstr(last, "\"error\""));
    return last;
}

static void has(const char *out, const char *needle) {
    if (!strstr(out, needle)) fprintf(stderr, "missing [%s] in %s\n", needle, out);
    assert(strstr(out, needle));
}

/* Actions answered with a status and, when kw is set, one word that says
 * which check refused. Rows run in order: later rows see earlier writes. */
typedef struct { int status; const char *json; const char *kw; } Case;

static void cases(const Case *c, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const char *out = act(c[i].status, c[i].json);
        if (c[i].kw && !strstr(out, c[i].kw)) {
            fprintf(stderr, "row %zu: missing [%s] in %s\n  for %s\n", i, c[i].kw, out, c[i].json);
            assert(0);
        }
    }
}
#define CASES(...) do { static const Case c_[] = { __VA_ARGS__ }; \
                        cases(c_, sizeof(c_) / sizeof(c_[0])); } while (0)

#define GA(agent, rest) "{\"action\":\"grant_add\",\"agent\":\"" agent "\"," rest "}"
#define MS(purpose, models) "{\"action\":\"models_set\",\"agent\":\"Beta\",\"purpose\":\"" \
                            purpose "\",\"models\":[" models "]}"

/* The last change_log row for a table: "actor|context|action". */
static int logged(const char *table, const char *want) {
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT actor || '|' || COALESCE(context, '') || '|' || action"
             " FROM change_log WHERE table_name = '%s' ORDER BY id DESC LIMIT 1", table);
    return qeq(sql, want);
}

/* ── a loopback provider ─────────────────────────────────────────────────── */

static volatile int g_stop;
static int g_listen = -1;

static void reply(int fd, int status, const char *body) {
    char head[256];
    int n = snprintf(head, sizeof(head), "HTTP/1.1 %d X\r\nContent-Type: application/json\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n", status, strlen(body));
    if (write(fd, head, (size_t)n) < 0 || write(fd, body, strlen(body)) < 0) { /* client gone */ }
}

/* /ok/models (Bearer), /gem/models?pageSize=1000 (x-goog-api-key),
 * /anth/models?limit=1000 (x-api-key): the catalog when the key is GOODKEY,
 * 401 otherwise. /open/models answers anyone. Anything else 404. */
static void serve_one(int fd) {
    char req[8192];
    size_t len = 0;
    struct timeval tv = { 3, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    while (len < sizeof(req) - 1) {
        ssize_t r = read(fd, req + len, sizeof(req) - 1 - len);
        if (r <= 0) break;
        len += (size_t)r;
        req[len] = '\0';
        if (strstr(req, "\r\n\r\n")) break;
    }
    req[len] = '\0';
    static const char OAI[] = "{\"data\":[{\"id\":\"vendor/textonly\",\"context_length\":32000,"
        "\"architecture\":{\"modality\":\"text->text\"}},{\"id\":\"vendor/seer\",\"context_length\":"
        "128000,\"architecture\":{\"modality\":\"text+image->text\"}},{\"id\":\"vendor/ear\","
        "\"architecture\":{\"modality\":\"text+audio->text\"}}]}";
    static const char GEM[] = "{\"models\":[{\"name\":\"models/gem-a\",\"inputTokenLimit\":1048576},"
        "{\"name\":\"models/gem-b\"}]}";
    int good = strstr(req, GOODKEY) != NULL;
    if (strncmp(req, "GET /ok/models ", 15) == 0)
        reply(fd, good && strstr(req, "Authorization: Bearer " GOODKEY) ? 200 : 401,
              good ? OAI : "{\"error\":\"bad key\"}");
    else if (strncmp(req, "GET /gem/models?pageSize=1000 ", 30) == 0)
        reply(fd, good && strstr(req, "x-goog-api-key: " GOODKEY) ? 200 : 401,
              good ? GEM : "{\"error\":\"bad key\"}");
    else if (strncmp(req, "GET /anth/models?limit=1000 ", 28) == 0)
        reply(fd, good && strstr(req, "x-api-key: " GOODKEY) && strstr(req, "anthropic-version:")
              ? 200 : 401, good ? OAI : "{\"error\":\"bad key\"}");
    else if (strncmp(req, "GET /open/models ", 17) == 0)
        reply(fd, 200, OAI);
    else
        reply(fd, 404, "{}");
    close(fd);
}

static void *server(void *arg) {
    (void)arg;
    while (!g_stop) {
        struct pollfd p = { g_listen, POLLIN, 0 };
        if (poll(&p, 1, 200) <= 0) continue;
        int fd = accept(g_listen, NULL, NULL);
        if (fd >= 0) serve_one(fd);
    }
    return NULL;
}

static pthread_t server_start(void) {
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct timeval tv = { 5, 0 };
    setsockopt(g_listen, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(g_listen, (struct sockaddr *)&a, sizeof(a)) == 0);
    assert(listen(g_listen, 8) == 0);
    pthread_t t;
    assert(pthread_create(&t, NULL, server, NULL) == 0);
    return t;
}

/* ── the world ───────────────────────────────────────────────────────────── */

static void seed(void) {
    exec("INSERT INTO providers(name, base_url, api_key_env) VALUES"
         " ('mock', 'http://127.0.0.1:19893/ok', 'MOCK_API_KEY');"
         "INSERT INTO models(id, provider_name, model, capabilities) VALUES"
         " ('mock/a', 'mock', 'a', '[\"text\"]'), ('mock/b', 'mock', 'b', '[\"text\",\"image\"]'),"
         " ('mock/c', 'mock', 'c', '[\"text\"]');"
         "INSERT INTO agents(name) VALUES ('Alpha'), ('Beta');"
         "INSERT INTO agent_models(agent_name, purpose, model_id, pos, reasoning_effort) VALUES"
         " ('Alpha', 'chat', 'mock/a', 0, 'high'), ('Alpha', 'image', 'mock/b', 0, NULL),"
         " ('Beta', 'chat', 'mock/a', 0, NULL);"
         "INSERT INTO tools(name, description) VALUES ('shell_exec', 'run'), ('web_fetch', 'fetch'),"
         " ('extension_publish', 'publish');"
         "INSERT INTO grants(agent_name, kind, value, source) VALUES"
         " ('Alpha', 'tool', 'shell_exec', 'seed'), ('Alpha', 'host', 'api.example.com', 'seed');"
         "INSERT INTO extensions(name, path) VALUES ('ext1', '/x');"
         "INSERT INTO agent_extensions(agent_name, extension_name) VALUES ('Alpha', 'ext1');"
         "INSERT INTO memory_blocks(id, created_by, label) VALUES (5, 'Alpha', 'persona'),"
         " (6, NULL, 'shared'), (8, NULL, 'persona');"
         "INSERT INTO agent_memory_blocks(agent_name, block_id, read_only) VALUES"
         " ('Alpha', 5, 0), ('Alpha', 6, 1);"
         "INSERT INTO channels(name, status) VALUES ('discord', 'active');");
    exec("INSERT INTO sessions(id, name, agent_name) VALUES (1, 's1', 'Alpha'), (2, 's2', 'Beta'),"
         " (3, 's3', 'Alpha');"
         "INSERT INTO sessions(id, name, agent_name, parent_session_id) VALUES (4, 'sub', 'Alpha', 1);"
         "INSERT INTO channel_chats(channel_name, chat_id, session_id, kind, title) VALUES"
         " ('discord', 'dm-a', 1, 'dm', 'DM with a'), ('discord', 'g-b', 2, 'group', '#general'),"
         " ('discord', 'new-c', NULL, 'group', '#random');"
         "UPDATE sessions SET channel_name='discord', chat_id='dm-a' WHERE id=1;"
         "UPDATE sessions SET channel_name='discord', chat_id='g-b' WHERE id=2;");
}

/* ── tests ───────────────────────────────────────────────────────────────── */

static void test_dispatch(void) {
    CASES({ 400, "not json", NULL },
          { 400, "[1,2]", NULL },
          { 400, "{\"no\":\"action\"}", NULL },
          { 404, "{\"action\":\"drop_table\"}", "no action" },
          { 400, GA("Alpha", "\"knd\":\"tool\""), "knd" });
}

static void test_grants(void) {
    has(act(200, GA("Alpha", "\"kind\":\"tool\",\"value\":\"web_fetch\"")), "freely");
    assert(qeq("SELECT source || '|' || approval_mode || '|' || COALESCE(approval_id, '-') FROM grants"
               " WHERE agent_name='Alpha' AND value='web_fetch'", "operator|silent|-"));
    assert(logged("grants", "dashboard|act:grant_add|insert"));
    /* A tool on agent_approval_tools starts out asking, as for a new agent. */
    has(act(200, GA("Alpha", "\"kind\":\"tool\",\"value\":\"extension_publish\"")), "asks");
    /* The mode of an existing tool grant can change; its origin stays. */
    act(200, GA("Alpha", "\"kind\":\"tool\",\"value\":\"shell_exec\",\"approval_mode\":\"always\""));
    assert(qeq("SELECT source || '|' || approval_mode FROM grants WHERE agent_name='Alpha'"
               " AND value='shell_exec'", "seed|always"));
    act(200, GA("Beta", "\"kind\":\"host\",\"value\":\".github.com\",\"expires_at\":4102444800"));
    assert(qi("SELECT expires_at FROM grants WHERE agent_name='Beta' AND value='.github.com'") == 4102444800LL);
    CASES({ 409, GA("Alpha", "\"kind\":\"host\",\"value\":\"api.example.com\""), "already" },
          { 404, GA("Nobody", "\"kind\":\"tool\",\"value\":\"web_fetch\""), "agent" },
          { 400, GA("Alpha", "\"kind\":\"root\",\"value\":\"x\""), "kind" },
          { 400, GA("Alpha", "\"kind\":\"tool\",\"value\":\"rm_rf\""), "no tool" },
          { 400, GA("Alpha", "\"kind\":\"secret\",\"value\":\"NO_SUCH_SECRET\""), "no secret called" },
          { 400, GA("Alpha", "\"kind\":\"host\",\"value\":\"https://x.example/path\""), "host" },
          { 400, GA("Alpha", "\"kind\":\"read_path\",\"value\":\"rel/dir\""), "absolute" });
    /* A path grant that would expose the database is refused at grant time. */
    char cwd[1024], j[1400];
    assert(getcwd(cwd, sizeof(cwd)));
    snprintf(j, sizeof(j), GA("Alpha", "\"kind\":\"read_path\",\"value\":\"%s\""), cwd);
    has(act(400, j), "cclaw.db");
    assert(qi("SELECT count(*) FROM grants WHERE kind='read_path'") == 0);
    exec("INSERT INTO secrets(name, value, scope) VALUES ('PROV_KEY', 'enc:00', 'system')");
    CASES({ 400, GA("Alpha", "\"kind\":\"secret\",\"value\":\"PROV_KEY\""), "system" },
          { 400, GA("Alpha", "\"kind\":\"secret\",\"value\":\"lower\""), "secret name" },
          { 400, GA("Alpha", "\"kind\":\"host\",\"value\":\"a.example\",\"approval_mode\":\"always\""), "tool" },
          { 400, GA("Alpha", "\"kind\":\"tool\",\"value\":\"web_fetch\",\"approval_mode\":\"sometimes\""),
            "tool_decides" },
          { 400, GA("Alpha", "\"kind\":\"host\",\"value\":\"b.example\",\"expires_at\":1000"), "past" });
    /* Remove. */
    act(200, "{\"action\":\"grant_remove\",\"agent\":\"Alpha\",\"kind\":\"tool\",\"value\":\"web_fetch\"}");
    assert(qi("SELECT count(*) FROM grants WHERE agent_name='Alpha' AND value='web_fetch'") == 0);
    assert(logged("grants", "dashboard|act:grant_remove|delete"));
    act(404, "{\"action\":\"grant_remove\",\"agent\":\"Alpha\",\"kind\":\"tool\",\"value\":\"web_fetch\"}");
}

static void test_agent_copy(void) {
    act(200, "{\"action\":\"agent_copy\",\"from\":\"Alpha\",\"name\":\"AlphaTwo\","
             "\"description\":\"a copy\",\"attach_memory\":true}");
    /* The row, with created_by NULL (the operator made it). */
    assert(qeq("SELECT description || '|' || COALESCE(created_by, 'operator') FROM agents"
               " WHERE name='AlphaTwo'", "a copy|operator"));
    /* Grants: the same set, each a new row of this creation. */
    assert(qi("SELECT count(*) FROM grants WHERE agent_name='AlphaTwo'") ==
           qi("SELECT count(*) FROM grants WHERE agent_name='Alpha'"));
    assert(qi("SELECT count(*) FROM grants WHERE agent_name='AlphaTwo' AND source <> 'agent_create'") == 0);
    /* Model lists for every purpose, effort included. */
    assert(qeq("SELECT group_concat(purpose || ':' || model_id || ':' || COALESCE(reasoning_effort, '-'), ',')"
               " FROM (SELECT * FROM agent_models WHERE agent_name='AlphaTwo' ORDER BY purpose, pos)",
               "chat:mock/a:high,image:mock/b:-"));
    assert(qi("SELECT count(*) FROM agent_extensions WHERE agent_name='AlphaTwo' AND extension_name='ext1'") == 1);
    /* Memory: shared read-only, never writable, never copied. */
    assert(qeq("SELECT group_concat(block_id || ':' || read_only, ',') FROM (SELECT * FROM agent_memory_blocks"
               " WHERE agent_name='AlphaTwo' ORDER BY block_id)", "5:1,6:1"));
    assert(qi("SELECT count(*) FROM memory_blocks") == 3);
    assert(qi("SELECT count(*) FROM change_log WHERE table_name='agents' AND action='insert'"
              " AND actor='dashboard' AND context='act:agent_copy'"
              " AND row_key LIKE '%AlphaTwo%'") == 1);
    /* Without memory. */
    act(200, "{\"action\":\"agent_copy\",\"from\":\"Alpha\",\"name\":\"AlphaThree\"}");
    assert(qi("SELECT count(*) FROM agent_memory_blocks WHERE agent_name='AlphaThree'") == 0);
    CASES({ 400, "{\"action\":\"agent_copy\",\"from\":\"Alpha\",\"name\":\"bad name\"}", "PascalCase" },
          { 409, "{\"action\":\"agent_copy\",\"from\":\"Alpha\",\"name\":\"Beta\"}", "already" },
          { 404, "{\"action\":\"agent_copy\",\"from\":\"Ghost\",\"name\":\"Ghost2\"}", "agent" });
}

/* A per-entry setting on Beta's chat list: set on mock/a; an entry that
 * leaves it out keeps it (a page that does not show it cannot reset it);
 * null clears it. Leaves the list as mock/c, mock/a. */
static void keep_then_clear(const char *field, const char *value, const char *set, const char *cleared) {
    char j[384], sql[256];
    snprintf(sql, sizeof(sql), "SELECT COALESCE(%s, '-') FROM agent_models WHERE agent_name='Beta'"
             " AND purpose='chat' AND model_id='mock/a'", field);
    snprintf(j, sizeof(j), MS("chat", "{\"model_id\":\"mock/a\",\"%s\":%s},{\"model_id\":\"mock/c\"}"),
             field, value);
    act(200, j);
    act(200, MS("chat", "{\"model_id\":\"mock/c\"},{\"model_id\":\"mock/a\"}"));
    assert(qeq(sql, set));
    snprintf(j, sizeof(j), MS("chat", "{\"model_id\":\"mock/c\"},{\"model_id\":\"mock/a\",\"%s\":null}"),
             field);
    act(200, j);
    assert(qeq(sql, cleared));
}

static void test_models(void) {
    static const char LIST[] = "SELECT group_concat(model_id || ':' || COALESCE(reasoning_effort, '-'), ',')"
        " FROM (SELECT * FROM agent_models WHERE agent_name='Beta' AND purpose='%s' ORDER BY pos)";
    char sql[256];
    act(200, MS("chat", "{\"model_id\":\"mock/c\"},{\"model_id\":\"mock/a\",\"reasoning_effort\":\"medium\"}"));
    snprintf(sql, sizeof(sql), LIST, "chat");
    assert(qeq(sql, "mock/c:-,mock/a:medium"));
    assert(logged("agent_models", "dashboard|act:models_set|insert"));
    has(act(200, MS("chat", "{\"model_id\":\"mock/c\"},{\"model_id\":\"mock/a\",\"reasoning_effort\":\"medium\"}")),
        "Nothing changed");
    act(200, MS("image", "{\"model_id\":\"mock/b\"}"));
    has(act(400, MS("image", "{\"model_id\":\"mock/c\"}")), "image");
    assert(qeq("SELECT model_id FROM agent_models WHERE agent_name='Beta' AND purpose='image'", "mock/b"));
    has(act(200, MS("image", "")), "capability");
    CASES({ 400, MS("chat", ""), "chat model" },
          { 400, MS("chat", "{\"model_id\":\"mock/a\"},{\"model_id\":\"mock/a\"}"), "twice" },
          { 400, MS("chat", "{\"model_id\":\"nope/x\"}"), "no model" },
          { 400, MS("image", "{\"model_id\":\"mock/b\",\"reasoning_effort\":\"low\"}"), "utility" },
          { 400, MS("chat", "{\"model_id\":\"mock/a\",\"reasoning_effort\":\"max\"}"), "effort" },
          { 400, MS("video", ""), "utility" });
    /* Utility: any model (mock/c has no capabilities), effort allowed, and
     * an empty list is legal — scripts then get the chat list. */
    act(200, MS("utility", "{\"model_id\":\"mock/c\",\"reasoning_effort\":\"low\"},{\"model_id\":\"mock/b\"}"));
    snprintf(sql, sizeof(sql), LIST, "utility");
    assert(qeq(sql, "mock/c:low,mock/b:-"));
    has(act(200, MS("utility", "")), "chat list");
    assert(qi("SELECT count(*) FROM agent_models WHERE agent_name='Beta' AND purpose='utility'") == 0);
    snprintf(sql, sizeof(sql), LIST, "chat");
    assert(qeq(sql, "mock/c:-,mock/a:medium"));

    /* Trim numbers, cache lifetime and reasoning display: chat list only. */
    keep_then_clear("trim_threshold", "9000", "9000", "-");
    keep_then_clear("prune_target", "3000", "3000", "-");
    keep_then_clear("cache_lifetime", "600", "600", "-");
    assert(qeq("SELECT reasoning_display FROM agent_models WHERE agent_name='Beta' AND model_id='mock/a'",
               "summary"));
    keep_then_clear("reasoning_display", "\"none\"", "none", "summary");
    assert(qeq("SELECT reasoning_display FROM agent_models WHERE agent_name='Beta' AND model_id='mock/c'",
               "summary"));
    /* An empty threshold is half the window (mock/a has none of its own:
     * the default 128000), and the target must be below that too. */
    CASES({ 400, MS("chat", "{\"model_id\":\"mock/a\",\"trim_threshold\":0}"), "token" },
          { 400, MS("chat", "{\"model_id\":\"mock/a\",\"trim_threshold\":100,\"prune_target\":200}"), "trim" },
          { 400, MS("image", "{\"model_id\":\"mock/b\",\"trim_threshold\":100}"), "chat list" },
          { 400, MS("chat", "{\"model_id\":\"mock/a\",\"trim_threshold\":null,\"prune_target\":64000}"), "64000" },
          { 400, MS("chat", "{\"model_id\":\"mock/a\",\"cache_lifetime\":0}"), "seconds" },
          { 400, MS("image", "{\"model_id\":\"mock/b\",\"cache_lifetime\":300}"), "chat list" },
          { 400, MS("chat", "{\"model_id\":\"mock/a\",\"reasoning_display\":\"full\"}"), "summary" },
          { 400, MS("utility", "{\"model_id\":\"mock/a\",\"reasoning_display\":\"none\"}"), "chat list" });
    exec("UPDATE models SET context_window = 10000 WHERE id = 'mock/a';");
    has(act(400, MS("chat", "{\"model_id\":\"mock/a\",\"trim_threshold\":null,\"prune_target\":6000}")), "5000");
    exec("UPDATE models SET context_window = NULL WHERE id = 'mock/a';");
    /* On an Anthropic model only 300 or 3600, and 3600 when a new entry
     * says nothing. */
    exec("INSERT INTO providers(name, base_url, endpoint_type) VALUES ('cachetest','http://x','anthropic');"
         "INSERT INTO models(id, provider_name, model) VALUES ('cachetest/c','cachetest','c');");
    has(act(400, MS("chat", "{\"model_id\":\"cachetest/c\",\"cache_lifetime\":600}")), "300");
    act(200, MS("chat", "{\"model_id\":\"mock/c\"},{\"model_id\":\"cachetest/c\"}"));
    assert(qeq("SELECT cache_lifetime FROM agent_models WHERE agent_name='Beta' AND model_id='cachetest/c'",
               "3600"));
    act(200, MS("chat", "{\"model_id\":\"mock/c\"},{\"model_id\":\"cachetest/c\",\"cache_lifetime\":300}"));
    assert(qeq("SELECT cache_lifetime FROM agent_models WHERE agent_name='Beta' AND model_id='cachetest/c'",
               "300"));
    act(200, MS("chat", "{\"model_id\":\"mock/c\"},{\"model_id\":\"mock/a\"}"));
}

/* A hook three failures turned off comes back on with its count at zero;
 * one that is on, or that does not exist, is refused in words. */
static void test_hooks(void) {
    exec("INSERT INTO hooks(extension_name, event, path, fail_count, disabled) VALUES"
         " ('ext1', 'ContextThreshold', '/x/compact.qjs', 3, 1),"
         " ('ext1', 'Stop', '/x/review.qjs', 1, 0);");
    act(200, "{\"action\":\"hook_enable\",\"extension\":\"ext1\",\"event\":\"ContextThreshold\","
             "\"path\":\"/x/compact.qjs\"}");
    assert(qeq("SELECT disabled || ':' || fail_count FROM hooks WHERE event='ContextThreshold'", "0:0"));
    CASES({ 409, "{\"action\":\"hook_enable\",\"extension\":\"ext1\",\"event\":\"ContextThreshold\","
                 "\"path\":\"/x/compact.qjs\"}", "not turned off" },
          { 409, "{\"action\":\"hook_enable\",\"extension\":\"ext1\",\"event\":\"Stop\","
                 "\"path\":\"/x/review.qjs\"}", "not turned off" });
    assert(qeq("SELECT fail_count FROM hooks WHERE event='Stop'", "1"));
    CASES({ 404, "{\"action\":\"hook_enable\",\"extension\":\"ext1\",\"event\":\"CacheExpiring\","
                 "\"path\":\"/x/compact.qjs\"}", "CacheExpiring" },
          { 404, "{\"action\":\"hook_enable\",\"extension\":\"nope\",\"event\":\"Stop\","
                 "\"path\":\"/x/review.qjs\"}", "extension" },
          { 400, "{\"action\":\"hook_enable\",\"extension\":\"ext1\"}", "hook" },
          { 400, "{\"action\":\"hook_enable\",\"extension\":\"ext1\",\"event\":\"Stop\","
                 "\"path\":\"/x/review.qjs\",\"fail_count\":0}", "field" });
}

#define CHAT(rest) "{\"action\":\"chat_settings\",\"channel\":\"discord\",\"chat_id\":\"new-c\"" rest "}"

static void test_chats(void) {
    /* An unrouted chat gets a fresh session. */
    has(act(200, "{\"action\":\"session_new_on_chat\",\"agent\":\"Beta\",\"channel\":\"discord\","
                 "\"chat_id\":\"new-c\"}"), "#random now talks to Beta");
    long long c_sid = qi("SELECT session_id FROM channel_chats WHERE chat_id='new-c'");
    assert(c_sid > 4 && qeq("SELECT agent_name FROM sessions WHERE id=(SELECT session_id FROM"
                            " channel_chats WHERE chat_id='new-c')", "Beta"));
    assert(logged("channel_chats", "dashboard|act:session_new_on_chat|update"));
    /* Same agent, pinned chat: the old session is left behind, still naming it. */
    has(act(200, "{\"action\":\"session_new_on_chat\",\"agent\":\"Alpha\",\"channel\":\"discord\","
                 "\"chat_id\":\"dm-a\"}"), "left behind");
    assert(qi("SELECT session_id FROM channel_chats WHERE chat_id='dm-a'") > c_sid);
    assert(qeq("SELECT channel_name || ':' || chat_id FROM sessions WHERE id=1", "discord:dm-a"));
    /* Another agent's chat: refused until the receiving agent is typed. */
    const char *r = act(409, "{\"action\":\"session_new_on_chat\",\"agent\":\"Alpha\",\"channel\":\"discord\","
                             "\"chat_id\":\"g-b\"}");
    has(r, "\"confirm\":{\"kind\":\"handover\",\"from\":\"Beta\",\"to\":\"Alpha\"");
    assert(qi("SELECT session_id FROM channel_chats WHERE chat_id='g-b'") == 2);
    act(409, "{\"action\":\"session_new_on_chat\",\"agent\":\"Alpha\",\"channel\":\"discord\","
             "\"chat_id\":\"g-b\",\"confirm_handover\":\"Beta\"}");
    act(200, "{\"action\":\"session_new_on_chat\",\"agent\":\"Alpha\",\"channel\":\"discord\","
             "\"chat_id\":\"g-b\",\"confirm_handover\":\"Alpha\"}");
    assert(qeq("SELECT s.agent_name FROM channel_chats c JOIN sessions s ON s.id=c.session_id"
               " WHERE c.chat_id='g-b'", "Alpha"));
    act(404, "{\"action\":\"session_new_on_chat\",\"agent\":\"Alpha\",\"channel\":\"discord\","
             "\"chat_id\":\"never\"}");
    /* Move session 3 (Alpha, no chat) to dm-a: dm-a's session is left behind. */
    long long dm_old = qi("SELECT session_id FROM channel_chats WHERE chat_id='dm-a'");
    act(200, "{\"action\":\"session_move_to_chat\",\"session_id\":3,\"channel\":\"discord\","
             "\"chat_id\":\"dm-a\"}");
    assert(qi("SELECT session_id FROM channel_chats WHERE chat_id='dm-a'") == 3);
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT chat_id FROM sessions WHERE id=%lld", dm_old);
    assert(qeq(sql, "dm-a"));
    /* Move it on to new-c (Beta's): needs the handover; dm-a gets a fresh Alpha session. */
    act(409, "{\"action\":\"session_move_to_chat\",\"session_id\":3,\"channel\":\"discord\","
             "\"chat_id\":\"new-c\"}");
    act(200, "{\"action\":\"session_move_to_chat\",\"session_id\":3,\"channel\":\"discord\","
             "\"chat_id\":\"new-c\",\"confirm_handover\":\"Alpha\"}");
    assert(qeq("SELECT s.agent_name FROM channel_chats c JOIN sessions s ON s.id=c.session_id"
               " WHERE c.chat_id='dm-a'", "Alpha"));
    assert(qi("SELECT session_id FROM channel_chats WHERE chat_id='dm-a'") > 3);
    CASES({ 409, "{\"action\":\"session_move_to_chat\",\"session_id\":3,\"channel\":\"discord\","
                 "\"chat_id\":\"new-c\"}", "already" },
          { 400, "{\"action\":\"session_move_to_chat\",\"session_id\":4,\"channel\":\"discord\","
                 "\"chat_id\":\"new-c\"}", "sub-agent" },
          { 404, "{\"action\":\"session_move_to_chat\",\"session_id\":999,\"channel\":\"discord\","
                 "\"chat_id\":\"new-c\"}", "999" });
    /* Chat settings. */
    act(200, CHAT(",\"delivery_mode\":\"auto\",\"tool_filter\":[\"web_fetch\"],\"guidance\":\"Be brief.\""));
    assert(qeq("SELECT delivery_mode || '|' || tool_filter || '|' || guidance FROM channel_chats"
               " WHERE chat_id='new-c'", "auto|[\"web_fetch\"]|Be brief."));
    assert(logged("channel_chats", "dashboard|act:chat_settings|update"));
    act(200, CHAT(",\"tool_filter\":\"unrestricted\",\"guidance\":\"\""));
    assert(qi("SELECT tool_filter IS NULL AND guidance IS NULL FROM channel_chats WHERE chat_id='new-c'") == 1);
    CASES({ 400, CHAT(",\"tool_filter\":[]"), "no tools" },
          { 400, CHAT(",\"tool_filter\":[\"nope\"]"), "no tool called" },
          { 400, CHAT(",\"delivery_mode\":\"loud\""), "auto" },
          { 400, CHAT(""), NULL });
    /* Unroute. */
    act(200, "{\"action\":\"chat_unroute\",\"channel\":\"discord\",\"chat_id\":\"new-c\"}");
    assert(qi("SELECT session_id IS NULL AND notified_at IS NULL FROM channel_chats WHERE chat_id='new-c'") == 1);
    act(409, "{\"action\":\"chat_unroute\",\"channel\":\"discord\",\"chat_id\":\"new-c\"}");
    /* Channel defaults: the open door, and closing it. */
    act(200, "{\"action\":\"channel_defaults\",\"channel\":\"discord\",\"default_agent\":\"Beta\","
             "\"default_tool_filter\":[\"web_fetch\"]}");
    assert(qeq("SELECT default_agent || '|' || default_tool_filter FROM channels WHERE name='discord'",
               "Beta|[\"web_fetch\"]"));
    has(act(200, "{\"action\":\"chat_unroute\",\"channel\":\"discord\",\"chat_id\":\"dm-a\"}"),
        "default agent, Beta");
    act(200, "{\"action\":\"channel_defaults\",\"channel\":\"discord\",\"default_agent\":null,"
             "\"default_tool_filter\":\"unrestricted\"}");
    assert(qi("SELECT default_agent IS NULL AND default_tool_filter IS NULL FROM channels") == 1);
    CASES({ 404, "{\"action\":\"channel_defaults\",\"channel\":\"discord\",\"default_agent\":\"Ghost\"}", "agent" },
          { 404, "{\"action\":\"channel_defaults\",\"channel\":\"irc\",\"default_agent\":\"Beta\"}", "channel" });
}

#define PA(name, url, rest) "{\"action\":\"provider_add\",\"name\":\"" name "\",\"base_url\":\"" url "\"" rest "}"
#define MOCK "http://127.0.0.1:19893"

static void test_providers(void) {
    /* A good key: key stored as a system secret, provider row, its catalog. */
    has(act(200, PA("good", MOCK "/ok", ",\"endpoint_type\":\"openai\",\"api_key\":\"" GOODKEY "\"")),
        "GOOD_API_KEY");
    assert(qeq("SELECT api_key_env || '|' || endpoint_type FROM providers WHERE name='good'",
               "GOOD_API_KEY|openai"));
    char *k = db_secret_get_system(g_db, "GOOD_API_KEY");
    assert(k && strcmp(k, GOODKEY) == 0);
    free(k);
    assert(qi("SELECT count(*) FROM models_cache WHERE provider_name='good'") == 3);
    assert(logged("providers", "dashboard|act:provider_add|insert"));
    assert(logged("secrets", "dashboard|act:provider_add|insert"));
    /* Gemini and Anthropic authenticate their own way, and Gemini's shape. */
    act(200, PA("gem", MOCK "/gem", ",\"endpoint_type\":\"gemini\",\"api_key\":\"" GOODKEY "\""));
    assert(qeq("SELECT group_concat(id || ':' || COALESCE(context_length, '-'), ',') FROM"
               " (SELECT * FROM models_cache WHERE provider_name='gem' ORDER BY id)", "gem-a:1048576,gem-b:-"));
    act(200, PA("anth", MOCK "/anth", ",\"endpoint_type\":\"anthropic\",\"api_key\":\"" GOODKEY "\""));
    /* A bad key, a wrong format and nothing listening: nothing is written. */
    long long logs = qi("SELECT count(*) FROM change_log");
    CASES({ 400, PA("bad", MOCK "/ok", ",\"endpoint_type\":\"openai\",\"api_key\":\"wrong\""), "401" },
          { 400, PA("bad", MOCK "/ok", ",\"endpoint_type\":\"gemini\",\"api_key\":\"" GOODKEY "\""), "Nothing" },
          { 400, PA("bad", "http://127.0.0.1:1/v1", ",\"endpoint_type\":\"openai\",\"api_key\":\"" GOODKEY "\""),
            "reach" });
    assert(qi("SELECT (SELECT count(*) FROM providers WHERE name='bad') + (SELECT count(*) FROM secrets"
              " WHERE name='BAD_API_KEY') + (SELECT count(*) FROM models_cache WHERE provider_name='bad')") == 0);
    assert(qi("SELECT count(*) FROM change_log") == logs);
    /* Keyless (a gateway on the network). */
    act(200, PA("local", MOCK "/open", ",\"endpoint_type\":\"openai\""));
    assert(qeq("SELECT '[' || api_key_env || ']' FROM providers WHERE name='local'", "[]"));
    exec("INSERT INTO secrets(name, value, scope) VALUES ('CLASH_API_KEY', 'enc:00', 'agent')");
    CASES({ 409, PA("good", MOCK "/ok", ",\"endpoint_type\":\"openai\""), "already" },
          { 400, PA("p2", "ftp://x", ",\"endpoint_type\":\"openai\""), "endpoint" },
          { 400, PA("p2", MOCK "/ok", ",\"endpoint_type\":\"responses\""), "wire format" },
          { 400, PA("2bad", MOCK "/ok", ",\"endpoint_type\":\"openai\""), "letter" },
          { 409, PA("clash", MOCK "/ok", ",\"endpoint_type\":\"openai\",\"api_key\":\"" GOODKEY "\""), "exists" });
    /* Edit: request_extra, a new key checked first. */
    act(200, "{\"action\":\"provider_edit\",\"name\":\"good\",\"request_extra\":\"{\\\"x\\\":{\\\"y\\\":1}}\"}");
    assert(qeq("SELECT request_extra FROM providers WHERE name='good'", "{\"x\":{\"y\":1}}"));
    assert(logged("providers", "dashboard|act:provider_edit|update"));
    has(act(400, "{\"action\":\"provider_edit\",\"name\":\"good\",\"request_extra\":\"[1]\"}"), "object");
    act(200, "{\"action\":\"provider_edit\",\"name\":\"good\",\"request_extra\":\"\"}");
    assert(qi("SELECT request_extra IS NULL FROM providers WHERE name='good'") == 1);
    has(act(400, "{\"action\":\"provider_edit\",\"name\":\"good\",\"api_key\":\"wrong-key\"}"), "Nothing");
    k = db_secret_get_system(g_db, "GOOD_API_KEY");
    assert(k && strcmp(k, GOODKEY) == 0);
    free(k);
    act(200, "{\"action\":\"provider_edit\",\"name\":\"good\",\"api_key\":\"" GOODKEY "\"}");
    has(act(400, "{\"action\":\"provider_edit\",\"name\":\"good\",\"base_url\":\"http://127.0.0.1:1/x\"}"),
        "reach");
    assert(qeq("SELECT base_url FROM providers WHERE name='good'", MOCK "/ok"));
    CASES({ 400, "{\"action\":\"provider_edit\",\"name\":\"good\"}", NULL },
          { 404, "{\"action\":\"provider_edit\",\"name\":\"ghost\",\"base_url\":\"http://a.example/v1\"}",
            "provider" });
    /* Refresh: good, then broken (the previous list is kept). */
    exec("DELETE FROM models_cache WHERE provider_name='good'");
    has(act(200, "{\"action\":\"provider_refresh_models\",\"name\":\"good\"}"), "3 models");
    exec("UPDATE providers SET base_url='http://127.0.0.1:1/x' WHERE name='good'");
    act(502, "{\"action\":\"provider_refresh_models\",\"name\":\"good\"}");
    assert(qi("SELECT count(*) FROM models_cache WHERE provider_name='good'") == 3);
    exec("UPDATE providers SET base_url='http://127.0.0.1:19893/ok' WHERE name='good'");
    /* Import, prefilled by the page from the cache; the operator's edit wins. */
    act(200, "{\"action\":\"model_import\",\"provider\":\"good\",\"model\":\"vendor/seer\","
             "\"context_window\":128000,\"capabilities\":[\"text\",\"image\"]}");
    assert(qeq("SELECT provider_name || '|' || model || '|' || context_window || '|' || capabilities"
               " FROM models WHERE id='good/vendor/seer'", "good|vendor/seer|128000|[\"text\",\"image\"]"));
    assert(logged("models", "dashboard|act:model_import|insert"));
    CASES({ 409, "{\"action\":\"model_import\",\"provider\":\"good\",\"model\":\"vendor/seer\"}", "already" },
          { 400, "{\"action\":\"model_import\",\"provider\":\"good\",\"model\":\"x\",\"capabilities\":\"image\"}",
            "list" },
          { 400, "{\"action\":\"model_import\",\"provider\":\"good\",\"model\":\"x\",\"context_window\":-5}",
            "whole number" },
          { 404, "{\"action\":\"model_import\",\"provider\":\"ghost\",\"model\":\"x\"}", NULL });
    /* Edit a model. */
    act(200, "{\"action\":\"model_edit\",\"id\":\"good/vendor/seer\",\"context_window\":200000,"
             "\"max_output_tokens\":8192,\"effort_map\":{\"format\":\"openai\",\"levels\":{\"low\":\"low\","
             "\"high\":\"high\",\"off\":null}}}");
    assert(qeq("SELECT context_window || '|' || max_output_tokens || '|' || (effort_map ->> '$.format')"
               " FROM models WHERE id='good/vendor/seer'", "200000|8192|openai"));
    CASES({ 400, "{\"action\":\"model_edit\",\"id\":\"good/vendor/seer\",\"effort_map\":{\"format\":\"x\"}}",
            "effort map" },
          { 400, "{\"action\":\"model_edit\",\"id\":\"good/vendor/seer\",\"effort_map\":"
                 "{\"format\":\"openai\",\"levels\":{\"turbo\":1}}}", "effort map" });
    act(200, "{\"action\":\"model_edit\",\"id\":\"good/vendor/seer\",\"effort_map\":\"\",\"context_window\":null}");
    assert(qi("SELECT effort_map IS NULL AND context_window IS NULL FROM models WHERE id='good/vendor/seer'") == 1);
    /* Taking a capability away from a model on a media list is refused. */
    has(act(409, "{\"action\":\"model_edit\",\"id\":\"mock/b\",\"capabilities\":[\"text\"]}"), "Alpha (image)");
    /* Disabling the only chat model of an agent warns. */
    has(act(200, "{\"action\":\"model_edit\",\"id\":\"mock/a\",\"enabled\":false}"), "only enabled chat model");
    assert(qeq("SELECT status FROM models WHERE id='mock/a'", "disabled"));
    act(200, "{\"action\":\"model_edit\",\"id\":\"mock/a\",\"enabled\":true}");
    assert(qeq("SELECT status FROM models WHERE id='mock/a'", "healthy"));
    /* Remove a model, a provider. */
    has(act(409, "{\"action\":\"model_remove\",\"id\":\"mock/a\"}"), "Alpha (chat)");
    act(200, "{\"action\":\"model_remove\",\"id\":\"good/vendor/seer\"}");
    assert(qi("SELECT count(*) FROM models WHERE id='good/vendor/seer'") == 0);
    has(act(409, "{\"action\":\"provider_remove\",\"name\":\"mock\"}"), "3 models");
    has(act(200, "{\"action\":\"provider_remove\",\"name\":\"good\"}"), "still stored");
    assert(qi("SELECT (SELECT count(*) FROM providers WHERE name='good') + (SELECT count(*) FROM models_cache"
              " WHERE provider_name='good')") == 0);
}

static void test_memory(void) {
    has(act(200, "{\"action\":\"memory_block_create\",\"label\":\"facts\",\"description\":\"d\","
                 "\"placement\":\"system\",\"agent\":\"Beta\"}"), "Beta");
    long long id = qi("SELECT id FROM memory_blocks WHERE label='facts'");
    assert(qi("SELECT read_only FROM agent_memory_blocks WHERE agent_name='Beta' AND block_id="
              "(SELECT id FROM memory_blocks WHERE label='facts')") == 0);
    assert(qi("SELECT created_by IS NULL FROM memory_blocks WHERE label='facts'") == 1);
    CASES({ 200, "{\"action\":\"memory_block_create\",\"label\":\"loose\"}", "No agent" },
          { 400, "{\"action\":\"memory_block_create\",\"label\":\"has space\"}", "spaces" },
          { 409, "{\"action\":\"memory_block_create\",\"label\":\"facts\",\"agent\":\"Beta\"}", "already" },
          { 400, "{\"action\":\"memory_block_create\",\"label\":\"x\",\"placement\":\"top\"}", "lacement" });
    char j[512];
    snprintf(j, sizeof(j), "{\"action\":\"memory_block_edit\",\"block_id\":%lld,\"description\":\"new\","
             "\"char_limit\":40,\"placement\":\"context\"}", id);
    act(200, j);
    snprintf(j, sizeof(j), "SELECT description || '|' || char_limit || '|' || placement FROM memory_blocks"
             " WHERE id=%lld", id);
    assert(qeq(j, "new|40|context"));
    act(404, "{\"action\":\"memory_block_edit\",\"block_id\":999,\"description\":\"x\"}");
    /* Attach: read-only by default; a label clash is refused with its name. */
    snprintf(j, sizeof(j), "{\"action\":\"memory_attach\",\"block_id\":%lld,\"agent\":\"Alpha\"}", id);
    has(act(200, j), "read");
    has(act(409, j), "read only");
    has(act(409, "{\"action\":\"memory_attach\",\"block_id\":8,\"agent\":\"Alpha\"}"), "persona");
    snprintf(j, sizeof(j), "{\"action\":\"memory_attach\",\"block_id\":%lld,\"agent\":\"Alpha\","
             "\"read_only\":false}", id);
    act(200, j);
    snprintf(j, sizeof(j), "{\"action\":\"memory_detach\",\"block_id\":%lld,\"agent\":\"Alpha\"}", id);
    act(200, j);
    act(404, j);
    /* Notes. */
    snprintf(j, sizeof(j), "{\"action\":\"memory_note_add\",\"block_id\":%lld,\"text\":\"short\"}", id);
    act(200, j);
    snprintf(j, sizeof(j), "{\"action\":\"memory_note_add\",\"block_id\":%lld,\"text\":\"%s\"}", id,
             "a note that is quite a lot longer than forty characters");
    has(act(200, j), "40");
    long long note = qi("SELECT MIN(id) FROM memory_notes");
    snprintf(j, sizeof(j), "{\"action\":\"memory_note_edit\",\"block_id\":%lld,\"note_id\":%lld,"
             "\"text\":\"edited\"}", id, note);
    act(200, j);
    snprintf(j, sizeof(j), "{\"action\":\"memory_note_add\",\"block_id\":%lld,\"text\":\"  \"}", id);
    has(act(400, j), "empty");
    snprintf(j, sizeof(j), "{\"action\":\"memory_note_delete\",\"block_id\":%lld,\"note_id\":%lld}", id, note);
    act(200, j);
    act(404, j);
    snprintf(j, sizeof(j), "{\"action\":\"memory_block_delete\",\"block_id\":%lld}", id);
    has(act(200, j), "Beta");
    snprintf(j, sizeof(j), "SELECT count(*) FROM memory_notes WHERE block_id=%lld", id);
    assert(qi(j) == 0);
}

static void test_approvals(void) {
    exec("INSERT INTO approvals(id, session_id, tool_name, park_reason, args_json, state) VALUES"
         " (30, 2, 'shell_exec', 'approval_required', '{\"command\":\"ls -la\"}', 'pending'),"
         " (31, 2, 'request_config', 'approval_required',"
         "  '{\"changes\":{\"grants\":{\"hosts\":[\"hist.example\"]}},\"reason\":\"need it\"}', 'denied'),"
         " (32, 2, 'shell_exec', 'approval_required', '{}', 'denied');");
    char *card = NULL;
    assert(explore_approval_card(g_db, "31", &card) == 200);
    collect(card);
    assert(strstr(card, "hist.example") && strstr(card, "\"state\":\"denied\""));
    free(card);
    assert(explore_approval_card(g_db, "999", &card) == 404);
    free(card);
    exec("INSERT INTO approvals(id, session_id, tool_name, park_reason, args_json, resolve, state) VALUES"
         " (33, 2, 'request_config', 'approval_required', '{}', 'apply', 'pending'),"
         " (34, 2, 'web_fetch', 'sensitive_target', '{}', 'rerun', 'pending');");
    has(act(400, "{\"action\":\"approval_decide\",\"approval_id\":33,\"decision\":\"once\"}"), "no \\\"once\\\"");
    has(act(200, "{\"action\":\"approval_decide\",\"approval_id\":33,\"decision\":\"always\"}"), "Approved.");
    has(act(400, "{\"action\":\"approval_decide\",\"approval_id\":34,\"decision\":\"always\"}"), "one call at a time");
    assert(explore_approval_card(g_db, "33", &card) == 200);
    assert(strstr(card, "\"resolve\":\"apply\""));
    free(card);
    has(act(200, "{\"action\":\"approval_decide\",\"approval_id\":30,\"decision\":\"once\"}"), "Approved once");
    assert(qeq("SELECT decision || '|' || decided_via FROM approval_decisions WHERE approval_id=30",
               "once|dashboard"));
    assert(qeq("SELECT state FROM approvals WHERE id=30", "pending"));
    has(act(409, "{\"action\":\"approval_decide\",\"approval_id\":30,\"decision\":\"deny\"}"), "already on its way");
    has(act(409, "{\"action\":\"approval_decide\",\"approval_id\":31,\"decision\":\"once\"}"), "denied already");
    act(400, "{\"action\":\"approval_decide\",\"approval_id\":30,\"decision\":\"maybe\"}");
    act(404, "{\"action\":\"approval_decide\",\"approval_id\":999,\"decision\":\"once\"}");
    has(act(200, "{\"action\":\"approval_grant_from_history\",\"approval_id\":31}"), "recorded as approval #");
    assert(qeq("SELECT source || '|' || (approval_id > 32) FROM grants WHERE agent_name='Beta'"
               " AND value='hist.example'", "approval|1"));
    assert(qeq("SELECT decided_via FROM approvals WHERE id=(SELECT approval_id FROM grants"
               " WHERE value='hist.example')", "dashboard"));
    assert(qeq("SELECT actor || '|' || substr(context, 1, 9) FROM change_log WHERE table_name='grants'"
               " ORDER BY id DESC LIMIT 1", "dashboard|approval:"));
    has(act(400, "{\"action\":\"approval_grant_from_history\",\"approval_id\":32}"), "Only a configuration");
    has(act(409, "{\"action\":\"approval_grant_from_history\",\"approval_id\":30}"), "was not denied");
}

static void test_secrets(void) {
    has(act(200, "{\"action\":\"secret_add\",\"name\":\"GH_TOKEN\",\"value\":\"tok-" MARK "\"}"),
        "until you grant it");
    assert(qeq("SELECT scope || '|' || source FROM secrets WHERE name='GH_TOKEN'", "agent|operator"));
    assert(qi("SELECT count(*) FROM grants WHERE kind='secret' AND value='GH_TOKEN'") == 0);
    assert(logged("secrets", "dashboard|act:secret_add|insert"));
    has(act(409, "{\"action\":\"secret_add\",\"name\":\"GH_TOKEN\",\"value\":\"x\"}"), "Replace value");
    has(act(200, "{\"action\":\"secret_add\",\"name\":\"GH_TOKEN\",\"value\":\"tok2-" MARK "\","
                 "\"replace\":true}"), "Replaced");
    has(act(409, "{\"action\":\"secret_add\",\"name\":\"GH_TOKEN\",\"value\":\"x\",\"replace\":true,"
                 "\"scope\":\"system\"}"), "change its scope");
    act(400, "{\"action\":\"secret_add\",\"name\":\"gh\",\"value\":\"x\"}");
    act(400, "{\"action\":\"secret_add\",\"name\":\"EMPTY\",\"value\":\"\"}");
    /* Grant and revoke it (the grants action, kind secret). */
    act(200, "{\"action\":\"grant_add\",\"agent\":\"Beta\",\"kind\":\"secret\",\"value\":\"GH_TOKEN\"}");
    assert(qeq("SELECT source FROM grants WHERE agent_name='Beta' AND kind='secret' AND value='GH_TOKEN'",
               "operator"));
    /* Hosts: set, replace, refuse. */
    act(200, "{\"action\":\"secret_hosts_set\",\"name\":\"GH_TOKEN\",\"hosts\":[\"api.github.com\",\".github.com\"]}");
    assert(qi("SELECT count(*) FROM secret_hosts WHERE secret_name='GH_TOKEN'") == 2);
    act(200, "{\"action\":\"secret_hosts_set\",\"name\":\"GH_TOKEN\",\"hosts\":[\"api.github.com\"]}");
    assert(qeq("SELECT group_concat(host) FROM secret_hosts WHERE secret_name='GH_TOKEN'", "api.github.com"));
    has(act(400, "{\"action\":\"secret_hosts_set\",\"name\":\"GH_TOKEN\",\"hosts\":[\"https://x\"]}"), "not a host");
    /* request_config's rule: no wildcard, no bare top-level domain. */
    has(act(400, "{\"action\":\"secret_hosts_set\",\"name\":\"GH_TOKEN\",\"hosts\":[\"*\"]}"), "not a host");
    has(act(400, "{\"action\":\"secret_hosts_set\",\"name\":\"GH_TOKEN\",\"hosts\":[\".com\"]}"), "not a host");
    assert(qeq("SELECT group_concat(host) FROM secret_hosts WHERE secret_name='GH_TOKEN'", "api.github.com"));
    has(act(400, "{\"action\":\"secret_hosts_set\",\"name\":\"GEM_API_KEY\",\"hosts\":[\"a.example\"]}"),
        "system secret");
    /* Remove: a provider's key is refused; otherwise everything goes with it. */
    has(act(409, "{\"action\":\"secret_remove\",\"name\":\"GEM_API_KEY\"}"), "API key of gem");
    has(act(200, "{\"action\":\"secret_remove\",\"name\":\"GH_TOKEN\"}"), "1 stored value, 1 allowed host, 1 grant");
    assert(qi("SELECT (SELECT count(*) FROM secrets WHERE name='GH_TOKEN') + (SELECT count(*) FROM"
              " secret_hosts WHERE secret_name='GH_TOKEN') + (SELECT count(*) FROM grants WHERE value='GH_TOKEN')") == 0);
    act(404, "{\"action\":\"secret_remove\",\"name\":\"NO_SUCH\"}");
}

/* The values submitted through actions (MARK, inside GOODKEY and the
 * secrets above) appear in no answer, no change_log row, and no explorer
 * read of any table. */
static void test_secret_never_leaves(void) {
    assert(g_all && !strstr(g_all, MARK));
    assert(qi("SELECT count(*) FROM change_log WHERE COALESCE(old_values, '') || COALESCE(new_values, '')"
              " || row_key LIKE '%" MARK "%'") == 0);
    /* The stored ciphertext is not the value either. */
    assert(qi("SELECT count(*) FROM secrets WHERE value LIKE '%" MARK "%'") == 0);
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(g_db, "SELECT name FROM pragma_table_list WHERE schema='main'"
                              " AND type='table' AND name NOT LIKE 'sqlite%'", -1, &st, NULL) == SQLITE_OK);
    int tables = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(st, 0);
        char *out = NULL;
        ExploreQuery q = { .table = t, .limit = 200 };
        int s = explore_rows(g_db, &q, &out);
        if (s == 200) { tables++; assert(!strstr(out, MARK)); }
        free(out);
    }
    sqlite3_finalize(st);
    char *meta = NULL;
    assert(explore_meta(g_db, &meta) == 200 && !strstr(meta, MARK));
    free(meta);
    assert(tables > 30);
    printf("    %d answers, %d tables\n", g_answers, tables);
}

int main(void) {
    TEST_INIT();
    test_db_clean(DB);
    g_db = test_db_open(DB);
    assert(g_db);
    db_set_actor(g_db, "dashboard");
    db_set_secret_key(TEST_KEY);
    unsetenv("GOOD_API_KEY");
    seed();
    pthread_t srv = server_start();
    RUN(test_dispatch);
    RUN(test_grants);
    RUN(test_agent_copy);
    RUN(test_models);
    RUN(test_hooks);
    RUN(test_chats);
    RUN(test_providers);
    RUN(test_memory);
    RUN(test_approvals);
    RUN(test_secrets);
    RUN(test_secret_never_leaves);
    g_stop = 1;
    pthread_join(srv, NULL);
    close(g_listen);
    free(g_all);
    db_close(g_db);
    test_db_clean(DB);
    printf("All explore action tests passed.\n");
    return 0;
}
