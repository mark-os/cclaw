#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <curl/curl.h>
#include "web.h"
#include "config_registry.h"
#include "db.h"
#include <pthread.h>
#include "approval.h"
#include "tool_share.h"
#include "tools.h"
#include "proc.h"
#include "test_util.h"

/* One response buffer for bodies and headers; bytes are kept as they came
 * (a shared file is binary), with a NUL after them for strstr. */
typedef struct { char *d; size_t cap, n; } Buf;

static size_t buf_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    Buf *b = ud;
    size_t t = size * nmemb, room = b->cap - 1 - b->n;
    if (t < room) room = t;
    memcpy(b->d + b->n, ptr, room);
    b->n += room;
    b->d[b->n] = '\0';
    return t;
}

static char g_page[65536];

/* GET url into g_page (redirects followed); returns the HTTP code. */
static long http_get(CURL *curl, const char *url) {
    Buf b = { g_page, sizeof(g_page), 0 };
    g_page[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buf_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    assert(curl_easy_perform(curl) == CURLE_OK);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    return code;
}

/* POST body to url with the given headers (NULL-terminated); answer in g_page. */
static long http_post(CURL *curl, const char *url, const char *body, const char *const *hdrs) {
    Buf b = { g_page, sizeof(g_page), 0 };
    g_page[0] = '\0';
    struct curl_slist *h = NULL;
    for (; hdrs && *hdrs; hdrs++) h = curl_slist_append(h, *hdrs);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buf_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &b);
    assert(curl_easy_perform(curl) == CURLE_OK);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, NULL);
    curl_slist_free_all(h);
    return code;
}

/* POST a JSON action the way the page does (Content-Type + X-Cclaw-Action).
 * extra adds one header. */
static long act_post(CURL *curl, int port, const char *json, const char *extra) {
    char url[96];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/admin/api/act", port);
    const char *h[] = { "Content-Type: application/json", "X-Cclaw-Action: 1", extra, NULL };
    return http_post(curl, url, json, h);
}

static void test_status_page(void) {
    Config cfg = {0};
    cfg.web_port = 19876;
    cfg.db_path = "test_web.db";

    sqlite3 *db = test_db_open(cfg.db_path);
    assert(db);

    int64_t sid = session_create(db, "test-session", NULL, -1, 0);
    assert(sid > 0);
    assert(session_set_state(db, sid, "llm_running") == 0);
    assert(inbox_insert(db, sid, "test", NULL, "hello") > 0);

    assert(web_start(&cfg, cfg.db_path) == 0);
    usleep(50000);

    /* Unauthenticated `/` is liveness only — no session table (r2 F3). */
    CURL *curl = curl_easy_init();
    assert(curl);
    assert(http_get(curl, "http://127.0.0.1:19876/") == 200);
    assert(strstr(g_page, "version:"));
    assert(strstr(g_page, "uptime:"));
    assert(strstr(g_page, "sessions:") == NULL);
    assert(strstr(g_page, "test-session") == NULL);

    /* With the dashboard admin token (?token=), the full dump is served. */
    char *tok = config_get(db, "web_admin_token");
    assert(tok && tok[0]);
    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:19876/?token=%s", tok);
    free(tok);
    assert(http_get(curl, url) == 200);
    assert(strstr(g_page, "sessions:\n"));
    assert(strstr(g_page, "test-session"));
    assert(strstr(g_page, "running: 1"));

    curl_easy_cleanup(curl);
    web_stop();
    db_close(db);
    test_db_clean("test_web.db");
}

/* Fake channel runner: accept one UDS connection, capture the envelope,
 * reply "207\nhook-reply". Forked child so the blocking proxy thread in
 * web.c has a peer. */
