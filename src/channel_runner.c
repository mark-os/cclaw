/* channel_runner — universal JS channel process.
 *
 * One single-threaded poll loop; JS is purely reactive and never blocks on
 * the network. All outbound HTTP is described as request shapes
 * (cclaw.send / poll shapes) and executed here on a curl_multi handle.
 * Inbound HTTP arrives pre-parsed from the daemon over a unix socket.
 *
 * JS contract (all handlers optional except onInit):
 *   onInit() -> {poll?: Req}                 start; optional first poll shape
 *   onPoll({status,body,error}) -> {poll?: Req}   poll completed; next shape
 *     (must return the shape synchronously — an async onPoll's pending
 *      promise carries no .poll, which would stop polling)
 *   onRequest(req) -> {status?, body?}       proxied HTTP request from daemon
 *   onOutbox({id,session_id,payload})        agent message to deliver
 *
 *   Req = {method?, url, body?, headers?: ["Name: v"], timeout?}
 *   channel.http(Req + {save_to?}) -> Promise<{status,body,path,bytes,error}>
 *     — channel-initiated API calls and downloads. Always resolves (transport
 *     failure = status 0 + error). save_to streams the body to the channel's
 *     media spool; JS gets a path, the payload never enters the JS heap.
 *   channel.send(Req + {outbox_id?, final?}) — outbox-tracked delivery (and
 *     fire-and-forget without outbox_id); an outbox send is acked on final
 *     2xx, failed/retried otherwise.
 *
 * argv: channel_runner <db_path> <channel_name> */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "channel.h"
#include "channel_api.h"
#include "channel_runner.h"
#include "admin_api.h"
#include "buf.h"
#include "db.h"
#include "extension_manifest.h"
#include "host_match.h"
#include "log.h"
#include "qjs_helpers.h"
#include "secret.h"
#include "util.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define CR_HEAP_SIZE (2 * 1024 * 1024)
#define CR_MAX_INSTRUCTIONS 100000000
#define CR_REQ_MAX (512 * 1024)   /* max proxied request envelope */
#define CR_SEND_TIMEOUT 60L
#define CR_POLL_TIMEOUT 35L       /* slightly longer than TG long-poll */
#define CR_DOWNLOAD_MAX (3L * 1024 * 1024)  /* binary download cap (small-RAM boxes) */
#define MAX_OUTBOX_ATTEMPTS 5
#define CR_CONN_MAX 4             /* live persistent connections per channel */
#define CR_CONN_CONNECT_TIMEOUT 15L  /* default handshake (TCP+TLS+upgrade) cap */
#define CR_CONN_MSG_MAX CR_DOWNLOAD_MAX  /* reassembled WS message cap (same as downloads) */

static volatile sig_atomic_t g_running = 1;
ChannelCtx *g_ctx;

static void handle_signal(int sig) { (void)sig; g_running = 0; }

/* Telegram 429 bodies carry {"parameters":{"retry_after":N}}. Parse via
 * SQLite JSON (hardened against malformed input -> NULL). 0 if absent. */
static int outbox_retry_after(const char *body) {
    if (!body || !g_ctx || !g_ctx->db) return 0;
    int ra = 0;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(g_ctx->db,
            "SELECT json_extract(?1,'$.parameters.retry_after')", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, body, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) ra = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    return ra > 0 ? ra : 0;
}

/* ── QuickJS runtime for channel ───────────────────────────────── */
static QjsRuntime *g_qrt;

/* ── JS value helpers ──────────────────────────────────────────── */

char *get_str_prop(JSContext *ctx, JSValue obj, const char *name) {
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    if (JS_IsException(v) || JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return NULL;
    }
    const char *s = JS_ToCString(ctx, v);
    JS_FreeValue(ctx, v);
    if (!s) return NULL;
    char *r = strdup(s);
    JS_FreeCString(ctx, s);
    return r;
}

int get_int_prop(JSContext *ctx, JSValue obj, const char *name, int dflt) {
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    if (JS_IsException(v) || JS_IsUndefined(v) || JS_IsNull(v)) {
        JS_FreeValue(ctx, v);
        return dflt;
    }
    int i = dflt;
    JS_ToInt32(ctx, &i, v);
    JS_FreeValue(ctx, v);
    return i;
}

/* Eval JS, logging any exception. Drains microtasks and awaits/unwraps a
 * returned promise. A sync throw or a rejected promise lands on the same path:
 * log and return JS_UNDEFINED (channel handlers are fire-and-forget). */
JSValue eval_js(JSContext *ctx, const char *code, const char *tag) {
    qjs_reset_instructions(g_qrt);
    JSValue v = JS_Eval(ctx, code, strlen(code), tag, JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsException(v))
        v = qjs_resolve(ctx, v);
    if (JS_IsException(v)) {
        JSValue exc = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, exc);
        LOG_ERROR_("channel_runner: JS error in %s: %s", tag, msg ? msg : "?");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exc);
        return JS_UNDEFINED;
    }
    return v;
}

/* Ingestion-only detection: a handler with no onOutbox is a pure source.
 * Recorded in channel_state at load time (runner startup and --check) so the
 * daemon can skip outbox inserts for its sessions instead of queueing rows
 * nothing will ever drain. */
static void record_outbox_capability(JSContext *ctx) {
    JSValue v = eval_js(ctx, "typeof onOutbox === 'function' ? '1' : '0'", "<cap>");
    const char *s = JS_IsUndefined(v) ? NULL : JS_ToCString(ctx, v);
    if (s) {
        channel_set_config(g_ctx, "has_outbox", s);
        JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);
}

static void set_global_str(JSContext *ctx, const char *name, const char *val) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, name,
                      val ? JS_NewString(ctx, val) : JS_NULL);
    JS_FreeValue(ctx, global);
}

static void set_global_int(JSContext *ctx, const char *name, int val) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, name, JS_NewInt32(ctx, val));
    JS_FreeValue(ctx, global);
}

/* ── Send queue (filled by cclaw.send, drained by the curl loop) ──
 * SendReq itself is declared in channel_runner.h — shared with
 * channel_harness.c, which drains this same queue but matches fixtures
 * instead of making real curl calls. */

static SendReq *g_send_head, *g_send_tail;

void send_req_free(SendReq *r) {
    if (!r) return;
    free(r->method); free(r->url); free(r->body);
    free(r->save_to);
    if (r->js_ctx) {
        JS_FreeValue(r->js_ctx, r->p_resolve);
        JS_FreeValue(r->js_ctx, r->p_reject);
    }
    for (int i = 0; i < r->n_headers; i++) free(r->headers[i]);
    free(r->headers);
    free(r);
}

void send_req_settle(JSContext *ctx, SendReq *r, int status, const char *body,
                     const char *path, long bytes, const char *error) {
    if (!r->js_ctx) return;
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "status", JS_NewInt32(ctx, status));
    JS_SetPropertyStr(ctx, obj, "body", body ? JS_NewString(ctx, body) : JS_NULL);
    JS_SetPropertyStr(ctx, obj, "path", path ? JS_NewString(ctx, path) : JS_NULL);
    JS_SetPropertyStr(ctx, obj, "bytes", JS_NewInt64(ctx, bytes));
    JS_SetPropertyStr(ctx, obj, "error", error ? JS_NewString(ctx, error) : JS_NULL);
    qjs_reset_instructions(g_qrt);
    JSValue ret = JS_Call(ctx, r->p_resolve, JS_UNDEFINED, 1, (JSValueConst *)&obj);
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, obj);
    /* Run the awaiting continuation (and anything it chains) to its next
     * suspension point. Job errors are logged inside qjs_resolve. */
    JSValue u = qjs_resolve(ctx, JS_UNDEFINED);
    JS_FreeValue(ctx, u);
}

