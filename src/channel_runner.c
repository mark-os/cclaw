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
#define _POSIX_C_SOURCE 200809L
#include "channel_api.h"
#include "admin_api.h"
#include "db.h"
#include "log.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <mquickjs.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define CR_HEAP_SIZE (2 * 1024 * 1024)
#define CR_MAX_INSTRUCTIONS 100000000
#define CR_REQ_MAX (512 * 1024)   /* max proxied request envelope */
#define CR_SEND_TIMEOUT 60L
#define CR_POLL_TIMEOUT 35L       /* slightly longer than TG long-poll */

extern const JSSTDLibraryDef js_std_library;

static volatile sig_atomic_t g_running = 1;
static ChannelCtx *g_ctx;

static void handle_signal(int sig) { (void)sig; g_running = 0; }

/* ── Host context for MQJS ─────────────────────────────────────── */

typedef struct {
    int instruction_count;
    int instruction_limit;
} HostCtx;

static int interrupt_handler(JSContext *ctx, void *opaque) {
    (void)ctx;
    HostCtx *h = (HostCtx *)opaque;
    h->instruction_count++;
    return h->instruction_count > h->instruction_limit;
}

/* ── JS value helpers ──────────────────────────────────────────── */

static char *get_str_prop(JSContext *ctx, JSValue obj, const char *name) {
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    if (JS_IsException(v) || JS_IsUndefined(v) || JS_IsNull(v)) return NULL;
    JSCStringBuf buf;
    const char *s = JS_ToCString(ctx, v, &buf);
    return s ? strdup(s) : NULL;
}

static int get_int_prop(JSContext *ctx, JSValue obj, const char *name, int dflt) {
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    if (JS_IsException(v) || JS_IsUndefined(v) || JS_IsNull(v)) return dflt;
    int i = dflt;
    JS_ToInt32(ctx, &i, v);
    return i;
}

/* Eval JS, logging any exception. Returns the value (JS_UNDEFINED on error). */
static JSValue eval_js(JSContext *ctx, const char *code, const char *tag) {
    JSValue v = JS_Eval(ctx, code, strlen(code), tag, JS_EVAL_RETVAL);
    if (JS_IsException(v)) {
        JSValue exc = JS_GetException(ctx);
        JSCStringBuf buf;
        const char *msg = JS_ToCString(ctx, exc, &buf);
        fprintf(stderr, "[channel_runner] JS error in %s: %s\n", tag, msg ? msg : "?");
        return JS_UNDEFINED;
    }
    return v;
}

static void set_global_str(JSContext *ctx, const char *name, const char *val) {
    JS_SetPropertyStr(ctx, JS_GetGlobalObject(ctx), name,
                      val ? JS_NewString(ctx, val) : JS_NULL);
}

static void set_global_int(JSContext *ctx, const char *name, int val) {
    JS_SetPropertyStr(ctx, JS_GetGlobalObject(ctx), name, JS_NewInt32(ctx, val));
}

/* ── Send queue (filled by cclaw.send, drained by the curl loop) ── */

typedef struct SendReq {
    char *method;
    char *url;
    char *body;
    char *tag;
    char **headers;
    int n_headers;
    int64_t outbox_id;   /* 0 = not outbox-bound */
    int is_final;        /* last send for this outbox row */
    long timeout;
    struct SendReq *next;
} SendReq;

static SendReq *g_send_head, *g_send_tail;

static void send_req_free(SendReq *r) {
    if (!r) return;
    free(r->method); free(r->url); free(r->body); free(r->tag);
    for (int i = 0; i < r->n_headers; i++) free(r->headers[i]);
    free(r->headers);
    free(r);
}

static void send_queue_push(SendReq *r) {
    r->next = NULL;
    if (g_send_tail) g_send_tail->next = r;
    else g_send_head = r;
    g_send_tail = r;
}