static pid_t start_fake_runner(const char *sock_path) {
    unlink(sock_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sock_path);
    assert(bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    assert(listen(fd, 1) == 0);
    struct timeval tv = {10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    pid_t pid = fork();
    if (pid == 0) {
        int cfd = accept(fd, NULL, NULL);
        if (cfd < 0) _exit(1);
        char buf[16384];
        size_t len = 0;
        for (;;) {
            ssize_t n = read(cfd, buf + len, sizeof(buf) - len - 1);
            if (n > 0) { len += (size_t)n; continue; }
            break;
        }
        buf[len] = '\0';
        /* Envelope must carry method, path and the body we POSTed */
        int ok = strstr(buf, "\"method\":\"POST\"") &&
                 strstr(buf, "\"path\":\"/hook/tchan\"") &&
                 strstr(buf, "hello-hook") &&
                 strstr(buf, "X-Hook-Test");
        const char *reply = ok ? "207\nhook-reply" : "500\nbad-envelope";
        (void)!write(cfd, reply, strlen(reply));
        close(cfd);
        close(fd);
        _exit(ok ? 0 : 2);
    }
    close(fd);
    return pid;
}

static void test_hook_proxy(void) {
    Config cfg = {0};
    cfg.web_port = 19877;
    cfg.db_path = "test_web_hook.db";

    sqlite3 *db = test_db_open(cfg.db_path);
    assert(db);
    assert(web_start(&cfg, cfg.db_path) == 0);
    usleep(50000);

    /* channel_uds_path("test_web_hook.db","tchan") = test_web_hook.tchan.sock */
    pid_t runner = start_fake_runner("test_web_hook.tchan.sock");
    assert(runner > 0);
    usleep(50000);

    CURL *curl = curl_easy_init();
    assert(curl);
    const char *h[] = { "X-Hook-Test: yes", NULL };
    assert(http_post(curl, "http://127.0.0.1:19877/hook/tchan", "{\"msg\":\"hello-hook\"}", h) == 207);
    assert(strcmp(g_page, "hook-reply") == 0);

    int status = -1;
    waitpid(runner, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* No runner socket → 502; bad channel name → 404 */
    assert(http_post(curl, "http://127.0.0.1:19877/hook/nosuch", "{}", NULL) == 502);
    assert(http_get(curl, "http://127.0.0.1:19877/hook/bad$chan") == 404);

    curl_easy_cleanup(curl);
    web_stop();
    db_close(db);
    test_db_clean("test_web_hook.db");
    unlink("test_web_hook.tchan.sock");
}

/* The operator's page at /admin/data over HTTP: sign-in, the actions'
 * guards, and what only the web server's own handle can show. The actions
 * themselves are test_explore_act's. */
static void test_admin_dashboard(void) {
    Config cfg = {0};
    cfg.web_port = 19878;
    cfg.db_path = "test_web_admin.db";
    test_db_clean(cfg.db_path);   /* a killed run leaves its database behind */

    sqlite3 *db = test_db_open(cfg.db_path);
    assert(db);
    assert(sqlite3_exec(db,
        "INSERT OR REPLACE INTO providers(name, base_url, api_key_env) VALUES"
        "  ('openrouter','https://openrouter.example/v1','OPENROUTER_API_KEY');"
        "INSERT OR REPLACE INTO models(id, provider_name, model) VALUES"
        "  ('m1','openrouter','vendor/one'), ('m2','openrouter','vendor/two');"
        "INSERT OR IGNORE INTO agents(name) VALUES('Assistant'), ('ev<il&name');"
        "INSERT INTO sessions(name, agent_name) VALUES('s','Assistant');"
        "INSERT INTO approvals(session_id, tool_name, park_reason, args_json, state)"
        "  VALUES(1, 'shell_exec', 'approval_required', '{\"cmd\":\"ls\"}', 'pending');",
        NULL, NULL, NULL) == SQLITE_OK);

    assert(web_start(&cfg, cfg.db_path) == 0);
    usleep(50000);

    /* web_start minted a token */
    char *token = test_qs(db, "SELECT value FROM config WHERE key='web_admin_token'");
    assert(token && strlen(token) == 32);

    CURL *curl = curl_easy_init();
    assert(curl);
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");  /* enable cookie engine */

    /* No token → 403; bad token → 403 */
    assert(http_get(curl, "http://127.0.0.1:19878/admin/data") == 403);
    assert(http_get(curl, "http://127.0.0.1:19878/admin/data?token=00000000000000000000000000000000") == 403);

    /* Good token → 303 sets the cookie, following lands on the page */
    char url[160];
    snprintf(url, sizeof(url), "http://127.0.0.1:19878/admin/data?token=%s", token);
    assert(http_get(curl, url) == 200);
    assert(strstr(g_page, "<title>cclaw</title>"));
    /* Cookie now carries auth on its own. */
    assert(http_get(curl, "http://127.0.0.1:19878/admin/data") == 200);

    /* A hostile agent name reaches the page as JSON data, never as markup
     * (the page draws with textContent only — test_explore checks the source). */
    assert(http_get(curl, "http://127.0.0.1:19878/admin/api/rows?table=agents") == 200);
    assert(strstr(g_page, "\"ev<il&name\""));

    /* The web server has its own handle. With this handle inside a
     * transaction, a page write still commits on its own and leaves this
     * transaction open. (On a shared handle the page's COMMIT ended it.) */
    assert(sqlite3_exec(db, "BEGIN; SELECT count(*) FROM agents;",
                        NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_get_autocommit(db) == 0);
    assert(act_post(curl, 19878, "{\"action\":\"models_set\",\"agent\":\"Assistant\",\"purpose\":\"chat\","
                                 "\"models\":[{\"model_id\":\"m2\"},{\"model_id\":\"m1\","
                                 "\"reasoning_effort\":\"low\"}]}", NULL) == 200);
    assert(sqlite3_get_autocommit(db) == 0);
    assert(sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL) == SQLITE_OK);
    char *kept = test_qs(db,
        "SELECT group_concat(model_id || ':' || COALESCE(reasoning_effort, '-'), ',') FROM"
        " (SELECT * FROM agent_models WHERE agent_name='Assistant' AND purpose='chat' ORDER BY pos)");
    assert(kept && strcmp(kept, "m2:-,m1:low") == 0);
    free(kept);
    /* ...written by the dashboard handle, labelled with the action. */
    char *who = test_qs(db, "SELECT actor || '|' || context FROM change_log"
                            " WHERE table_name='agent_models' ORDER BY id DESC LIMIT 1");
    assert(who && strcmp(who, "dashboard|act:models_set") == 0);
    free(who);

    /* approve does NOT resolve on the web thread: it records the decision
     * for the event loop and leaves the approval pending. */
    assert(act_post(curl, 19878, "{\"action\":\"approval_decide\",\"approval_id\":1,"
                                 "\"decision\":\"always\"}", NULL) == 200);
    assert(test_qi(db, "SELECT state = 'pending' FROM approvals WHERE id=1") == 1);
    /* A second click does not replace the first decision. */
    assert(act_post(curl, 19878, "{\"action\":\"approval_decide\",\"approval_id\":1,"
                                 "\"decision\":\"deny\"}", NULL) == 409);
    int nq = 0;
    ApprovalQueued *q = approval_decisions_take(db, &nq);
    assert(nq == 1 && q);
    assert(q[0].approval_id == 1);
    assert(q[0].decision == APPROVAL_ALWAYS);
    assert(strcmp(q[0].decided_via, "dashboard") == 0);
    free(q);
    /* Taken exactly once. */
    q = approval_decisions_take(db, &nq);
    assert(nq == 0 && q == NULL);

    /* A long note arrives over several TCP segments and is read whole; a
     * note id addressed through the wrong block changes nothing. */
    assert(act_post(curl, 19878, "{\"action\":\"memory_block_create\",\"label\":\"NOTES\","
                                 "\"agent\":\"Assistant\"}", NULL) == 200);
    long long blk = test_qi(db, "SELECT id FROM memory_blocks WHERE label='NOTES'");
    char form[256];
    snprintf(form, sizeof(form), "{\"action\":\"memory_note_add\",\"block_id\":%lld,\"text\":\"n\"}", blk);
    assert(act_post(curl, 19878, form, NULL) == 200);
    long long nid = test_qi(db, "SELECT id FROM memory_notes WHERE text='n'");
    assert(blk > 0 && nid > 0);
    {
        static char big[4000 * 2 + 256];
        int o = snprintf(big, sizeof(big),
                         "{\"action\":\"memory_note_edit\",\"block_id\":%lld,\"note_id\":%lld,\"text\":\"",
                         blk, nid);
        for (int i = 0; i < 4000; i++) {
            assert((size_t)o + 3 <= sizeof(big));
            big[o++] = '\xc3'; big[o++] = '\xa9';
        }
        snprintf(big + o, sizeof(big) - (size_t)o, "\"}");
        assert(act_post(curl, 19878, big, NULL) == 200);
        assert(test_qi(db, "SELECT length(text) FROM memory_notes") == 4000);
    }
    snprintf(form, sizeof(form), "{\"action\":\"memory_note_delete\",\"block_id\":%lld,\"note_id\":%lld}",
             blk + 999, nid);
    assert(act_post(curl, 19878, form, NULL) == 404);
    assert(test_qi(db, "SELECT count(*) FROM memory_notes") == 1);

    /* ── Forgery: the cookie alone does not make an action ── */
    long long before = test_qi(db, "SELECT count(*) FROM change_log");
    static const char GRANT[] = "{\"action\":\"grant_add\",\"agent\":\"Assistant\","
                                "\"kind\":\"host\",\"value\":\"evil.example\"}";
    static const char ACT[] = "http://127.0.0.1:19878/admin/api/act";
    const char *form_ct[] = { "Content-Type: application/x-www-form-urlencoded", NULL };
    const char *text_ct[] = { "Content-Type: text/plain", NULL };
    const char *json_ct[] = { "Content-Type: application/json", NULL };
    /* Another site's HTML form: urlencoded or text/plain, no custom header. */
    assert(http_post(curl, ACT, "action=grant_add&agent=Assistant&kind=host&value=evil.example",
                     form_ct) == 403);
    assert(http_post(curl, ACT, GRANT, text_ct) == 403);
    /* JSON without the page's header (what a no-preflight fetch could send). */
    assert(http_post(curl, ACT, GRANT, json_ct) == 403);
    /* The browser says it came from elsewhere. */
    assert(act_post(curl, 19878, GRANT, "Origin: http://evil.example") == 403);
    assert(act_post(curl, 19878, GRANT, "Sec-Fetch-Site: cross-site") == 403);
    /* The old form endpoint is gone. */
    assert(http_post(curl, "http://127.0.0.1:19878/admin/data/act",
                     "action=grant&agent=Assistant&kind=host&value=evil.example", form_ct) == 405);
    /* GET cannot act. */
    assert(http_get(curl, "http://127.0.0.1:19878/admin/api/act?action=grant_add&agent=Assistant"
                          "&kind=host&value=evil.example") == 405);
    /* Same origin, as the page sends it: allowed. */
    assert(act_post(curl, 19878, "{\"action\":\"grant_add\",\"agent\":\"Assistant\",\"kind\":\"host\","
                                 "\"value\":\"ok.example\"}", "Origin: http://127.0.0.1:19878") == 200);
    assert(test_qi(db, "SELECT count(*) FROM grants WHERE value='evil.example'") == 0);
    assert(test_qi(db, "SELECT count(*) FROM change_log WHERE NOT (table_name='grants'"
                       " AND COALESCE(new_values, '') LIKE '%ok.example%')") == before);

    /* POST without the cookie → 403 (the query token is GET-only) */
    CURL *bare = curl_easy_init();
    assert(bare);
    assert(act_post(bare, 19878, GRANT, NULL) == 403);
    assert(strstr(g_page, "not signed in"));
    curl_easy_cleanup(bare);

    free(token);
    curl_easy_cleanup(curl);
    web_stop();
    db_close(db);
    test_db_clean("test_web_admin.db");
}

/* The admin panel at /admin: the same token and cookie as /admin/data. A
 * cookie set through either page signs in the other page and the API (it is
 * Path=/), and the token leaves the URL with a 303 to the clean address. */
static void test_dashboard_page(void) {
    Config cfg = {0};
    cfg.web_port = 19879;
    cfg.db_path = "test_web_dash.db";
    test_db_clean(cfg.db_path);
    sqlite3 *db = test_db_open(cfg.db_path);
    assert(db);
    assert(sqlite3_exec(db, "INSERT OR IGNORE INTO agents(name) VALUES('Assistant');",
                        NULL, NULL, NULL) == SQLITE_OK);
    assert(web_start(&cfg, cfg.db_path) == 0);
    usleep(50000);
    char *token = test_qs(db, "SELECT value FROM config WHERE key='web_admin_token'");
    assert(token && strlen(token) == 32);

    char url[160];
    CURL *curl = curl_easy_init();
    assert(curl);
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");

    /* Not signed in: the page is 403 (and says how to get in), the API too. */
    assert(http_get(curl, "http://127.0.0.1:19879/admin") == 403);
    assert(strstr(g_page, "/admin"));
    assert(http_get(curl, "http://127.0.0.1:19879/admin?token=00000000000000000000000000000000") == 403);
    assert(http_get(curl, "http://127.0.0.1:19879/admin/api/rows?table=agents") == 403);

    /* The token: 303 to the clean URL, setting a Path=/ HttpOnly cookie. */
    {
        static char hdr[4096];
        Buf hb = { hdr, sizeof(hdr), 0 };
        hdr[0] = '\0';
        snprintf(url, sizeof(url), "http://127.0.0.1:19879/admin?token=%s", token);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, buf_cb);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hb);
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        Buf pb = { g_page, sizeof(g_page), 0 };
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buf_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &pb);
        assert(curl_easy_perform(curl) == CURLE_OK);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        assert(code == 303);
        assert(strstr(hdr, "Location: /admin\r\n"));
        assert(strstr(hdr, "Path=/; HttpOnly; SameSite=Lax"));
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, NULL);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, NULL);
    }
    /* That cookie: the page, the API, the explorer, and an action. */
    assert(http_get(curl, "http://127.0.0.1:19879/admin") == 200);
    assert(strstr(g_page, "<title>") && strstr(g_page, "/admin/api/"));
    assert(http_get(curl, "http://127.0.0.1:19879/admin/api/rows?table=agents") == 200);
    assert(strstr(g_page, "\"Assistant\""));
    assert(http_get(curl, "http://127.0.0.1:19879/admin/data") == 200);
    assert(act_post(curl, 19879, "{\"action\":\"grant_add\",\"agent\":\"Assistant\",\"kind\":\"host\","
                                 "\"value\":\"dash.example\"}", NULL) == 200);
    assert(test_qi(db, "SELECT count(*) FROM grants WHERE value='dash.example'") == 1);
    /* Only GET, only the exact path. */
    const char *json_ct[] = { "Content-Type: application/json", NULL };
    assert(http_post(curl, "http://127.0.0.1:19879/admin", "{}", json_ct) == 405);
    assert(http_get(curl, "http://127.0.0.1:19879/admin/x") == 404);
    curl_easy_cleanup(curl);

    /* The other way round: signed in through /admin/data, /admin opens. */
    curl = curl_easy_init();
    assert(curl);
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");
    snprintf(url, sizeof(url), "http://127.0.0.1:19879/admin/data?token=%s", token);
    assert(http_get(curl, url) == 200);
    assert(http_get(curl, "http://127.0.0.1:19879/admin") == 200);
    assert(strstr(g_page, "/admin/api/"));
    curl_easy_cleanup(curl);

    free(token);
    web_stop();
    db_close(db);
    test_db_clean(cfg.db_path);
}

