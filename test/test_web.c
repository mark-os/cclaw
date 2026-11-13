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


static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    char *buf = (char *)userdata;
    size_t cur = strlen(buf);
    if (cur + total < 4096) {
        memcpy(buf + cur, ptr, total);
        buf[cur + total] = '\0';
    }
    return total;
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
    char response[4096] = {0};
    curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:19876/");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)response);
    long http_code = 0;
    CURLcode res = curl_easy_perform(curl);
    assert(res == CURLE_OK);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    assert(http_code == 200);
    assert(strstr(response, "version:"));
    assert(strstr(response, "uptime:"));
    assert(strstr(response, "sessions:") == NULL);
    assert(strstr(response, "test-session") == NULL);
    curl_easy_cleanup(curl);

    /* With the dashboard admin token (?token=), the full dump is served. */
    char *tok = config_get(db, "web_admin_token");
    assert(tok && tok[0]);
    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:19876/?token=%s", tok);
    free(tok);
    curl = curl_easy_init();
    assert(curl);
    response[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)response);
    res = curl_easy_perform(curl);
    assert(res == CURLE_OK);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    assert(http_code == 200);

    /* Plain text assertions */
    assert(strstr(response, "version:"));
    assert(strstr(response, "uptime:"));
    assert(strstr(response, "sessions:\n"));
    assert(strstr(response, "id|name|state|updated_at|inbox_depth\n"));
    assert(strstr(response, "test-session"));
    assert(strstr(response, "running"));
    assert(strstr(response, "state_metrics:\n"));
    assert(strstr(response, "running: 1"));

    curl_easy_cleanup(curl);
    web_stop();
    db_close(db);
    test_db_clean("test_web.db");
    printf("PASS: test_status_page\n");
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
        (void)write(cfd, reply, strlen(reply));
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
    char response[4096] = {0};
    struct curl_slist *hdrs = curl_slist_append(NULL, "X-Hook-Test: yes");
    curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:19877/hook/tchan");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "{\"msg\":\"hello-hook\"}");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)response);
    CURLcode res = curl_easy_perform(curl);
    assert(res == CURLE_OK);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    assert(http_code == 207);
    assert(strcmp(response, "hook-reply") == 0);

    int status = -1;
    waitpid(runner, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* No runner socket → 502 */
    char response2[4096] = {0};
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:19877/hook/nosuch");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "{}");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)response2);
    res = curl_easy_perform(curl);
    assert(res == CURLE_OK);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    assert(http_code == 502);

    /* Bad channel name → 404 */
    char response3[4096] = {0};
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:19877/hook/bad$chan");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)response3);
    res = curl_easy_perform(curl);
    assert(res == CURLE_OK);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    assert(http_code == 404);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    web_stop();
    db_close(db);
    test_db_clean("test_web_hook.db");
    unlink("test_web_hook.tchan.sock");
    printf("PASS: test_hook_proxy\n");
}

/* ── admin dashboard ────────────────────────────────────────────── */

struct page_buf { char *buf; size_t cap; };

static size_t page_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    struct page_buf *b = (struct page_buf *)userdata;
    size_t cur = strlen(b->buf);
    if (cur + total < b->cap) {
        memcpy(b->buf + cur, ptr, total);
        b->buf[cur + total] = '\0';
    }
    return total;
}

static char *query_text(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st;
    char *out = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) out = strdup(v);
    }
    sqlite3_finalize(st);
    return out;
}

/* GET url into page (cleared first); returns final HTTP code. */
static long dash_get(CURL *curl, const char *url, char *page, size_t cap) {
    struct page_buf pb = {page, cap};
    page[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, page_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &pb);
    assert(curl_easy_perform(curl) == CURLE_OK);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    return code;
}

/* POST form to /admin/act without following the 303; returns HTTP code. */
static long dash_post(CURL *curl, const char *url, const char *form,
                      char *page, size_t cap) {
    struct page_buf pb = {page, cap};
    page[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, form);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, page_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &pb);
    assert(curl_easy_perform(curl) == CURLE_OK);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    return code;
}

