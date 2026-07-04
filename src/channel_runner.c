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
 *   onRequest(req) -> {status?, body?}       proxied HTTP request from daemon
 *   onOutbox({id,session_id,payload})        agent message to deliver
 *   onResult({tag,status,body,error})        tagged cclaw.send completed
 *
 *   Req = {method?, url, body?, headers?: ["Name: v"], timeout?}
 *   cclaw.send(Req + {tag?, outbox_id?, final?}) queues an outbound request.
 *   A send carrying outbox_id is acked on final 2xx, failed otherwise.
 *
 * argv: channel_runner <db_path> <channel_name> */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "channel_api.h"
#include "channel_runner.h"
#include "admin_api.h"
#include "buf.h"
#include "db.h"
#include "extension_manifest.h"
#include "log.h"
#include "qjs_helpers.h"
#include "secret.h"
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

static volatile sig_atomic_t g_running = 1;
ChannelCtx *g_ctx;

static void handle_signal(int sig) { (void)sig; g_running = 0; }

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

void set_global_str(JSContext *ctx, const char *name, const char *val) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, name,
                      val ? JS_NewString(ctx, val) : JS_NULL);
    JS_FreeValue(ctx, global);
}

void set_global_int(JSContext *ctx, const char *name, int val) {
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
    free(r->method); free(r->url); free(r->body); free(r->tag);
    for (int i = 0; i < r->n_headers; i++) free(r->headers[i]);
    free(r->headers);
    free(r);
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

/* Pin channel_runner's outbound HTTP to the channel's own configured
 * base_url — a channel is semantically "talks to one external service," not
 * arbitrary web access, so unlike shell/web_fetch/js tools it gets no
 * allowlist, just this single-endpoint check. Uses curl's own URL parser
 * (not a hand-rolled host extractor) so the validated host is exactly what
 * curl will later dial. Exact-host-equality only — no suffix matching. */
static int url_host_allowed(const char *url) {
    char *base = channel_get_config(g_ctx, "base_url");
    if (!base) return 0;  /* fail-closed: no config → no send */
    CURLU *bu = curl_url(), *tu = curl_url();
    char *bh = NULL, *th = NULL;
    int ok = 0;
    if (bu && tu &&
        curl_url_set(bu, CURLUPART_URL, base, 0) == CURLUE_OK &&
        curl_url_set(tu, CURLUPART_URL, url, 0) == CURLUE_OK &&
        curl_url_get(bu, CURLUPART_HOST, &bh, 0) == CURLUE_OK &&
        curl_url_get(tu, CURLUPART_HOST, &th, 0) == CURLUE_OK)
        ok = (strcasecmp(bh, th) == 0);
    curl_free(bh); curl_free(th);
    if (bu) curl_url_cleanup(bu);
    if (tu) curl_url_cleanup(tu);
    free(base);
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

static void send_start_next(void) {
    if (g_send_active) return;
    SendReq *r = send_queue_pop();
    if (!r) return;
    g_send_easy = make_easy(r->method, r->url, r->body, r->headers, r->n_headers,
                            r->timeout > 0 ? r->timeout : CR_SEND_TIMEOUT,
                            &g_send_resp, &g_send_hdrs);
    if (!g_send_easy) {
        if (r->outbox_id > 0) channel_fail_outbox(g_ctx, r->outbox_id, "curl init failed");
        send_req_free(r);
        return;
    }
    curl_multi_add_handle(g_multi, g_send_easy);
    g_send_active = r;
}

/* ── JS handler call sites ─────────────────────────────────────── */

void call_on_outbox(JSContext *ctx, ChannelOutboxRow *row) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "id", JS_NewInt32(ctx, (int32_t)row->id));
    JS_SetPropertyStr(ctx, obj, "session_id", JS_NewInt32(ctx, (int32_t)row->session_id));
    JS_SetPropertyStr(ctx, obj, "payload",
        JS_NewString(ctx, row->payload ? row->payload : ""));
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

void call_on_result(JSContext *ctx, const char *tag, int status,
                           const char *body, const char *error) {
    set_global_str(ctx, "__cr_tag", tag);
    set_global_int(ctx, "__cr_status", status);
    set_global_str(ctx, "__cr_body", body);
    set_global_str(ctx, "__cr_err", error);
    JSValue ret = eval_js(ctx,
        "(typeof onResult === 'function')"
        " ? onResult({tag: __cr_tag, status: __cr_status, body: __cr_body,"
        "             error: __cr_err}) : null",
        "onResult");
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
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
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

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len <= 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

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

    /* Load the secret key so admin.setKey can write to the encrypted kv. */
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

    char *js_src = read_file(js_path);
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
        send_start_next();
        poll_start();

        struct curl_waitfd extra[2];
        int n_extra = 0;
        if (outbox_fd >= 0) {
            extra[n_extra].fd = outbox_fd;
            extra[n_extra].events = CURL_WAIT_POLLIN;
            extra[n_extra].revents = 0;
            n_extra++;
        }
        int uds_slot = -1;
        if (uds_fd >= 0) {
            uds_slot = n_extra;
            extra[n_extra].fd = uds_fd;
            extra[n_extra].events = CURL_WAIT_POLLIN;
            extra[n_extra].revents = 0;
            n_extra++;
        }

        int numfds = 0;
        curl_multi_poll(g_multi, extra, (unsigned)n_extra, 1000, &numfds);

        if (outbox_fd >= 0 && (extra[0].revents & CURL_WAIT_POLLIN)) {
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
                        char err[256];
                        snprintf(err, sizeof(err), "%s",
                                 cerr ? cerr : (g_send_resp.data ? g_send_resp.data : "http error"));
                        channel_fail_outbox(g_ctx, r->outbox_id, err);
                        send_queue_drop_outbox(r->outbox_id);
                    } else if (r->is_final) {
                        channel_ack_outbox(g_ctx, r->outbox_id);
                    }
                }
                if (r->tag)
                    call_on_result(ctx, r->tag, (int)status,
                                   g_send_resp.data ? g_send_resp.data : "", cerr);
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
    if (g_poll.easy) { curl_multi_remove_handle(g_multi, g_poll.easy); curl_easy_cleanup(g_poll.easy); }
    curl_slist_free_all(g_poll.hdrs);
    free(g_poll.method); free(g_poll.url); free(g_poll.body);
    buf_free(&g_poll.resp);
    if (g_send_easy) { curl_multi_remove_handle(g_multi, g_send_easy); curl_easy_cleanup(g_send_easy); }
    curl_slist_free_all(g_send_hdrs);
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

    char js_path[1024];
    if (resolve_js_path(g_ctx->db, channel_name, js_path, sizeof(js_path)) != 0) {
        if (err_out) *err_out = strdup("no channel.qjs resolved");
        channel_ctx_free(g_ctx); g_ctx = NULL;
        return -1;
    }

    char *js_src = read_file(js_path);
    if (!js_src) {
        if (err_out) {
            char b[1200];
            snprintf(b, sizeof(b), "cannot read %s", js_path);
            *err_out = strdup(b);
        }
        channel_ctx_free(g_ctx); g_ctx = NULL;
        return -1;
    }

    /* Secret key so admin.setKey doesn't fail if onInit touches it. */
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
        curl_global_init(CURL_GLOBAL_DEFAULT);
        g_multi = curl_multi_init();
        JSValue init_ret = eval_js(ctx, "onInit()", "<check-init>");
        if (JS_IsUndefined(init_ret)) {
            if (err_out) *err_out = strdup("onInit() failed or threw");
            rc = -1;
        } else {
            JS_FreeValue(ctx, init_ret);
        }
        /* Never enters the event loop — drop anything onInit queued so
         * nothing actually goes out over the network. */
        SendReq *r;
        while ((r = send_queue_pop()) != NULL) send_req_free(r);
        curl_multi_cleanup(g_multi); g_multi = NULL;
        curl_global_cleanup();
    }

    JS_FreeContext(ctx);
    qjs_runtime_destroy(g_qrt); g_qrt = NULL;
    channel_ctx_free(g_ctx); g_ctx = NULL;
    return rc;
}