/* proc_db() notices a caller that is not the thread that installed it. */
static void *foreign_use(void *arg) {
    (void)arg;
    (void)proc_db();
    return NULL;
}

static void test_db_handle_owner(void) {
    sqlite3 *db = test_db_open("test_web_owner.db");
    assert(db);
    proc_set_db(db);
    assert(proc_db() == db);
    assert(proc_db_foreign_uses() == 0);     /* the owner is not counted */
    pthread_t t;
    assert(pthread_create(&t, NULL, foreign_use, NULL) == 0);
    pthread_join(t, NULL);
    assert(proc_db_foreign_uses() == 1);     /* another thread is */
    proc_set_db(NULL);
    db_close(db);
    test_db_clean("test_web_owner.db");
}

/* ── /share/<token>: files an agent put behind a link ── */

typedef struct { char d[8192]; Buf b; } Blob;

/* GET url → status; headers (text) and body (bytes) captured. */
static long share_get(const char *url, Blob *hdr, Blob *body) {
    hdr->b = (Buf){ hdr->d, sizeof(hdr->d), 0 };
    body->b = (Buf){ body->d, sizeof(body->d), 0 };
    hdr->d[0] = body->d[0] = '\0';
    CURL *c = curl_easy_init();
    assert(c);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, buf_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body->b);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, buf_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &hdr->b);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 5L);
    long code = 0;
    if (curl_easy_perform(c) == CURLE_OK)
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    return code;
}