char *channel_save_path(const char *name) {
    if (!g_ctx || !name || !name[0] || name[0] == '.' ||
        strchr(name, '/') || strlen(name) > 128)
        return NULL;
    /* Media spool lives next to the DB: <db_dir>/media/<channel>/<name>.
     * Harness scratch DBs land it in /tmp, the real daemon in ~/.cclaw. */
    const char *dbp = g_ctx->db_path ? g_ctx->db_path : ".";
    const char *slash = strrchr(dbp, '/');
    char dir[1024];
    int wn;
    if (slash)
        wn = snprintf(dir, sizeof(dir), "%.*s/media", (int)(slash - dbp), dbp);
    else
        wn = snprintf(dir, sizeof(dir), "media");
    if (wn < 0 || (size_t)wn >= sizeof(dir)) {
        LOG_ERROR_("channel_runner: media dir path truncated (db_path too long)");
        return NULL;
    }
    mkdir(dir, 0700);
    size_t used = strlen(dir);
    wn = snprintf(dir + used, sizeof(dir) - used, "/%s", g_ctx->channel_name);
    if (wn < 0 || (size_t)wn >= sizeof(dir) - used) {
        LOG_ERROR_("channel_runner: media dir path truncated (channel name too long)");
        return NULL;
    }
    mkdir(dir, 0700);
    char *out = NULL;
    if (asprintf(&out, "%s/%s", dir, name) < 0) return NULL;
    return out;
}

void send_queue_push(SendReq *r) {
    r->next = NULL;
    if (g_send_tail) g_send_tail->next = r;
    else g_send_head = r;
    g_send_tail = r;
}

SendReq *send_queue_pop(void) {
    SendReq *r = g_send_head;
    if (r) {
        g_send_head = r->next;
        if (!g_send_head) g_send_tail = NULL;
    }
    return r;
}

/* Drop queued sends bound to a failed outbox row (skip stale chunks). */
static void send_queue_drop_outbox(int64_t outbox_id) {
    SendReq **pp = &g_send_head;
    while (*pp) {
        SendReq *r = *pp;
        if (r->outbox_id == outbox_id) {
            *pp = r->next;
            if (g_send_tail == r) {
                g_send_tail = NULL;
                for (SendReq *t = g_send_head; t; t = t->next) g_send_tail = t;
            }
            send_req_free(r);
        } else {
            pp = &r->next;
        }
    }
}

/* ── HTTP transfers (poll + sends) on one curl_multi ───────────── */

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    size_t bytes = size * nmemb;
    Buf *b = (Buf *)ud;
    buf_append(b, (const char *)ptr, bytes);
    return b->oom ? 0 : bytes;
}

static CURLM *g_multi;

/* Poller: the recurring long-poll request (shape owned here) */
static struct {
    CURL *easy;
    Buf resp;
    char *method, *url, *body;
    struct curl_slist *hdrs;
    int active;
    int errors;          /* consecutive failures, drives backoff */
    time_t next_at;
} g_poll;

/* One in-flight send at a time preserves per-channel ordering */
static SendReq *g_send_active;
static CURL *g_send_easy;
static Buf g_send_resp;
static struct curl_slist *g_send_hdrs;

/* save_to download in flight: body streams to disk chunk by chunk — the full
 * payload never exists in RAM (or the JS heap). Non-2xx bodies divert to
 * g_send_resp so JS gets a readable error body instead of a junk file. */
static struct {
    FILE *f;
    long written;
    int checked, divert;
} g_save;

static size_t curl_write_save_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    (void)ud;
    size_t bytes = size * nmemb;
    if (!g_save.checked) {
        /* No FOLLOWLOCATION on these handles, so the first body byte's
         * status is the final status. */
        long code = 0;
        curl_easy_getinfo(g_send_easy, CURLINFO_RESPONSE_CODE, &code);
        g_save.divert = (code < 200 || code >= 300);
        g_save.checked = 1;
    }
    if (g_save.divert) {
        buf_append(&g_send_resp, (const char *)ptr, bytes);
        return g_send_resp.oom ? 0 : bytes;
    }
    if (g_save.written + (long)bytes > CR_DOWNLOAD_MAX) return 0; /* abort: too large */
    if (fwrite(ptr, 1, bytes, g_save.f) != bytes) return 0;
    g_save.written += (long)bytes;
    return bytes;
}

/* Pin channel_runner's outbound HTTP (and channel.conn.* URLs) to the
 * channel's egress allowlist — a channel is semantically "talks to one
 * external service," not arbitrary web access, so unlike shell/web_fetch/js
 * tools it gets no general allowlist, just this pin. Uses curl's own URL
 * parser (not a hand-rolled host extractor) so the validated host is exactly
 * what curl will later dial.
 *
 * The allowlist is base_url's host PLUS any entry in an optional
 * comma-separated `egress_hosts` config key. Matching is host_match's: exact
 * by default, and a leading-dot entry (".discord.gg") is an opt-in suffix
 * match at a dot boundary — it covers gateway.discord.gg AND the bare apex
 * discord.gg, but never evildiscord.gg (the match must land on a dot).
 * Suffix entries are safe ONLY on provider-exclusive
 * domains — an operator/review responsibility, not enforceable here (see
 * specs/channel-transports.md §Egress pinning). Fail-closed: no config → no
 * host matches → refused. */
static int url_host_allowed(const char *url) {
    CURLU *tu = curl_url();
    if (!tu) return 0;
    char *th = NULL;
    int ok = 0;
    /* NON_SUPPORT_SCHEME: curl's URL parser rejects ws/wss by default (they are
     * transfer schemes, not URL-API schemes). conn.open URLs are ws/wss, so we
     * must let the parser accept them to extract the host. Harmless for the
     * http(s) send/http paths — only the host is ever used. */
    if (curl_url_set(tu, CURLUPART_URL, url, CURLU_NON_SUPPORT_SCHEME) == CURLUE_OK &&
        curl_url_get(tu, CURLUPART_HOST, &th, 0) == CURLUE_OK && th) {
        char *rules[24];
        size_t nr = 0;

        /* base_url's host is always an (exact) rule */
        char *base = channel_config_get(g_ctx->db, g_ctx->channel_name, "base_url");
        char *bh = NULL;
        if (base && base[0]) {
            CURLU *bu = curl_url();
            if (bu) {
                if (curl_url_set(bu, CURLUPART_URL, base, 0) == CURLUE_OK)
                    curl_url_get(bu, CURLUPART_HOST, &bh, 0);
                curl_url_cleanup(bu);
            }
        }
        if (bh) rules[nr++] = bh;

        /* egress_hosts: comma-/space-separated bare hosts / ".suffix"
         * entries, tokenized in place in the config copy. */
        char *egress = channel_config_get(g_ctx->db, g_ctx->channel_name, "egress_hosts");
        nr += (size_t)split_and_trim(egress, &rules[nr], (int)(24 - nr));

        ok = host_match(rules, nr, th);
        curl_free(bh);
        free(base);
        free(egress);
    }
    curl_free(th);
    curl_url_cleanup(tu);
    return ok;
}