static SendReq *send_queue_pop(void) {
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

/* ── JS host functions: channel API ────────────────────────────── */

JSValue js_ch_emit(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "emit(type, payload)");
    JSCStringBuf tbuf, pbuf;
    const char *type = JS_ToCString(ctx, argv[0], &tbuf);
    const char *payload = JS_ToCString(ctx, argv[1], &pbuf);
    if (!type || !payload) return JS_ThrowTypeError(ctx, "emit: string args required");
    int rc = channel_emit(g_ctx, type, payload);
    return JS_NewInt32(ctx, rc);
}

JSValue js_ch_get_config(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "getConfig(key)");
    JSCStringBuf buf;
    const char *key = JS_ToCString(ctx, argv[0], &buf);
    if (!key) return JS_NULL;
    char *val = channel_get_config(g_ctx, key);
    if (!val) return JS_NULL;
    JSValue r = JS_NewString(ctx, val);
    free(val);
    return r;
}

JSValue js_ch_set_config(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "setConfig(key, value)");
    JSCStringBuf kbuf, vbuf;
    const char *key = JS_ToCString(ctx, argv[0], &kbuf);
    const char *val = JS_ToCString(ctx, argv[1], &vbuf);
    if (!key || !val) return JS_ThrowTypeError(ctx, "setConfig: string args required");
    int rc = channel_set_config(g_ctx, key, val);
    return JS_NewInt32(ctx, rc);
}

JSValue js_ch_ack_outbox(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "ackOutbox(id)");
    int id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    int rc = channel_ack_outbox(g_ctx, (int64_t)id);
    return JS_NewInt32(ctx, rc);
}

JSValue js_ch_fail_outbox(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "failOutbox(id, error)");
    int id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    JSCStringBuf ebuf;
    const char *err = JS_ToCString(ctx, argv[1], &ebuf);
    int rc = channel_fail_outbox(g_ctx, (int64_t)id, err ? err : "unknown");
    return JS_NewInt32(ctx, rc);
}

JSValue js_ch_log(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    JSCStringBuf buf;
    const char *msg = JS_ToCString(ctx, argv[0], &buf);
    if (msg) fprintf(stderr, "[%s] %s\n", g_ctx->channel_name, msg);
    return JS_UNDEFINED;
}

/* cclaw.send(req) — queue an outbound HTTP request for the C loop.
 * The JS wrapper normalizes the shape before calling here. */
JSValue js_ch_send(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "send(request)");
    JSValue o = argv[0];

    char *url = get_str_prop(ctx, o, "url");
    if (!url || !url[0]) {
        free(url);
        return JS_ThrowTypeError(ctx, "send: url required");
    }

    SendReq *r = calloc(1, sizeof(SendReq));
    if (!r) { free(url); return JS_ThrowTypeError(ctx, "send: OOM"); }
    r->url = url;
    r->method = get_str_prop(ctx, o, "method");
    r->body = get_str_prop(ctx, o, "body");
    r->tag = get_str_prop(ctx, o, "tag");
    r->outbox_id = get_int_prop(ctx, o, "outbox_id", 0);
    r->is_final = get_int_prop(ctx, o, "final", 0);
    r->timeout = get_int_prop(ctx, o, "timeout", 0);

    JSValue h = JS_GetPropertyStr(ctx, o, "headers");
    if (!JS_IsException(h) && !JS_IsUndefined(h) && !JS_IsNull(h)) {
        int n = get_int_prop(ctx, h, "length", 0);
        if (n > 0 && n <= 32) {
            r->headers = calloc((size_t)n, sizeof(char *));
            if (r->headers) {
                for (int i = 0; i < n; i++) {
                    JSValue hv = JS_GetPropertyUint32(ctx, h, (uint32_t)i);
                    JSCStringBuf hbuf;
                    const char *hs = JS_ToCString(ctx, hv, &hbuf);
                    if (hs) r->headers[r->n_headers++] = strdup(hs);
                }
            }
        }
    }

    send_queue_push(r);
    return JS_NewInt32(ctx, 0);
}