static void put_file(const char *path, const void *d, size_t n) {
    FILE *f = fopen(path, "wb");
    assert(f && fwrite(d, 1, n, f) == n && fclose(f) == 0);
}

/* share_file's result is "<url>\nvalid for ..."; cut it to the URL. */
static char *share(ToolRegistry *reg, const char *args, int *failed) {
    ToolEntry *te = tools_lookup(reg, "share_file");
    assert(te);
    *failed = 0;
    char *r = te->handler(args, te->user_data, failed);
    assert(r);
    if (!*failed) *strchr(r, '\n') = '\0';
    return r;
}

#define SHARE_WS "/tmp/test_web_share_ws"

static void test_share(void) {
    static const unsigned char png[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n',
                                         0x00, 0xFF, 0x00, 0x80, 0xFE, 0x00 };
    static const unsigned char blob[] = { 'x', 0x00, 0x01, 0x02, 0xFF };
    const char *html = "<script>alert(document.cookie)</script>";
    assert(system("rm -rf " SHARE_WS " && mkdir -p " SHARE_WS "/pics") == 0);
    put_file(SHARE_WS "/pics/my pic.png", png, sizeof(png));
    put_file(SHARE_WS "/page.html", html, strlen(html));
    put_file(SHARE_WS "/data.bin", blob, sizeof(blob));

    Config cfg = {0};
    cfg.web_port = 19879;
    cfg.db_path = "test_web_share.db";
    test_db_clean(cfg.db_path);
    sqlite3 *db = test_db_open(cfg.db_path);
    assert(db);
    test_seed_agent(db, "Sharer");
    assert(sqlite3_exec(db, "INSERT OR REPLACE INTO config(key,value) VALUES"
                            " ('web_public_url','http://127.0.0.1:19879/'),"
                            " ('share_max_hours','48');",
                        NULL, NULL, NULL) == SQLITE_OK);
    assert(web_start(&cfg, cfg.db_path) == 0);
    usleep(50000);

    ToolRegistry reg;
    tools_init(&reg);
    ToolChannelSendCtx ctx = { .db = db, .workspace = SHARE_WS };
    snprintf(ctx.agent_name, sizeof(ctx.agent_name), "Sharer");
    assert(tool_share_register(&reg, &ctx) == 0);
    Blob *h = malloc(sizeof(Blob)), *b = malloc(sizeof(Blob));
    assert(h && b);
    int failed;

    /* An image: the link is built on web_public_url (trailing slash dropped),
     * and what comes back is the file, byte for byte, as an image. */
    char *url = share(&reg, "{\"file\":\"pics/my pic.png\",\"hours\":1000}", &failed);
    assert(!failed && strncmp(url, "http://127.0.0.1:19879/share/", 29) == 0);
    assert(strlen(url) == 29 + 32 + strlen("/my_pic.png") &&
           strcmp(url + 29 + 32, "/my_pic.png") == 0);
    assert(share_get(url, h, b) == 200);
    assert(b->b.n == sizeof(png) && memcmp(b->d, png, sizeof(png)) == 0);
    assert(strstr(h->d, "Content-Type: image/png"));
    assert(strstr(h->d, "Content-Security-Policy: sandbox"));
    assert(strstr(h->d, "X-Content-Type-Options: nosniff"));
    /* hours is capped by share_max_hours */
    assert(test_qi(db, "SELECT (max(expires_at) - unixepoch() + 30) / 3600 FROM shares") == 48);
    /* the name after the token is decoration: the token alone selects */
    char bare[128];
    snprintf(bare, sizeof(bare), "%.61s", url);
    assert(share_get(bare, h, b) == 200 && b->b.n == sizeof(png));

    /* The server also hosts /admin/data, so an agent's file must never run as a
     * page: HTML is served as text, unknown binary as a download. */
    char *hurl = share(&reg, "{\"file\":\"page.html\"}", &failed);
    assert(!failed && share_get(hurl, h, b) == 200);
    assert(strstr(h->d, "Content-Type: text/plain"));
    assert(!strstr(h->d, "text/html"));
    assert(b->b.n == strlen(html) && memcmp(b->d, html, b->b.n) == 0);
    char *burl = share(&reg, "{\"file\":\"data.bin\"}", &failed);
    assert(!failed && share_get(burl, h, b) == 200);
    assert(strstr(h->d, "Content-Type: application/octet-stream"));
    assert(strstr(h->d, "Content-Disposition: attachment"));
    assert(b->b.n == sizeof(blob) && memcmp(b->d, blob, sizeof(blob)) == 0);

    /* Not a live share: one answer for all of it. */
    assert(share_get("http://127.0.0.1:19879/share/"
                     "00000000000000000000000000000000/x", h, b) == 404);
    assert(share_get("http://127.0.0.1:19879/share/nope", h, b) == 404);
    assert(share_get("http://127.0.0.1:19879/share/", h, b) == 404);

    /* The agent still owns the path. Once it leads somewhere else, the link
     * serves nothing — not the new target, wherever that is. */
    assert(system("rm " SHARE_WS "/page.html && ln -s /etc/hostname "
                  SHARE_WS "/page.html") == 0);
    assert(share_get(hurl, h, b) == 404);
    assert(system("mv " SHARE_WS "/pics " SHARE_WS "/pics.real && ln -s /etc "
                  SHARE_WS "/pics") == 0);
    assert(share_get(url, h, b) == 404);
    assert(system("rm " SHARE_WS "/pics && mv " SHARE_WS "/pics.real "
                  SHARE_WS "/pics") == 0);
    assert(share_get(url, h, b) == 200);

    /* Expiry is the clock against the row. */
    assert(sqlite3_exec(db, "UPDATE shares SET expires_at = unixepoch() - 1;",
                        NULL, NULL, NULL) == SQLITE_OK);
    assert(share_get(url, h, b) == 404 && share_get(burl, h, b) == 404);

    /* Only the agent's own files can be shared; an expired row is swept by
     * the next share. */
    char *r = share(&reg, "{\"file\":\"/etc/hostname\"}", &failed);
    assert(failed && strstr(r, "outside your workspace"));
    free(r);
    r = share(&reg, "{\"file\":\"pics\"}", &failed);
    assert(failed && strstr(r, "regular file"));
    free(r);
    r = share(&reg, "{\"file\":\"data.bin\"}", &failed);
    assert(!failed);
    free(r);
    assert(test_qi(db, "SELECT count(*) FROM shares") == 1);

    free(url); free(hurl); free(burl); free(h); free(b);
    tools_free(&reg);
    web_stop();
    db_close(db);
    test_db_clean(cfg.db_path);
    assert(system("rm -rf " SHARE_WS) == 0);
}