static void test_admin_dashboard(void) {
    Config cfg = {0};
    cfg.web_port = 19878;
    cfg.db_path = "test_web_admin.db";

    sqlite3 *db = test_db_open(cfg.db_path);
    assert(db);
    assert(sqlite3_exec(db,
        "INSERT OR REPLACE INTO providers(name, base_url, api_key_env) VALUES"
        "  ('openrouter','https://openrouter.example/v1','OPENROUTER_API_KEY');"
        "INSERT OR REPLACE INTO models(id, provider_name, model) VALUES"
        "  ('m1','openrouter','vendor/one'), ('m2','openrouter','vendor/two');"
        "INSERT OR IGNORE INTO agents(name) VALUES('Assistant'), ('ev<il&name');"
        "INSERT INTO sessions(name, agent_name, channel_name)"
        "  VALUES('s','Assistant','telegram');"
        "INSERT INTO approvals(session_id, tool_name, park_reason, args_json, state)"
        "  VALUES(1, 'shell_exec', 'approval_required', '{\"cmd\":\"ls\"}', 'pending');",
        NULL, NULL, NULL) == SQLITE_OK);

    assert(web_start(&cfg, cfg.db_path) == 0);
    usleep(50000);

    /* web_start minted a token */
    char *token = query_text(db, "SELECT value FROM config WHERE key='web_admin_token'");
    assert(token && strlen(token) == 32);

    static char page[65536];
    CURL *curl = curl_easy_init();
    assert(curl);
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");  /* enable cookie engine */

    /* No token → 403 */
    assert(dash_get(curl, "http://127.0.0.1:19878/admin", page, sizeof(page)) == 403);

    /* Bad token → 403 */
    assert(dash_get(curl, "http://127.0.0.1:19878/admin?token=00000000000000000000000000000000",
                    page, sizeof(page)) == 403);

    /* Good token → 303 sets cookie, follow lands on the page */
    char url[160];
    snprintf(url, sizeof(url), "http://127.0.0.1:19878/admin?token=%s", token);
    assert(dash_get(curl, url, page, sizeof(page)) == 200);
    assert(strstr(page, "cclaw admin"));
    assert(strstr(page, "m1"));
    assert(strstr(page, "primary model"));
    assert(strstr(page, "shell_exec"));         /* pending approval row */

    /* Cookie now carries auth on its own */
    assert(dash_get(curl, "http://127.0.0.1:19878/admin", page, sizeof(page)) == 200);

    /* Hostile agent name is HTML-escaped everywhere */
    assert(strstr(page, "ev&lt;il&amp;name"));
    assert(strstr(page, "ev<il&name") == NULL);

    /* switch_model makes m2 the head of every agent's routing list */
    assert(dash_post(curl, "http://127.0.0.1:19878/admin/act",
                     "action=switch_model&model_id=m2", page, sizeof(page)) == 303);
    char *head = query_text(db,
        "SELECT model_id FROM agent_models"
        " WHERE agent_name='Assistant' AND pos=0");
    assert(head && strcmp(head, "m2") == 0);
    free(head);

    /* grant → row exists; revoke by rowid → gone */
    assert(dash_post(curl, "http://127.0.0.1:19878/admin/act",
                     "action=grant&agent=Assistant&kind=tool&value=web_fetch",
                     page, sizeof(page)) == 303);
    char *gid = query_text(db,
        "SELECT rowid FROM grants WHERE agent_name='Assistant' AND value='web_fetch'");
    assert(gid);
    char form[128];
    snprintf(form, sizeof(form), "action=revoke&agent=Assistant&grant_id=%s", gid);
    free(gid);
    assert(dash_post(curl, "http://127.0.0.1:19878/admin/act", form,
                     page, sizeof(page)) == 303);
    char *gone = query_text(db,
        "SELECT rowid FROM grants WHERE agent_name='Assistant' AND value='web_fetch'");
    assert(gone == NULL);

    /* The web server has its own handle. With this handle inside a
     * transaction, a dashboard write still commits on its own and leaves
     * this transaction open. (On a shared handle the dashboard's COMMIT
     * ended it.) */
    assert(sqlite3_exec(db, "BEGIN; SELECT count(*) FROM agents;",
                        NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_get_autocommit(db) == 0);
    assert(dash_post(curl, "http://127.0.0.1:19878/admin/act",
                     "action=set_agent_model&agent=Assistant&primary_model=m2",
                     page, sizeof(page)) == 303);
    assert(sqlite3_get_autocommit(db) == 0);
    assert(sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL) == SQLITE_OK);
    char *kept = query_text(db,
        "SELECT model_id FROM agent_models WHERE agent_name='Assistant' AND pos=0");
    assert(kept && strcmp(kept, "m2") == 0);
    free(kept);

    /* approve does NOT resolve on the web thread: it records the decision
     * for the event loop and leaves the approval pending. */
    assert(dash_post(curl, "http://127.0.0.1:19878/admin/act",
                     "action=approve&approval_id=1&decision=always",
                     page, sizeof(page)) == 303);
    char *st = query_text(db, "SELECT state FROM approvals WHERE id=1");
    assert(st && strcmp(st, "pending") == 0);
    free(st);
    /* A second click does not replace the first decision. */
    assert(dash_post(curl, "http://127.0.0.1:19878/admin/act",
                     "action=approve&approval_id=1&decision=deny",
                     page, sizeof(page)) == 303);
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

    /* POST without the cookie → 403 (query token is GET-only) */
    CURL *bare = curl_easy_init();
    assert(bare);
    assert(dash_post(bare, "http://127.0.0.1:19878/admin/act",
                     "action=switch_model&model_id=m1", page, sizeof(page)) == 403);
    curl_easy_cleanup(bare);

    free(token);
    curl_easy_cleanup(curl);
    web_stop();
    db_close(db);
    test_db_clean("test_web_admin.db");
    printf("PASS: test_admin_dashboard\n");
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
    printf("PASS: test_db_handle_owner\n");
}