/* ── JS host functions: admin API ──────────────────────────────── */

JSValue js_admin_set_key(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "admin.setKey(provider, value)");
    JSCStringBuf pbuf, vbuf;
    const char *provider = JS_ToCString(ctx, argv[0], &pbuf);
    const char *value = JS_ToCString(ctx, argv[1], &vbuf);
    if (!provider || !value) return JS_NewInt32(ctx, -1);
    char *env_file = channel_get_config(g_ctx, "env_file");
    const char *ef = (env_file && env_file[0]) ? env_file : "/etc/cclaw/env";
    int rc = admin_set_key(ef, provider, value);
    free(env_file);
    return JS_NewInt32(ctx, rc);
}

JSValue js_admin_set_model(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "admin.setModel(index, model)");
    int idx = 0;
    JS_ToInt32(ctx, &idx, argv[0]);
    JSCStringBuf mbuf;
    const char *model = JS_ToCString(ctx, argv[1], &mbuf);
    if (!model) return JS_NewInt32(ctx, -1);
    int rc = admin_set_model(g_ctx->db, idx, model);
    return JS_NewInt32(ctx, rc);
}

JSValue js_admin_set_endpoint(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "admin.setEndpoint(index, url)");
    int idx = 0;
    JS_ToInt32(ctx, &idx, argv[0]);
    JSCStringBuf ubuf;
    const char *url = JS_ToCString(ctx, argv[1], &ubuf);
    if (!url) return JS_NewInt32(ctx, -1);
    int rc = admin_set_endpoint(g_ctx->db, idx, url);
    return JS_NewInt32(ctx, rc);
}

JSValue js_admin_add_host(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "admin.addHost(agent, host)");
    JSCStringBuf abuf, hbuf;
    const char *agent = JS_ToCString(ctx, argv[0], &abuf);
    const char *host = JS_ToCString(ctx, argv[1], &hbuf);
    if (!agent || !host) return JS_NewInt32(ctx, -1);
    return JS_NewInt32(ctx, admin_add_host(g_ctx->db, agent, host));
}

JSValue js_admin_remove_host(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "admin.removeHost(agent, host)");
    JSCStringBuf abuf, hbuf;
    const char *agent = JS_ToCString(ctx, argv[0], &abuf);
    const char *host = JS_ToCString(ctx, argv[1], &hbuf);
    if (!agent || !host) return JS_NewInt32(ctx, -1);
    return JS_NewInt32(ctx, admin_remove_host(g_ctx->db, agent, host));
}

JSValue js_admin_list_providers(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    AdminProvider *providers = NULL;
    size_t count = 0;
    if (admin_list_providers(g_ctx->db, &providers, &count) != 0)
        return JS_NewArray(ctx, 0);
    JSValue arr = JS_NewArray(ctx, (int)count);
    for (size_t i = 0; i < count; i++) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "index", JS_NewInt32(ctx, providers[i].index));
        JS_SetPropertyStr(ctx, obj, "model",
            JS_NewString(ctx, providers[i].model ? providers[i].model : ""));
        JS_SetPropertyStr(ctx, obj, "base_url",
            JS_NewString(ctx, providers[i].base_url ? providers[i].base_url : ""));
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, obj);
    }
    admin_providers_free(providers, count);
    return arr;
}

JSValue js_admin_list_agents(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    int count = 0;
    char **names = db_agent_list(g_ctx->db, &count);
    JSValue arr = JS_NewArray(ctx, count);
    for (int i = 0; i < count; i++) {
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, JS_NewString(ctx, names[i]));
    }
    if (names) { for (int i = 0; i < count; i++) free(names[i]); free(names); }
    return arr;
}