static CURL *make_easy(const char *method, const char *url, const char *body,
                       char **headers, int n_headers, long timeout,
                       Buf *resp, struct curl_slist **out_hdrs) {
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    if (!url_host_allowed(url)) {
        curl_easy_cleanup(c);
        LOG_WARN_("channel_runner: url host mismatch, refusing: %s", url);
        return NULL;
    }
    buf_free(resp);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeout);

    struct curl_slist *hl = NULL;
    int have_ct = 0;
    for (int i = 0; i < n_headers; i++) {
        hl = curl_slist_append(hl, headers[i]);
        if (strncasecmp(headers[i], "Content-Type:", 13) == 0) have_ct = 1;
    }
    if (body) {
        if (!have_ct) hl = curl_slist_append(hl, "Content-Type: application/json");
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    }
    if (hl) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
    if (method && strcmp(method, "GET") != 0 && !body)
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    else if (method && strcmp(method, "POST") != 0 && body)
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    *out_hdrs = hl;
    return c;
}

/* Replace the poll shape from a JS {poll: Req} return value (if present). */
static void poll_shape_update(JSContext *ctx, JSValue ret) {
    if (JS_IsUndefined(ret) || JS_IsNull(ret)) return;
    JSValue p = JS_GetPropertyStr(ctx, ret, "poll");
    if (JS_IsException(p) || JS_IsUndefined(p)) { JS_FreeValue(ctx, p); return; }
    free(g_poll.method); free(g_poll.url); free(g_poll.body);
    g_poll.method = g_poll.url = g_poll.body = NULL;
    if (JS_IsNull(p)) { JS_FreeValue(ctx, p); return; }  /* explicit null stops polling */
    g_poll.url = get_str_prop(ctx, p, "url");
    g_poll.method = get_str_prop(ctx, p, "method");
    g_poll.body = get_str_prop(ctx, p, "body");
    JS_FreeValue(ctx, p);
}

static void poll_start(void) {
    if (g_poll.active || !g_poll.url || !g_poll.url[0]) return;
    if (time(NULL) < g_poll.next_at) return;
    if (g_poll.easy) { curl_multi_remove_handle(g_multi, g_poll.easy); curl_easy_cleanup(g_poll.easy); }
    curl_slist_free_all(g_poll.hdrs); g_poll.hdrs = NULL;
    g_poll.easy = make_easy(g_poll.method, g_poll.url, g_poll.body,
                            NULL, 0, CR_POLL_TIMEOUT, &g_poll.resp, &g_poll.hdrs);
    if (!g_poll.easy) {
        /* Back off like any other poll failure — without this, a poll shape
         * that ever points off-base-url would hot-loop retrying (and
         * re-logging the refusal) every cycle forever. */
        g_poll.errors++;
        int delay = g_poll.errors < 6 ? (1 << g_poll.errors) : 60;
        g_poll.next_at = time(NULL) + delay;
        return;
    }
    curl_multi_add_handle(g_multi, g_poll.easy);
    g_poll.active = 1;
}

static void send_start_next(JSContext *ctx) {
    if (g_send_active) return;
    SendReq *r = send_queue_pop();
    if (!r) return;
    g_send_easy = make_easy(r->method, r->url, r->body, r->headers, r->n_headers,
                            r->timeout > 0 ? r->timeout : CR_SEND_TIMEOUT,
                            &g_send_resp, &g_send_hdrs);
    if (!g_send_easy) {
        if (r->outbox_id > 0) channel_fail_outbox(g_ctx, r->outbox_id, "curl init failed");
        if (r->js_ctx) send_req_settle(ctx, r, 0, NULL, NULL, 0, "request refused");
        send_req_free(r);
        return;
    }
    if (r->save_to) {
        memset(&g_save, 0, sizeof(g_save));
        g_save.f = fopen(r->save_to, "wb");
        if (!g_save.f) {
            curl_easy_cleanup(g_send_easy); g_send_easy = NULL;
            curl_slist_free_all(g_send_hdrs); g_send_hdrs = NULL;
            send_req_settle(ctx, r, 0, NULL, NULL, 0, "cannot open save_to file");
            send_req_free(r);
            return;
        }
        curl_easy_setopt(g_send_easy, CURLOPT_WRITEFUNCTION, curl_write_save_cb);
        curl_easy_setopt(g_send_easy, CURLOPT_WRITEDATA, NULL);
        curl_easy_setopt(g_send_easy, CURLOPT_MAXFILESIZE, CR_DOWNLOAD_MAX);
        curl_easy_setopt(g_send_easy, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)CR_DOWNLOAD_MAX);
    }
    curl_multi_add_handle(g_multi, g_send_easy);
    g_send_active = r;
}

/* ── conn callbacks into JS (all optional) ─────────────────────── */

static void call_on_conn_open(JSContext *ctx, int id) {
    set_global_int(ctx, "__cr_conn_id", id);
    JSValue r = eval_js(ctx,
        "(typeof onConnOpen === 'function') && onConnOpen(__cr_conn_id)", "onConnOpen");
    JS_FreeValue(ctx, r);
}

static void call_on_conn_message(JSContext *ctx, int id, const char *text) {
    set_global_int(ctx, "__cr_conn_id", id);
    set_global_str(ctx, "__cr_conn_msg", text);
    JSValue r = eval_js(ctx,
        "(typeof onConnMessage === 'function') && onConnMessage(__cr_conn_id, __cr_conn_msg)",
        "onConnMessage");
    JS_FreeValue(ctx, r);
}

static void call_on_conn_close(JSContext *ctx, int id, int code) {
    set_global_int(ctx, "__cr_conn_id", id);
    set_global_int(ctx, "__cr_conn_code", code);
    JSValue r = eval_js(ctx,
        "(typeof onConnClose === 'function') && onConnClose(__cr_conn_id, __cr_conn_code)",
        "onConnClose");
    JS_FreeValue(ctx, r);
}

/* onTimer, resolved once at handler load (JS_UNDEFINED when the handler has
 * none). The event loop calls this on every iteration, and curl_multi_poll
 * returns on any readable fd — so under load it runs far more often than its
 * nominal 1s tick. It used to re-parse and evaluate a probe string each time,
 * paying a full JS_Eval even for a handler with no onTimer at all. */
/* Guarded by a flag rather than a JS_UNDEFINED sentinel: JS_UNDEFINED is not a
 * constant initializer here, and a zero-initialized JSValue is tag JS_TAG_INT
 * (integer 0), not undefined. */
static JSValue g_on_timer;
static int g_on_timer_set;

static void resolve_on_timer(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, "onTimer");
    JS_FreeValue(ctx, global);
    if (JS_IsFunction(ctx, fn)) {
        g_on_timer = fn;                 /* keep the reference */
        g_on_timer_set = 1;
    } else {
        JS_FreeValue(ctx, fn);
        g_on_timer_set = 0;
    }
}