/* web_bind: the listener is loopback unless the operator says otherwise, the
 * links follow it, and a value that is not one IPv4 address stops the server
 * instead of being passed to the listener. */
static void test_web_bind(void) {
    Config cfg = {0};
    cfg.web_port = 19880;
    cfg.db_path = "test_web.db";
    sqlite3 *db = test_db_open(cfg.db_path);
    assert(db);

    char *v = config_get(db, "web_bind");
    assert(v && strcmp(v, "127.0.0.1") == 0);
    free(v);
    setenv("CCLAW_WEB_PORT", "19880", 1);
    char *base = config_web_base_url(db);
    assert(base && strcmp(base, "http://127.0.0.1:19880") == 0);
    free(base);

    assert(config_set(db, "web_bind", "0.0.0.0:80,9000") == 0);
    assert(web_start(&cfg, cfg.db_path) != 0);
    assert(config_set(db, "web_bind", "localhost") == 0);
    assert(web_start(&cfg, cfg.db_path) != 0);

    /* Every interface: still answers on loopback. */
    assert(config_set(db, "web_bind", "0.0.0.0") == 0);
    assert(web_start(&cfg, cfg.db_path) == 0);
    usleep(50000);
    CURL *curl = curl_easy_init();
    assert(curl);
    assert(http_get(curl, "http://127.0.0.1:19880/") == 200);
    assert(strstr(g_page, "version:"));
    curl_easy_cleanup(curl);
    web_stop();

    unsetenv("CCLAW_WEB_PORT");
    db_close(db);
    test_db_clean(cfg.db_path);
}

int main(void) {
    TEST_INIT();
    RUN(test_db_handle_owner);
    RUN(test_status_page);
    RUN(test_hook_proxy);
    RUN(test_admin_dashboard);
    RUN(test_dashboard_page);
    RUN(test_share);
    RUN(test_web_bind);
    printf("All web tests passed.\n");
    return 0;
}