JSValue js_admin_is_admin(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewBool(0);
    JSCStringBuf buf;
    const char *channel_id = JS_ToCString(ctx, argv[0], &buf);
    if (!channel_id) return JS_NewBool(0);
    /* Check channel_state for admin_ids (comma-separated) */
    char *admins = channel_get_config(g_ctx, "admin_ids");
    if (!admins) return JS_NewBool(0);
    size_t cid_len = strlen(channel_id);
    int found = 0;
    const char *p = admins;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        const char *end = p;
        while (*end && *end != ',') end++;
        size_t seg_len = (size_t)(end - p);
        if (seg_len == cid_len && memcmp(p, channel_id, cid_len) == 0) { found = 1; break; }
        p = *end ? end + 1 : end;
    }
    free(admins);
    return JS_NewBool(found);
}

/* ── Install host functions on JS global ───────────────────────── */

static void install_host_fns(JSContext *ctx) {
    /* Host functions are dispatched via __cclaw_call_tool(name, ...args).
     * cclaw.send normalizes the request shape so the C side reads
     * predictable types. */
    const char *init_code =
        "globalThis.cclaw = {\n"
        "  emit: function(t,p) { return __cclaw_call_tool('emit',t,p); },\n"
        "  getConfig: function(k) { return __cclaw_call_tool('getConfig',k); },\n"
        "  setConfig: function(k,v) { return __cclaw_call_tool('setConfig',k,v); },\n"
        "  ackOutbox: function(id) { return __cclaw_call_tool('ackOutbox',id); },\n"
        "  failOutbox: function(id,e) { return __cclaw_call_tool('failOutbox',id,e); },\n"
        "  send: function(r) { return __cclaw_call_tool('send', {\n"
        "    url: '' + r.url,\n"
        "    method: r.method || (r.body != null ? 'POST' : 'GET'),\n"
        "    body: r.body == null ? null : '' + r.body,\n"
        "    tag: r.tag == null ? null : '' + r.tag,\n"
        "    headers: r.headers || null,\n"
        "    outbox_id: r.outbox_id ? (r.outbox_id|0) : 0,\n"
        "    final: r.final ? 1 : 0,\n"
        "    timeout: r.timeout ? (r.timeout|0) : 0\n"
        "  }); },\n"
        "  log: function(m) { return __cclaw_call_tool('log',m); },\n"
        "  admin: {\n"
        "    setKey: function(p,v) { return __cclaw_call_tool('admin.setKey',p,v); },\n"
        "    setModel: function(i,m) { return __cclaw_call_tool('admin.setModel',i,m); },\n"
        "    setEndpoint: function(i,u) { return __cclaw_call_tool('admin.setEndpoint',i,u); },\n"
        "    addHost: function(a,h) { return __cclaw_call_tool('admin.addHost',a,h); },\n"
        "    removeHost: function(a,h) { return __cclaw_call_tool('admin.removeHost',a,h); },\n"
        "    listProviders: function() { return __cclaw_call_tool('admin.listProviders'); },\n"
        "    listAgents: function() { return __cclaw_call_tool('admin.listAgents'); },\n"
        "    isAdmin: function(id) { return __cclaw_call_tool('admin.isAdmin',id); }\n"
        "  }\n"
        "};\n";

    JSValue v = JS_Eval(ctx, init_code, strlen(init_code), "<cr_init>", 0);
    if (JS_IsException(v)) {
        JSValue exc = JS_GetException(ctx);
        JSCStringBuf buf;
        const char *msg = JS_ToCString(ctx, exc, &buf);
        fprintf(stderr, "[channel_runner] cclaw init error: %s\n", msg ? msg : "?");
    }
}

/* ── HTTP transfers (poll + sends) on one curl_multi ───────────── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} RespBuf;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *ud) {
    size_t bytes = size * nmemb;
    RespBuf *b = (RespBuf *)ud;
    if (b->len + bytes + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->len + bytes + 1) nc *= 2;
        char *tmp = realloc(b->data, nc);
        if (!tmp) return 0;
        b->data = tmp;
        b->cap = nc;
    }
    memcpy(b->data + b->len, ptr, bytes);
    b->len += bytes;
    b->data[b->len] = '\0';
    return bytes;
}

static CURLM *g_multi;

/* Poller: the recurring long-poll request (shape owned here) */
static struct {
    CURL *easy;
    RespBuf resp;
    char *method, *url, *body;
    struct curl_slist *hdrs;
    int active;
    int errors;          /* consecutive failures, drives backoff */
    time_t next_at;
} g_poll;