static void call_on_timer(JSContext *ctx) {
    if (!g_on_timer_set) return;
    JSValue r = JS_Call(ctx, g_on_timer, JS_UNDEFINED, 0, NULL);
    if (JS_IsException(r)) {
        JSValue exc = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, exc);
        LOG_WARN_("channel_runner: onTimer threw: %s", msg ? msg : "?");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, r);
}

/* ── channel.conn.* — persistent bidirectional connections ────────
 * C owns the transport (WebSocket via libcurl CONNECT_ONLY); the .qjs handler
 * owns the protocol (opcodes, heartbeats, reconnect). The CONNECT_ONLY easy
 * handles are driven MANUALLY — never added to g_multi. Each conn's
 * CURLINFO_ACTIVESOCKET fd rides in the loop's extra[] pollfd array; on
 * readable we call curl_ws_recv() directly, and curl_ws_send() drains the
 * outbound queue. See specs/channel-transports.md. */

typedef struct ConnMsg {           /* one queued outbound text frame */
    char *data;
    size_t len;
    struct ConnMsg *next;
} ConnMsg;

typedef struct {
    CURL *easy;                    /* NULL = free slot */
    curl_socket_t fd;             /* CURLINFO_ACTIVESOCKET, polled in extra[] */
    Buf msg;                       /* inbound reassembly (across reads + fragments) */
    int msg_text;                  /* the message being reassembled is TEXT */
    int msg_over;                  /* message blew CR_CONN_MSG_MAX — drain, don't buffer */
    ConnMsg *out_head, *out_tail;  /* outbound backpressure queue */
    int pending_open;              /* fire onConnOpen on next iteration */
    int want_close;                /* reap: free + fire onConnClose */
    int close_code;
} Conn;

static Conn g_conn[CR_CONN_MAX];

/* Set only for the duration of channel_runner_check()'s onInit() call — makes
 * conn.open validate without dialing (see cr_conn_open). */
static int g_check_mode;

static void conn_free_slot(int i) {
    Conn *c = &g_conn[i];
    if (c->easy) curl_easy_cleanup(c->easy);
    ConnMsg *m = c->out_head;
    while (m) { ConnMsg *n = m->next; free(m->data); free(m); m = n; }
    buf_free(&c->msg);
    memset(c, 0, sizeof(*c));
}

/* Drain the outbound queue with curl_ws_send. Backpressure (CURLE_AGAIN)
 * stops the drain — the head retries on the next tick. Sends are atomic:
 * realistic control/heartbeat/reply frames sit far below the socket buffer,
 * so a non-OK-or-partial result means a broken stream — mark for close and
 * let the loop reap it (the handler reconnects). No JS is called from here
 * (this runs on the JS send path too), so close is deferred, never
 * re-entrant. */
static void conn_flush(int i) {
    Conn *c = &g_conn[i];
    while (c->out_head && !c->want_close) {
        ConnMsg *m = c->out_head;
        size_t sent = 0;
        CURLcode res = curl_ws_send(c->easy, m->data, m->len, &sent, 0, CURLWS_TEXT);
        if (res == CURLE_AGAIN) return;
        if (res != CURLE_OK || sent != m->len) {
            LOG_WARN_("channel_runner: conn %d send failed: %s (sent=%zu/%zu)",
                      i + 1, curl_easy_strerror(res), sent, m->len);
            c->want_close = 1;
            return;
        }
        c->out_head = m->next;
        if (!c->out_head) c->out_tail = NULL;
        free(m->data); free(m);
    }
}

/* Read every available frame until CURLE_AGAIN, reassembling a message before
 * dispatch. Two separate splits both have to be joined: a single frame split
 * across reads (curl_ws_frame.bytesleft > 0) and a message split across frames
 * (RFC 6455 fragmentation — CURLWS_CONT means "more fragments follow"). A
 * message is complete only when the current frame is drained AND is the final
 * fragment. C consumes control frames — PING/PONG are curl's business (autopong
 * is on by default), a CLOSE becomes a deferred onConnClose. Only reassembled
 * TEXT reaches onConnMessage (v1 is text/json; BINARY is dropped).
 *
 * Reassembly is capped at CR_CONN_MSG_MAX: the WS length field is 64-bit and
 * libcurl imposes no message ceiling, so an allowlisted-but-hostile peer could
 * otherwise stream us out of RAM on a 128MB box. Over-cap messages are dropped
 * with a warning, and the rest of the message is drained without buffering. */
static void conn_recv_drain(JSContext *ctx, int i) {
    Conn *c = &g_conn[i];
    char buf[16384];
    for (;;) {
        size_t rlen = 0;
        const struct curl_ws_frame *meta = NULL;
        CURLcode res = curl_ws_recv(c->easy, buf, sizeof(buf), &rlen, &meta);
        if (res == CURLE_AGAIN) return;
        if (res != CURLE_OK) { c->want_close = 1; c->close_code = 0; return; }
        if (!meta) continue;
        if (meta->flags & CURLWS_CLOSE) {
            /* Close payload (if any) is a 2-byte big-endian status code. */
            c->close_code = (rlen >= 2)
                ? (((unsigned char)buf[0] << 8) | (unsigned char)buf[1]) : 0;
            c->want_close = 1;
            return;
        }
        if (meta->flags & (CURLWS_PING | CURLWS_PONG)) continue;
        if (c->msg.len == 0 && !c->msg_over)
            c->msg_text = (meta->flags & CURLWS_TEXT) != 0;
        if (rlen) {
            if (c->msg.len + rlen > (size_t)CR_CONN_MSG_MAX) {
                if (!c->msg_over)
                    LOG_WARN_("channel_runner: conn %d message exceeds %ld bytes — dropping",
                              i + 1, CR_CONN_MSG_MAX);
                c->msg_over = 1;
                buf_free(&c->msg);                   /* stop holding the prefix */
            } else if (!c->msg_over) {
                buf_append(&c->msg, buf, rlen);
            }
        }
        if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) {  /* message complete */
            if (!c->msg_over && c->msg_text && !c->msg.oom) {
                buf_append_char(&c->msg, '\0');      /* NUL-terminate for JS */
                if (!c->msg.oom) call_on_conn_message(ctx, i + 1, c->msg.data);
            }
            /* Reset, don't free: the next message reuses this capacity. A
             * steady stream of ~6KB events otherwise pays a full realloc ramp
             * plus a free per message. conn_free_slot still frees it, and the
             * over-cap branch above frees deliberately (never sit on 3MB). */
            c->msg.len = 0;
            c->msg.oom = 0;
            c->msg_over = 0;
        }
    }
}

/* Open a persistent connection. framing defaults to "ws"; "raw" is a planned
 * second framing (specs/channel-transports.md) not yet implemented. The
 * handshake is synchronous — curl_easy_perform drives the TCP+TLS+WS upgrade
 * and returns once connected. Returns an id >= 1, or -1 on any failure. */