/* ── /share/<token>: files an agent put behind a link ── */

typedef struct { unsigned char d[8192]; size_t n; } Blob;

static size_t blob_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    Blob *b = ud;
    size_t t = size * nmemb, room = sizeof(b->d) - 1 - b->n;
    if (t < room) room = t;
    memcpy(b->d + b->n, ptr, room);
    b->n += room;
    b->d[b->n] = '\0';
    return t;
}

/* GET url → status; headers (text) and body (bytes) captured. */
static long share_get(const char *url, Blob *hdr, Blob *body) {
    hdr->n = body->n = 0;
    hdr->d[0] = body->d[0] = '\0';
    CURL *c = curl_easy_init();
    assert(c);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, blob_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, blob_cb);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, hdr);
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
    assert(b->n == sizeof(png) && memcmp(b->d, png, sizeof(png)) == 0);
    assert(strstr((char *)h->d, "Content-Type: image/png"));
    assert(strstr((char *)h->d, "Content-Security-Policy: sandbox"));
    assert(strstr((char *)h->d, "X-Content-Type-Options: nosniff"));
    /* hours is capped by share_max_hours */
    assert(db_scalar_i64(db, "SELECT (max(expires_at) - unixepoch() + 30) / 3600"
                             " FROM shares WHERE ? = 0;", 0, -1) == 48);
    /* the name after the token is decoration: the token alone selects */
    char bare[128];
    snprintf(bare, sizeof(bare), "%.61s", url);
    assert(share_get(bare, h, b) == 200 && b->n == sizeof(png));

    /* The server also hosts /admin, so an agent's file must never run as a
     * page: HTML is served as text, unknown binary as a download. */
    char *hurl = share(&reg, "{\"file\":\"page.html\"}", &failed);
    assert(!failed && share_get(hurl, h, b) == 200);
    assert(strstr((char *)h->d, "Content-Type: text/plain"));
    assert(!strstr((char *)h->d, "text/html"));
    assert(b->n == strlen(html) && memcmp(b->d, html, b->n) == 0);
    char *burl = share(&reg, "{\"file\":\"data.bin\"}", &failed);
    assert(!failed && share_get(burl, h, b) == 200);
    assert(strstr((char *)h->d, "Content-Type: application/octet-stream"));
    assert(strstr((char *)h->d, "Content-Disposition: attachment"));
    assert(b->n == sizeof(blob) && memcmp(b->d, blob, sizeof(blob)) == 0);

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
    assert(failed && strstr(r, "not a regular file"));
    free(r);
    r = share(&reg, "{\"file\":\"data.bin\"}", &failed);
    assert(!failed);
    free(r);
    assert(db_scalar_i64(db, "SELECT count(*) FROM shares WHERE ? = 0;", 0, -1) == 1);

    free(url); free(hurl); free(burl); free(h); free(b);
    tools_free(&reg);
    web_stop();
    db_close(db);
    test_db_clean(cfg.db_path);
    assert(system("rm -rf " SHARE_WS) == 0);
    printf("PASS: test_share\n");
}

int main(void) {
    TEST_INIT();
    test_db_handle_owner();
    test_status_page();
    test_hook_proxy();
    test_admin_dashboard();
    test_share();
    printf("All web tests passed.\n");
    return 0;
}