/* One in-flight send at a time preserves per-channel ordering */
static SendReq *g_send_active;
static CURL *g_send_easy;
static RespBuf g_send_resp;
static struct curl_slist *g_send_hdrs;

static CURL *make_easy(const char *method, const char *url, const char *body,
                       char **headers, int n_headers, long timeout,
                       RespBuf *resp, struct curl_slist **out_hdrs) {
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    free(resp->data);
    memset(resp, 0, sizeof(*resp));
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
    if (JS_IsException(p) || JS_IsUndefined(p)) return;
    free(g_poll.method); free(g_poll.url); free(g_poll.body);
    g_poll.method = g_poll.url = g_poll.body = NULL;
    if (JS_IsNull(p)) return;  /* explicit null stops polling */
    g_poll.url = get_str_prop(ctx, p, "url");
    g_poll.method = get_str_prop(ctx, p, "method");
    g_poll.body = get_str_prop(ctx, p, "body");
}

static void poll_start(void) {
    if (g_poll.active || !g_poll.url || !g_poll.url[0]) return;
    if (time(NULL) < g_poll.next_at) return;
    if (g_poll.easy) { curl_multi_remove_handle(g_multi, g_poll.easy); curl_easy_cleanup(g_poll.easy); }
    curl_slist_free_all(g_poll.hdrs); g_poll.hdrs = NULL;
    g_poll.easy = make_easy(g_poll.method, g_poll.url, g_poll.body,
                            NULL, 0, CR_POLL_TIMEOUT, &g_poll.resp, &g_poll.hdrs);
    if (!g_poll.easy) return;
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

static void call_on_outbox(JSContext *ctx, ChannelOutboxRow *row) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "id", JS_NewInt32(ctx, (int32_t)row->id));
    JS_SetPropertyStr(ctx, obj, "session_id", JS_NewInt32(ctx, (int32_t)row->session_id));
    JS_SetPropertyStr(ctx, obj, "payload",
        JS_NewString(ctx, row->payload ? row->payload : ""));
    JS_SetPropertyStr(ctx, global, "__cr_outbox_item", obj);
    eval_js(ctx, "onOutbox(__cr_outbox_item)", "onOutbox");
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
}