int cr_conn_open(const char *url, const char *framing,
                 char **headers, int n_headers, long timeout) {
    if (!url || !url[0]) return -1;
    if (framing && framing[0] && strcmp(framing, "ws") != 0) {
        LOG_WARN_("channel_runner: conn framing '%s' not supported (ws only)", framing);
        return -1;
    }
    if (!url_host_allowed(url)) {
        LOG_WARN_("channel_runner: conn.open host not allowed, refusing: %s", url);
        return -1;
    }
    /* --check is static validation: it runs onInit() but never enters the event
     * loop. Dialing for real would make a health check depend on the provider
     * being reachable (a transient outage would refuse activation) and would
     * leave a live TLS session behind, since the check path never reaps conns.
     * Validate the spec, report a plausible id, open nothing. */
    if (g_check_mode) {
        LOG_INFO_("channel_runner: --check: conn.open validated, not dialed: %s", url);
        return 1;
    }
    int slot = -1;
    for (int i = 0; i < CR_CONN_MAX; i++) if (!g_conn[i].easy) { slot = i; break; }
    if (slot < 0) { LOG_WARN_("channel_runner: conn table full (max %d)", CR_CONN_MAX); return -1; }

    CURL *c = curl_easy_init();
    if (!c) return -1;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 2L);   /* 2 = WebSocket */
    long cap = timeout > 0 ? timeout : CR_CONN_CONNECT_TIMEOUT;
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, cap);
    /* CONNECTTIMEOUT alone is not enough: it only covers TCP+TLS, while the
     * HTTP Upgrade → 101 exchange runs in the transfer phase. A peer that
     * completes TLS and then says nothing would block curl_easy_perform
     * forever — wedging the single-threaded runner (outbox, UDS, every other
     * conn) with only SIGKILL to end it. TIMEOUT caps the whole handshake;
     * it is cleared below so it can never expire the long-lived stream. */
    curl_easy_setopt(c, CURLOPT_TIMEOUT, cap);
    struct curl_slist *hl = NULL;
    for (int i = 0; i < n_headers; i++) hl = curl_slist_append(hl, headers[i]);
    if (hl) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);

    CURLcode res = curl_easy_perform(c);             /* blocking handshake */
    curl_slist_free_all(hl);
    if (res != CURLE_OK) {
        LOG_WARN_("channel_runner: conn.open handshake failed: %s", curl_easy_strerror(res));
        curl_easy_cleanup(c);
        return -1;
    }
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 0L);        /* handshake done — no cap on the stream */
    curl_socket_t sock = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(c, CURLINFO_ACTIVESOCKET, &sock) != CURLE_OK ||
        sock == CURL_SOCKET_BAD) {
        curl_easy_cleanup(c);
        return -1;
    }
    Conn *cn = &g_conn[slot];
    memset(cn, 0, sizeof(*cn));
    cn->easy = c;
    cn->fd = sock;
    cn->pending_open = 1;
    LOG_INFO_("channel_runner: conn %d open: %s", slot + 1, url);
    return slot + 1;
}

/* Queue a text frame; drain opportunistically. Returns 0 if id is unknown or
 * the conn is closed. */
int cr_conn_send(int id, const char *text) {
    int i = id - 1;
    if (i < 0 || i >= CR_CONN_MAX || !g_conn[i].easy || g_conn[i].want_close) return 0;
    Conn *c = &g_conn[i];
    ConnMsg *m = calloc(1, sizeof(*m));
    if (!m) return 0;
    size_t len = text ? strlen(text) : 0;
    m->data = malloc(len ? len : 1);
    if (!m->data) { free(m); return 0; }
    if (len) memcpy(m->data, text, len);
    m->len = len;
    if (c->out_tail) c->out_tail->next = m; else c->out_head = m;
    c->out_tail = m;
    conn_flush(i);
    return 1;
}

/* Graceful close: best-effort CLOSE frame to the peer, then defer teardown +
 * onConnClose to the loop (never re-entrant with the calling JS handler). */
void cr_conn_close(int id) {
    int i = id - 1;
    if (i < 0 || i >= CR_CONN_MAX || !g_conn[i].easy) return;
    size_t sent = 0;
    curl_ws_send(g_conn[i].easy, "", 0, &sent, 0, CURLWS_CLOSE);
    g_conn[i].want_close = 1;
    g_conn[i].close_code = 1000;   /* normal closure */
}

/* ── JS handler call sites ─────────────────────────────────────── */

void call_on_outbox(JSContext *ctx, ChannelOutboxRow *row) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "id", JS_NewInt32(ctx, (int32_t)row->id));
    JS_SetPropertyStr(ctx, obj, "session_id", JS_NewInt32(ctx, (int32_t)row->session_id));
    JS_SetPropertyStr(ctx, obj, "payload",
        JS_NewString(ctx, row->payload ? row->payload : ""));
    JS_SetPropertyStr(ctx, obj, "mode", JS_NewInt32(ctx, row->deliver_mode));
    JS_SetPropertyStr(ctx, global, "__cr_outbox_item", obj);
    JSValue ret = eval_js(ctx, "onOutbox(__cr_outbox_item)", "onOutbox");
    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, global);
}

static void drain_outbox(JSContext *ctx) {
    ChannelOutboxRow *row;
    while ((row = channel_next_outbox(g_ctx)) != NULL) {
        /* Move to 'sending' first — the send is async, so a still-pending
         * row would be re-fetched forever */
        channel_dispatch_outbox(g_ctx, row->id);
        call_on_outbox(ctx, row);
        channel_outbox_row_free(row);
    }
}

static void call_on_poll_done(JSContext *ctx, int status, const char *body,
                              const char *error) {
    set_global_int(ctx, "__cr_status", status);
    set_global_str(ctx, "__cr_body", body);
    set_global_str(ctx, "__cr_err", error);
    JSValue ret = eval_js(ctx,
        "(typeof onPoll === 'function')"
        " ? onPoll({status: __cr_status, body: __cr_body, error: __cr_err}) : null",
        "onPoll");
    poll_shape_update(ctx, ret);
    JS_FreeValue(ctx, ret);
}

/* ── Proxied requests over UDS (from the daemon) ───────────────── */

static int uds_listen_open(const char *db_path, const char *name) {
    char *path = channel_uds_path(db_path, name);
    if (!path) return -1;
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { free(path); return -1; }
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    int wn = snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    if (wn < 0 || (size_t)wn >= sizeof(sa.sun_path)) {
        /* Truncated sun_path would bind a *different* socket and listen on it
         * happily — fail loud instead of serving the wrong address. */
        LOG_ERROR_("channel_runner: UDS path too long (%s)", path);
        free(path);
        close(fd);
        return -1;
    }
    free(path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

/* Read the full envelope (daemon writes then shuts down its end), run
 * onRequest, write "status\nbody" back. One request per connection. */
static void uds_handle_conn(JSContext *ctx, int cfd) {
    struct timeval tv = {5, 0};
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    if (!buf) { close(cfd); return; }
    for (;;) {
        if (len + 4096 + 1 > cap) {
            if (cap >= CR_REQ_MAX) break;
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) break;
            buf = tmp;
        }
        ssize_t n = read(cfd, buf + len, 4096);
        if (n > 0) { len += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        break; /* EOF or error/timeout */
    }
    buf[len] = '\0';

    set_global_str(ctx, "__cr_req", buf);
    free(buf);

    /* The wrapper builds the "status\nbody" wire reply directly. */
    JSValue ret = eval_js(ctx,
        "(function(){\n"
        "  try {\n"
        "    var r = (typeof onRequest === 'function')"
        "            ? onRequest(JSON.parse(__cr_req)) : null;\n"
        "    if (r == null) return '200\\nok';\n"
        "    if (typeof r === 'string') return '200\\n' + r;\n"
        "    return (r.status || 200) + '\\n' + (r.body == null ? '' : '' + r.body);\n"
        "  } catch (e) { return '500\\n' + e; }\n"
        "})()",
        "onRequest");

    const char *reply = JS_ToCString(ctx, ret);
    int reply_owned = (reply != NULL);
    if (!reply) reply = "500\n";
    size_t rlen = strlen(reply), off = 0;
    while (off < rlen) {
        ssize_t n = write(cfd, reply + off, rlen - off);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
        off += (size_t)n;
    }
    if (reply_owned) JS_FreeCString(ctx, reply);
    JS_FreeValue(ctx, ret);
    close(cfd);
}

/* ── Main ──────────────────────────────────────────────────────── */

/* Resolve js_path from extensions table, shared by the live runner and
 * --check. A channel row's extension path is a directory (channel.qjs
 * inside) unless it points straight at a file. */
int resolve_js_path(sqlite3 *db, const char *channel_name, char *out, size_t outlen) {
    out[0] = '\0';
    const char *sql = "SELECT e.path FROM channels c"
                      " JOIN extensions e ON c.extension_name=e.name"
                      " WHERE c.name=?;";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(s, 1, channel_name, -1, SQLITE_STATIC);
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *p = (const char *)sqlite3_column_text(s, 0);
        if (p) {
            struct stat st;
            if (stat(p, &st) == 0 && S_ISREG(st.st_mode))
                snprintf(out, outlen, "%s", p);
            else
                snprintf(out, outlen, "%s/channel.qjs", p);
        }
    }
    sqlite3_finalize(s);
    return out[0] ? 0 : -1;
}

int channel_runner_main(const char *db_path, const char *channel_name) {
    /* Logging is already initialized by main() before the --channel branch. */
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    g_ctx = channel_ctx_open(db_path, channel_name);
    if (!g_ctx) { LOG_ERROR_("channel_runner: DB open failed"); return 1; }

    /* Load the secret key so extension JS touching the encrypted kv works. */
    {
        uint8_t sk[32];
        if (secret_key_load_or_create(db_path, sk) == 0)
            db_set_secret_key(sk);
    }

    char js_path[1024];
    if (resolve_js_path(g_ctx->db, channel_name, js_path, sizeof(js_path)) != 0) {
        LOG_ERROR_("channel_runner: no extension path for channel '%s'", channel_name);
        channel_ctx_free(g_ctx);
        return 1;
    }

    int outbox_fd = channel_outbox_fifo_open(db_path, channel_name);
    if (outbox_fd < 0)
        LOG_WARN_("channel_runner: outbox FIFO unavailable");

    int uds_fd = uds_listen_open(db_path, channel_name);
    if (uds_fd < 0)
        LOG_WARN_("channel_runner: request socket unavailable");

    char *js_src = util_read_file(js_path, NULL);
    if (!js_src) {
        LOG_ERROR_("channel_runner: cannot read %s", js_path);
        channel_ctx_free(g_ctx);
        return 1;
    }

    g_qrt = qjs_runtime_create(CR_HEAP_SIZE);
    if (!g_qrt) { free(js_src); channel_ctx_free(g_ctx); return 1; }
    qjs_set_interrupt_limit(g_qrt, CR_MAX_INSTRUCTIONS);
    JSContext *ctx = qjs_context_create(g_qrt, QJS_PROFILE_CHANNEL);
    if (!ctx) { qjs_runtime_destroy(g_qrt); free(js_src); channel_ctx_free(g_ctx); return 1; }

    /* Register channel host functions (cclaw.*, admin.*) */
    qjs_register_channel_host_functions(ctx);

    JSValue load_val = JS_Eval(ctx, js_src, strlen(js_src), js_path, JS_EVAL_TYPE_GLOBAL);
    free(js_src);
    if (JS_IsException(load_val)) {
        JSValue exc = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, exc);
        LOG_ERROR_("channel_runner: JS load error: %s", msg ? msg : "?");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exc);
        JS_FreeContext(ctx); qjs_runtime_destroy(g_qrt); channel_ctx_free(g_ctx);
        return 1;
    }
    JS_FreeValue(ctx, load_val);
    record_outbox_capability(ctx);
    resolve_on_timer(ctx);

    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_multi = curl_multi_init();

    /* onInit: required; may set a poll shape and queue sends */
    JSValue init_ret = eval_js(ctx, "onInit()", "<init>");
    if (JS_IsUndefined(init_ret)) {
        LOG_ERROR_("channel_runner: onInit failed");
        curl_multi_cleanup(g_multi); curl_global_cleanup();
        JS_FreeContext(ctx); qjs_runtime_destroy(g_qrt); channel_ctx_free(g_ctx);
        return 1;
    }
    poll_shape_update(ctx, init_ret);
    JS_FreeValue(ctx, init_ret);

    LOG_INFO_("channel_runner: started: channel=%s poll=%s requests=%s",
            channel_name, g_poll.url ? "yes" : "no", uds_fd >= 0 ? "yes" : "no");

    /* Recover rows a crashed predecessor left mid-send, then deliver
     * anything queued before startup */
    channel_reset_outbox(g_ctx);
    drain_outbox(ctx);

    /* ── Event loop: curl transfers + outbox FIFO + request UDS ── */
    while (g_running) {
        int still = 0;
        curl_multi_perform(g_multi, &still);
        send_start_next(ctx);
        poll_start();

        struct curl_waitfd extra[2 + CR_CONN_MAX];
        int n_extra = 0;
        int outbox_slot = -1, uds_slot = -1;
        if (outbox_fd >= 0) {
            outbox_slot = n_extra;
            extra[n_extra].fd = outbox_fd;
            extra[n_extra].events = CURL_WAIT_POLLIN;
            extra[n_extra].revents = 0;
            n_extra++;
        }
        if (uds_fd >= 0) {
            uds_slot = n_extra;
            extra[n_extra].fd = uds_fd;
            extra[n_extra].events = CURL_WAIT_POLLIN;
            extra[n_extra].revents = 0;
            n_extra++;
        }
        /* Persistent conns: each drives itself (not in g_multi), so its socket
         * must be polled here. The 1000ms timeout below is also the onTimer
         * tick, so heartbeats fire even when every fd is idle. */
        int conn_idx[CR_CONN_MAX], n_conn = 0;
        for (int i = 0; i < CR_CONN_MAX; i++) {
            if (!g_conn[i].easy) continue;
            conn_idx[n_conn++] = i;
            extra[n_extra].fd = g_conn[i].fd;
            extra[n_extra].events = CURL_WAIT_POLLIN;
            extra[n_extra].revents = 0;
            n_extra++;
        }

        int numfds = 0;
        curl_multi_poll(g_multi, extra, (unsigned)n_extra, 1000, &numfds);

        /* Re-drain outbox every iteration: rescheduled rows become due when
         * next_attempt_at passes. channel_next_outbox filters by time so this
         * is a cheap no-op when nothing is ready. */
        drain_outbox(ctx);

        if (outbox_slot >= 0 && (extra[outbox_slot].revents & CURL_WAIT_POLLIN)) {
            char drain[64];
            while (read(outbox_fd, drain, sizeof(drain)) > 0) {}
            drain_outbox(ctx);
        }

        if (uds_slot >= 0 && (extra[uds_slot].revents & CURL_WAIT_POLLIN)) {
            for (;;) {
                int cfd = accept(uds_fd, NULL, NULL);
                if (cfd < 0) break;
                uds_handle_conn(ctx, cfd);
            }
        }

        /* Persistent conns: fire pending onConnOpen, drain readable sockets,
         * flush outbound, tick onTimer, then reap any closed. onConnOpen
         * before recv guarantees "open before first message"; reap last so a
         * close raised during onTimer is handled this same iteration. */
        for (int k = 0; k < n_conn; k++) {
            int i = conn_idx[k];
            if (g_conn[i].pending_open) {
                g_conn[i].pending_open = 0;
                call_on_conn_open(ctx, i + 1);
            }
        }
        /* Drain unconditionally, not just on POLLIN: a CONNECT_ONLY handle can
         * hold already-received frames in curl's own buffer (e.g. a HELLO that
         * arrived during the handshake), which leaves the OS fd non-readable —
         * gating on revents would strand them until the next inbound byte. The
         * fd is still in extra[] so poll wakes us promptly on fresh data;
         * curl_ws_recv returns CURLE_AGAIN cheaply when empty. */
        for (int k = 0; k < n_conn; k++)
            conn_recv_drain(ctx, conn_idx[k]);
        for (int i = 0; i < CR_CONN_MAX; i++)
            if (g_conn[i].easy) conn_flush(i);
        call_on_timer(ctx);
        for (int i = 0; i < CR_CONN_MAX; i++) {
            if (g_conn[i].easy && g_conn[i].want_close) {
                int code = g_conn[i].close_code;
                conn_free_slot(i);              /* free first so onConnClose can reopen */
                call_on_conn_close(ctx, i + 1, code);
            }
        }

        /* Completed transfers */
        CURLMsg *msg;
        int msgs_left;
        while ((msg = curl_multi_info_read(g_multi, &msgs_left)) != NULL) {
            if (msg->msg != CURLMSG_DONE) continue;
            long status = 0;
            const char *cerr = (msg->data.result == CURLE_OK)
                               ? NULL : curl_easy_strerror(msg->data.result);
            if (!cerr) curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &status);

            if (msg->easy_handle == g_poll.easy) {
                int ok = (!cerr && status >= 200 && status < 400);
                call_on_poll_done(ctx, (int)status,
                                  g_poll.resp.data ? g_poll.resp.data : "", cerr);
                curl_multi_remove_handle(g_multi, g_poll.easy);
                curl_easy_cleanup(g_poll.easy);
                g_poll.easy = NULL;
                g_poll.active = 0;
                if (ok) {
                    g_poll.errors = 0;
                    g_poll.next_at = 0;
                } else {
                    /* Back off so a dead endpoint doesn't hot-loop */
                    g_poll.errors++;
                    int delay = g_poll.errors < 6 ? (1 << g_poll.errors) : 60;
                    g_poll.next_at = time(NULL) + delay;
                }
            } else if (msg->easy_handle == g_send_easy) {
                SendReq *r = g_send_active;
                int ok = (!cerr && status >= 200 && status < 300);
                if (r->outbox_id > 0) {
                    if (!ok) {
                        /* Transient = network/curl error (cerr set), 429, or 5xx. Everything
                         * else (4xx) is terminal — with one generic exception below. */
                        int transient = (cerr != NULL) || status == 429 || (status >= 500 && status < 600);
                        int attempts = channel_outbox_attempts(g_ctx, r->outbox_id);
                        /* Always clear remaining queued chunks for this row: on retry the row
                         * is re-drained and re-chunked from scratch (accepting at-least-once
                         * duplication of earlier chunks over permanent truncation). */
                        send_queue_drop_outbox(r->outbox_id);
                        if (!transient && channel_outbox_mode(g_ctx, r->outbox_id) == 0) {
                            /* Terminal 4xx on a formatted (mode 0) delivery: re-deliver
                             * once at plain before failing. C needs no platform knowledge
                             * of WHY the format was rejected — the platform's renderer is
                             * the channel handler's business (it sees item.mode). A 4xx
                             * whose cause isn't the formatting wastes one plain retry and
                             * then fails below. The ladder only descends, so no loop. */
                            channel_retry_outbox_mode(g_ctx, r->outbox_id, 1);
                            LOG_WARN_("outbox formatted delivery rejected %ld id=%lld"
                                      " -> retry plain", status, (long long)r->outbox_id);
                        } else if (transient && attempts < MAX_OUTBOX_ATTEMPTS) {
                            int delay = (status == 429) ? outbox_retry_after(g_send_resp.data) : 0;
                            if (delay <= 0) { delay = 1 << attempts; if (delay > 60) delay = 60; }
                            channel_retry_outbox(g_ctx, r->outbox_id, delay);
                            LOG_WARN_("outbox retry id=%lld attempt=%d delay=%ds status=%ld",
                                      (long long)r->outbox_id, attempts + 1, delay, status);
                        } else {
                            char err[256];
                            snprintf(err, sizeof(err), "%s",
                                     cerr ? cerr : (g_send_resp.data ? g_send_resp.data : "http error"));
                            channel_fail_outbox(g_ctx, r->outbox_id, err);
                            LOG_WARN_("outbox failed id=%lld attempts=%d status=%ld: %s",
                                      (long long)r->outbox_id, attempts, status, err);
                        }
                    } else if (r->is_final) {
                        channel_ack_outbox(g_ctx, r->outbox_id);
                    }
                }
                if (r->js_ctx) {
                    if (r->save_to) {
                        if (g_save.f) { fclose(g_save.f); g_save.f = NULL; }
                        int dl_ok = (!cerr && status >= 200 && status < 300 &&
                                     !g_save.divert);
                        if (dl_ok) {
                            send_req_settle(ctx, r, (int)status, NULL,
                                            r->save_to, g_save.written, NULL);
                        } else {
                            unlink(r->save_to);
                            send_req_settle(ctx, r, (int)status,
                                            g_send_resp.data ? g_send_resp.data : NULL,
                                            NULL, 0,
                                            cerr ? cerr :
                                            (g_save.divert ? NULL : "download failed"));
                        }
                    } else {
                        send_req_settle(ctx, r, (int)status,
                                        g_send_resp.data ? g_send_resp.data : "",
                                        NULL, 0, cerr);
                    }
                }
                curl_multi_remove_handle(g_multi, g_send_easy);
                curl_easy_cleanup(g_send_easy);
                curl_slist_free_all(g_send_hdrs);
                g_send_easy = NULL;
                g_send_hdrs = NULL;
                send_req_free(r);
                g_send_active = NULL;
            }
        }
    }

    /* ── Cleanup ───────────────────────────────────────────────── */
    for (int i = 0; i < CR_CONN_MAX; i++) if (g_conn[i].easy) conn_free_slot(i);
    if (g_poll.easy) { curl_multi_remove_handle(g_multi, g_poll.easy); curl_easy_cleanup(g_poll.easy); }
    curl_slist_free_all(g_poll.hdrs);
    free(g_poll.method); free(g_poll.url); free(g_poll.body);
    buf_free(&g_poll.resp);
    if (g_send_easy) { curl_multi_remove_handle(g_multi, g_send_easy); curl_easy_cleanup(g_send_easy); }
    curl_slist_free_all(g_send_hdrs);
    if (g_save.f) { fclose(g_save.f); g_save.f = NULL; }
    send_req_free(g_send_active);
    buf_free(&g_send_resp);
    SendReq *r;
    while ((r = send_queue_pop()) != NULL) send_req_free(r);
    curl_multi_cleanup(g_multi);
    curl_global_cleanup();

    if (uds_fd >= 0) {
        close(uds_fd);
        char *sp = channel_uds_path(db_path, channel_name);
        if (sp) { unlink(sp); free(sp); }
    }
    if (outbox_fd >= 0)
        channel_outbox_fifo_close(outbox_fd, db_path, channel_name);
    if (g_on_timer_set) {                /* held reference — release before the ctx */
        JS_FreeValue(ctx, g_on_timer);
        g_on_timer_set = 0;
    }
    JS_FreeContext(ctx);
    qjs_runtime_destroy(g_qrt);
    channel_ctx_free(g_ctx);
    LOG_INFO_("channel_runner: stopped");
    return 0;
}