static void call_on_result(JSContext *ctx, const char *tag, int status,
                           const char *body, const char *error) {
    set_global_str(ctx, "__cr_tag", tag);
    set_global_int(ctx, "__cr_status", status);
    set_global_str(ctx, "__cr_body", body);
    set_global_str(ctx, "__cr_err", error);
    eval_js(ctx,
        "(typeof onResult === 'function')"
        " ? onResult({tag: __cr_tag, status: __cr_status, body: __cr_body,"
        "             error: __cr_err}) : null",
        "onResult");
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

    JSCStringBuf rbuf;
    const char *reply = JS_ToCString(ctx, ret, &rbuf);
    if (!reply) reply = "500\n";
    size_t rlen = strlen(reply), off = 0;
    while (off < rlen) {
        ssize_t n = write(cfd, reply + off, rlen - off);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
        off += (size_t)n;
    }
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

int main(int argc, char **argv) {
    cclaw_log_init();
    cclaw_log_set_level(log_level_parse(getenv("CCLAW_LOG_LEVEL")));
    if (argc < 3) {
        fprintf(stderr, "usage: channel_runner <db_path> <channel_name>\n");
        return 1;
    }
    const char *db_path = argv[1];
    const char *channel_name = argv[2];

    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    g_ctx = channel_ctx_open(db_path, channel_name);
    if (!g_ctx) { fprintf(stderr, "[channel_runner] DB open failed\n"); return 1; }

    /* Resolve js_path from extensions table */
    char js_path[1024] = {0};
    {
        const char *sql = "SELECT e.path FROM channels c"
                          " JOIN extensions e ON c.extension_name=e.name"
                          " WHERE c.name=?;";
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(g_ctx->db, sql, -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, channel_name, -1, SQLITE_STATIC);
            if (sqlite3_step(s) == SQLITE_ROW) {
                const char *p = (const char *)sqlite3_column_text(s, 0);
                if (p) snprintf(js_path, sizeof(js_path), "%s/channel.js", p);
            }
            sqlite3_finalize(s);
        }
    }
    if (!js_path[0]) {
        fprintf(stderr, "[channel_runner] no extension path for channel '%s'\n", channel_name);
        channel_ctx_free(g_ctx);
        return 1;
    }

    int outbox_fd = channel_outbox_fifo_open(db_path, channel_name);
    if (outbox_fd < 0)
        fprintf(stderr, "[channel_runner] warning: outbox FIFO unavailable\n");

    int uds_fd = uds_listen_open(db_path, channel_name);
    if (uds_fd < 0)
        fprintf(stderr, "[channel_runner] warning: request socket unavailable\n");

    char *js_src = read_file(js_path);
    if (!js_src) {
        fprintf(stderr, "[channel_runner] cannot read %s\n", js_path);
        channel_ctx_free(g_ctx);
        return 1;
    }

    void *heap = malloc(CR_HEAP_SIZE);
    if (!heap) { free(js_src); channel_ctx_free(g_ctx); return 1; }
    JSContext *ctx = JS_NewContext(heap, CR_HEAP_SIZE, &js_std_library);
    if (!ctx) { free(heap); free(js_src); channel_ctx_free(g_ctx); return 1; }

    HostCtx hctx = {.instruction_count = 0, .instruction_limit = CR_MAX_INSTRUCTIONS};
    JS_SetInterruptHandler(ctx, interrupt_handler);
    JS_SetContextOpaque(ctx, &hctx);

    install_host_fns(ctx);

    JSValue load_val = JS_Eval(ctx, js_src, strlen(js_src), js_path, 0);
    free(js_src);
    if (JS_IsException(load_val)) {
        JSValue exc = JS_GetException(ctx);
        JSCStringBuf buf;
        const char *msg = JS_ToCString(ctx, exc, &buf);
        fprintf(stderr, "[channel_runner] JS load error: %s\n", msg ? msg : "?");
        JS_FreeContext(ctx); free(heap); channel_ctx_free(g_ctx);
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_multi = curl_multi_init();

    /* onInit: required; may set a poll shape and queue sends */
    JSValue init_ret = JS_Eval(ctx, "onInit()", 8, "<init>", JS_EVAL_RETVAL);
    if (JS_IsException(init_ret)) {
        JSValue exc = JS_GetException(ctx);
        JSCStringBuf buf;
        const char *msg = JS_ToCString(ctx, exc, &buf);
        fprintf(stderr, "[channel_runner] onInit failed: %s\n", msg ? msg : "?");
        curl_multi_cleanup(g_multi); curl_global_cleanup();
        JS_FreeContext(ctx); free(heap); channel_ctx_free(g_ctx);
        return 1;
    }
    poll_shape_update(ctx, init_ret);

    fprintf(stderr, "[channel_runner] started: channel=%s poll=%s requests=%s\n",
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
    free(g_poll.resp.data);
    if (g_send_easy) { curl_multi_remove_handle(g_multi, g_send_easy); curl_easy_cleanup(g_send_easy); }
    curl_slist_free_all(g_send_hdrs);
    send_req_free(g_send_active);
    free(g_send_resp.data);
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
    free(heap);
    channel_ctx_free(g_ctx);
    fprintf(stderr, "[channel_runner] stopped\n");
    return 0;
}