/* ── --check: static validation gate ──────────────────────────────
 * Reuses the manifest check + JS-load + onInit() sequence from
 * channel_runner_main, but never opens the outbox FIFO / request UDS and
 * never enters the event loop — nothing onInit queues actually goes out.
 * The caller (main.c) performs the draft/broken → validated DB transition
 * on success; this function only reports pass/fail. */
int channel_runner_check(const char *db_path, const char *channel_name, char **err_out) {
    if (err_out) *err_out = NULL;

    g_ctx = channel_ctx_open(db_path, channel_name);
    if (!g_ctx) { if (err_out) *err_out = strdup("DB open failed"); return -1; }

    /* extension_install copies extension.json alongside channel.qjs into the
     * store dir, so e.path doubles as the bundle_dir manifest validation wants. */
    char store_dir[1024] = {0};
    {
        const char *sql = "SELECT e.path FROM channels c"
                          " JOIN extensions e ON c.extension_name=e.name"
                          " WHERE c.name=?;";
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(g_ctx->db, sql, -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, channel_name, -1, SQLITE_STATIC);
            if (sqlite3_step(s) == SQLITE_ROW) {
                const char *p = (const char *)sqlite3_column_text(s, 0);
                if (p) snprintf(store_dir, sizeof(store_dir), "%s", p);
            }
            sqlite3_finalize(s);
        }
    }
    if (!store_dir[0]) {
        if (err_out) *err_out = strdup("no extension registered for this channel");
        channel_ctx_free(g_ctx); g_ctx = NULL;
        return -1;
    }
    if (extension_manifest_validate(store_dir, err_out) != 0) {
        channel_ctx_free(g_ctx); g_ctx = NULL;
        return -1;
    }

    /* Fail-closed on transport capability: a channel that declares the
     * "persistent" transport needs a WS-capable libcurl. Refuse at activation
     * with a clear message rather than letting the live runner throw at the
     * first conn.open (which surfaces only as a JS onInit failure in the log).
     * The URLs themselves are runtime values, so `transports` is advisory —
     * this is the one thing C validates it for. */
    if (extension_manifest_declares_persistent(g_ctx->db, store_dir) && !curl_ws_available()) {
        if (err_out) *err_out = strdup(
            "channel declares transport 'persistent' but this libcurl has no "
            "ws/wss support (libcurl-minimal?) — install a WS-capable libcurl");
        channel_ctx_free(g_ctx); g_ctx = NULL;
        return -1;
    }

    char js_path[1024];
    if (resolve_js_path(g_ctx->db, channel_name, js_path, sizeof(js_path)) != 0) {
        if (err_out) *err_out = strdup("no channel.qjs resolved");
        channel_ctx_free(g_ctx); g_ctx = NULL;
        return -1;
    }

    char *js_src = util_read_file(js_path, NULL);
    if (!js_src) {
        if (err_out) {
            char b[1200];
            snprintf(b, sizeof(b), "cannot read %s", js_path);
            *err_out = strdup(b);
        }
        channel_ctx_free(g_ctx); g_ctx = NULL;
        return -1;
    }

    /* Secret key so encrypted-kv access doesn't fail if onInit touches it. */
    { uint8_t sk[32]; if (secret_key_load_or_create(db_path, sk) == 0) db_set_secret_key(sk); }

    g_qrt = qjs_runtime_create(CR_HEAP_SIZE);
    if (!g_qrt) {
        free(js_src);
        if (err_out) *err_out = strdup("qjs runtime init failed");
        channel_ctx_free(g_ctx); g_ctx = NULL;
        return -1;
    }
    qjs_set_interrupt_limit(g_qrt, CR_MAX_INSTRUCTIONS);
    JSContext *ctx = qjs_context_create(g_qrt, QJS_PROFILE_CHANNEL);
    if (!ctx) {
        free(js_src);
        qjs_runtime_destroy(g_qrt); g_qrt = NULL;
        if (err_out) *err_out = strdup("qjs context init failed");
        channel_ctx_free(g_ctx); g_ctx = NULL;
        return -1;
    }
    qjs_register_channel_host_functions(ctx);

    JSValue load_val = JS_Eval(ctx, js_src, strlen(js_src), js_path, JS_EVAL_TYPE_GLOBAL);
    free(js_src);
    int rc = 0;
    if (JS_IsException(load_val)) {
        JSValue exc = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, exc);
        if (err_out) {
            char b[512];
            snprintf(b, sizeof(b), "JS load error: %s", msg ? msg : "?");
            *err_out = strdup(b);
        }
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exc);
        rc = -1;
    } else {
        JS_FreeValue(ctx, load_val);
        record_outbox_capability(ctx);
        curl_global_init(CURL_GLOBAL_DEFAULT);
        g_multi = curl_multi_init();
        g_check_mode = 1;
        JSValue init_ret = eval_js(ctx, "onInit()", "<check-init>");
        g_check_mode = 0;
        if (JS_IsUndefined(init_ret)) {
            if (err_out) *err_out = strdup("onInit() failed or threw");
            rc = -1;
        } else {
            JS_FreeValue(ctx, init_ret);
        }
        /* Never enters the event loop — drop anything onInit queued so
         * nothing actually goes out over the network. Conns are validated
         * rather than dialed under g_check_mode, but reap the table anyway:
         * curl_global_cleanup() with a live easy handle is undefined. */
        SendReq *r;
        while ((r = send_queue_pop()) != NULL) send_req_free(r);
        for (int i = 0; i < CR_CONN_MAX; i++) if (g_conn[i].easy) conn_free_slot(i);
        curl_multi_cleanup(g_multi); g_multi = NULL;
        curl_global_cleanup();
    }

    JS_FreeContext(ctx);
    qjs_runtime_destroy(g_qrt); g_qrt = NULL;
    channel_ctx_free(g_ctx); g_ctx = NULL;
    return rc;
}
