/* channel_runner — universal JS channel process.
 * Owns the poll() event loop; JS is purely reactive (onInit, onNetwork, onOutbox).
 * Supports: http_long_poll, websocket, listen.
 * argv: channel_runner <db_path> <channel_name> <js_path> */
#define _POSIX_C_SOURCE 200809L
#include "channel_api.h"
#include "admin_api.h"
#include "db.h"
#include "log.h"
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <mquickjs.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CR_HEAP_SIZE (2 * 1024 * 1024)
#define CR_MAX_INSTRUCTIONS 100000000

extern const JSSTDLibraryDef js_std_library;

static volatile sig_atomic_t g_running = 1;
static ChannelCtx *g_ctx;

static void handle_signal(int sig) { (void)sig; g_running = 0; }

/* ── Host context for MQJS ─────────────────────────────────────── */

typedef struct {
    int instruction_count;
    int instruction_limit;
    char **allowed_hosts;
    size_t allowed_hosts_count;
} HostCtx;

static int interrupt_handler(JSContext *ctx, void *opaque) {
    (void)ctx;
    HostCtx *h = (HostCtx *)opaque;
    h->instruction_count++;
    return h->instruction_count > h->instruction_limit;
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

/* ── JS host functions: HTTP ───────────────────────────────────── */

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

JSValue js_http_request(JSContext *ctx, const char *method,
                               const char *url, const char *body) {
    CURL *c = curl_easy_init();
    if (!c) return JS_ThrowTypeError(ctx, "curl_easy_init failed");

    RespBuf resp = {0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);

    struct curl_slist *headers = NULL;
    if (body) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    }
    if (strcmp(method, "GET") != 0 && !body)
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);

    CURLcode res = curl_easy_perform(c);
    long status = 0;
    if (res == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);

    if (res != CURLE_OK) {
        free(resp.data);
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "status", JS_NewInt32(ctx, -1));
        JS_SetPropertyStr(ctx, obj, "error",
            JS_NewString(ctx, curl_easy_strerror(res)));
        return obj;
    }

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "status", JS_NewInt32(ctx, (int32_t)status));
    JS_SetPropertyStr(ctx, obj, "body",
        JS_NewString(ctx, resp.data ? resp.data : ""));
    free(resp.data);
    return obj;
}

JSValue js_ch_http_post(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "httpPost(url, body)");
    JSCStringBuf ubuf, bbuf;
    const char *url = JS_ToCString(ctx, argv[0], &ubuf);
    const char *body = JS_ToCString(ctx, argv[1], &bbuf);
    if (!url) return JS_ThrowTypeError(ctx, "httpPost: url required");
    return js_http_request(ctx, "POST", url, body);
}

JSValue js_ch_http_get(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "httpGet(url)");
    JSCStringBuf ubuf;
    const char *url = JS_ToCString(ctx, argv[0], &ubuf);
    if (!url) return JS_ThrowTypeError(ctx, "httpGet: url required");
    return js_http_request(ctx, "GET", url, NULL);
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
     * We create the cclaw namespace object that wraps this dispatch. */
    const char *init_code =
        "globalThis.cclaw = {\n"
        "  emit: function(t,p) { return __cclaw_call_tool('emit',t,p); },\n"
        "  getConfig: function(k) { return __cclaw_call_tool('getConfig',k); },\n"
        "  setConfig: function(k,v) { return __cclaw_call_tool('setConfig',k,v); },\n"
        "  ackOutbox: function(id) { return __cclaw_call_tool('ackOutbox',id); },\n"
        "  failOutbox: function(id,e) { return __cclaw_call_tool('failOutbox',id,e); },\n"
        "  httpPost: function(u,b) { return __cclaw_call_tool('httpPost',u,b); },\n"
        "  httpGet: function(u) { return __cclaw_call_tool('httpGet',u); },\n"
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

/* ── curl_multi based long-poll ────────────────────────────────── */

typedef struct {
    CURLM *multi;
    CURL *easy;
    RespBuf resp;
    char *url;          /* current request URL (owned) */
    int active;         /* 1 = transfer in progress */
} LongPollState;

static void lp_start_request(LongPollState *lp, const char *url) {
    if (lp->easy) { curl_multi_remove_handle(lp->multi, lp->easy); curl_easy_cleanup(lp->easy); }
    free(lp->resp.data);
    memset(&lp->resp, 0, sizeof(lp->resp));
    free(lp->url);
    lp->url = strdup(url);

    lp->easy = curl_easy_init();
    curl_easy_setopt(lp->easy, CURLOPT_URL, url);
    curl_easy_setopt(lp->easy, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(lp->easy, CURLOPT_WRITEDATA, &lp->resp);
    curl_easy_setopt(lp->easy, CURLOPT_TIMEOUT, 35L); /* slightly longer than TG timeout */
    struct curl_slist *h = curl_slist_append(NULL, "Content-Type: application/json");
    curl_easy_setopt(lp->easy, CURLOPT_HTTPHEADER, h);
    /* Note: headers leak — acceptable for long-running process */
    curl_multi_add_handle(lp->multi, lp->easy);
    lp->active = 1;
}

static void lp_cleanup(LongPollState *lp) {
    if (lp->easy) { curl_multi_remove_handle(lp->multi, lp->easy); curl_easy_cleanup(lp->easy); }
    if (lp->multi) curl_multi_cleanup(lp->multi);
    free(lp->resp.data);
    free(lp->url);
}

/* ── Main event loop ───────────────────────────────────────────── */

enum PollType { POLL_HTTP_LONG_POLL, POLL_LISTEN, POLL_WEBSOCKET };

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

/* Call a JS function by name with a string argument. Returns heap string or NULL. */
static char *call_js_str(JSContext *ctx, const char *fn, const char *arg) {
    char code[128];
    if (arg) {
        JS_SetPropertyStr(ctx, JS_GetGlobalObject(ctx), "__cr_call_arg",
                          JS_NewString(ctx, arg));
        snprintf(code, sizeof(code), "JSON.stringify(%s(__cr_call_arg))", fn);
    } else {
        snprintf(code, sizeof(code), "JSON.stringify(%s())", fn);
    }
    JSValue val = JS_Eval(ctx, code, strlen(code), "<call>", JS_EVAL_RETVAL);
    if (JS_IsException(val)) {
        JSValue exc = JS_GetException(ctx);
        JSCStringBuf buf;
        const char *msg = JS_ToCString(ctx, exc, &buf);
        fprintf(stderr, "[channel_runner] JS error in %s: %s\n", fn, msg ? msg : "?");
        return NULL;
    }
    if (JS_IsNull(val) || JS_IsUndefined(val)) return NULL;
    JSCStringBuf buf;
    const char *s = JS_ToCString(ctx, val, &buf);
    return s ? strdup(s) : NULL;
}

/* Call JS onNetwork(body_string). */
static void call_on_network(JSContext *ctx, const char *body) {
    /* Pass raw body via global property to avoid escaping issues */
    JS_SetPropertyStr(ctx, JS_GetGlobalObject(ctx), "__cr_net_body",
                      JS_NewString(ctx, body));
    const char *code = "onNetwork(__cr_net_body)";
    JSValue val = JS_Eval(ctx, code, strlen(code), "<net>", 0);
    if (JS_IsException(val)) {
        JSValue exc = JS_GetException(ctx);
        JSCStringBuf buf;
        const char *msg = JS_ToCString(ctx, exc, &buf);
        fprintf(stderr, "[channel_runner] onNetwork error: %s\n", msg ? msg : "?");
    }
}

/* Call JS onOutbox({id, session_id, payload}). */
static void call_on_outbox(JSContext *ctx, ChannelOutboxRow *row) {
    /* Set outbox data as a global object */
    char code[256];
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "id", JS_NewInt32(ctx, (int32_t)row->id));
    JS_SetPropertyStr(ctx, obj, "session_id", JS_NewInt32(ctx, (int32_t)row->session_id));
    JS_SetPropertyStr(ctx, obj, "payload",
        JS_NewString(ctx, row->payload ? row->payload : ""));
    JS_SetPropertyStr(ctx, global, "__cr_outbox_item", obj);
    snprintf(code, sizeof(code), "onOutbox(__cr_outbox_item)");
    JSValue val = JS_Eval(ctx, code, strlen(code), "<outbox>", 0);
    if (JS_IsException(val)) {
        JSValue exc = JS_GetException(ctx);
        JSCStringBuf buf;
        const char *msg = JS_ToCString(ctx, exc, &buf);
        fprintf(stderr, "[channel_runner] onOutbox error: %s\n", msg ? msg : "?");
    }
}

int main(int argc, char **argv) {
    cclaw_log_init();
    if (argc < 3) {
        fprintf(stderr, "usage: channel_runner <db_path> <channel_name>\n");
        return 1;
    }
    const char *db_path = argv[1];
    const char *channel_name = argv[2];

    /* Resolve js_path from extensions table */
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);

    /* Open channel context */
    g_ctx = channel_ctx_open(db_path, channel_name);
    if (!g_ctx) { fprintf(stderr, "[channel_runner] DB open failed\n"); return 1; }

    char js_path_buf[1024] = {0};
    {
        const char *sql = "SELECT e.path FROM channels c"
                          " JOIN extensions e ON c.extension_name=e.name"
                          " WHERE c.name=?;";
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(g_ctx->db, sql, -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, channel_name, -1, SQLITE_STATIC);
            if (sqlite3_step(s) == SQLITE_ROW) {
                const char *p = (const char *)sqlite3_column_text(s, 0);
                if (p) snprintf(js_path_buf, sizeof(js_path_buf), "%s/channel.js", p);
            }
            sqlite3_finalize(s);
        }
    }
    const char *js_path = js_path_buf;
    if (!js_path[0]) {
        fprintf(stderr, "[channel_runner] no extension path for channel '%s'\n", channel_name);
        channel_ctx_free(g_ctx);
        return 1;
    }

    /* Open outbox wake FIFO */
    int outbox_fd = channel_outbox_fifo_open(db_path, channel_name);
    if (outbox_fd < 0)
        fprintf(stderr, "[channel_runner] warning: outbox FIFO unavailable\n");

    /* Load JS */
    char *js_src = read_file(js_path);
    if (!js_src) {
        fprintf(stderr, "[channel_runner] cannot read %s\n", js_path);
        channel_ctx_free(g_ctx);
        return 1;
    }

    /* Create MQJS context */
    void *heap = malloc(CR_HEAP_SIZE);
    if (!heap) { free(js_src); channel_ctx_free(g_ctx); return 1; }
    JSContext *ctx = JS_NewContext(heap, CR_HEAP_SIZE, &js_std_library);
    if (!ctx) { free(heap); free(js_src); channel_ctx_free(g_ctx); return 1; }

    HostCtx hctx = {.instruction_limit = CR_MAX_INSTRUCTIONS};
    JS_SetInterruptHandler(ctx, interrupt_handler);
    JS_SetContextOpaque(ctx, &hctx);

    /* Install host functions — they need to be in the stdlib's c_function_table.
     * Since we can't modify the stdlib at runtime, we use a different approach:
     * evaluate JS code that references the built-in functions (http_fetch, Date.now)
     * and add our channel functions via the global object. */
    install_host_fns(ctx);

    /* Evaluate channel JS */
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

    /* Call onInit() */
    char *init_result = call_js_str(ctx, "onInit", NULL);
    if (!init_result) {
        fprintf(stderr, "[channel_runner] onInit() failed or returned null\n");
        JS_FreeContext(ctx); free(heap); channel_ctx_free(g_ctx);
        return 1;
    }

    /* Parse init result to determine poll_type and url */
    /* Quick JSON parse via JS eval */
    char parse_code[4096];
    snprintf(parse_code, sizeof(parse_code),
        "var __cr_init = %s; __cr_init;", init_result);
    JS_Eval(ctx, parse_code, strlen(parse_code), "<parse>", 0);
    free(init_result);

    /* Extract poll_type */
    const char *extract_pt = "globalThis.__cr_init.poll_type";
    JSValue pt_val = JS_Eval(ctx, extract_pt, strlen(extract_pt), "<pt>", JS_EVAL_RETVAL);
    JSCStringBuf ptbuf;
    const char *poll_type_str = JS_ToCString(ctx, pt_val, &ptbuf);

    enum PollType poll_type = POLL_HTTP_LONG_POLL;
    if (poll_type_str) {
        if (strcmp(poll_type_str, "listen") == 0) poll_type = POLL_LISTEN;
        else if (strcmp(poll_type_str, "websocket") == 0) poll_type = POLL_WEBSOCKET;
    }

    /* Extract URL */
    const char *extract_url = "globalThis.__cr_init.url || ''";
    JSValue url_val = JS_Eval(ctx, extract_url, strlen(extract_url), "<url>", JS_EVAL_RETVAL);
    JSCStringBuf urlbuf;
    const char *init_url = JS_ToCString(ctx, url_val, &urlbuf);
    char *poll_url = init_url ? strdup(init_url) : NULL;

    fprintf(stderr, "[channel_runner] started: channel=%s poll_type=%s\n",
            channel_name, poll_type_str ? poll_type_str : "http_long_poll");

    /* ── Initial outbox drain (handles messages queued before startup) ── */
    {
        ChannelOutboxRow *row;
        while ((row = channel_next_outbox(g_ctx)) != NULL) {
            call_on_outbox(ctx, row);
            channel_outbox_row_free(row);
        }
    }

    /* ── Event loop ────────────────────────────────────────────── */

    if (poll_type == POLL_HTTP_LONG_POLL) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        LongPollState lp = {0};
        lp.multi = curl_multi_init();

        /* Start first request */
        if (poll_url && poll_url[0])
            lp_start_request(&lp, poll_url);

        while (g_running) {
            /* Drive curl_multi */
            int still_running = 0;
            curl_multi_perform(lp.multi, &still_running);

            /* Get curl fds for poll */
            /* Use curl_multi_poll to wait on both curl + our extra fd */
            struct curl_waitfd extra[1];
            int n_extra = 0;
            if (outbox_fd >= 0) {
                extra[0].fd = outbox_fd;
                extra[0].events = CURL_WAIT_POLLIN;
                extra[0].revents = 0;
                n_extra = 1;
            }

            int numfds = 0;
            curl_multi_poll(lp.multi, extra, (unsigned)n_extra, 1000, &numfds);

            /* Check outbox FIFO */
            if (n_extra > 0 && (extra[0].revents & CURL_WAIT_POLLIN)) {
                char drain[64];
                while (read(outbox_fd, drain, sizeof(drain)) > 0) {}
                /* Drain all pending outbox rows */
                ChannelOutboxRow *row;
                while ((row = channel_next_outbox(g_ctx)) != NULL) {
                    call_on_outbox(ctx, row);
                    channel_outbox_row_free(row);
                }
            }

            /* Check for completed transfers */
            CURLMsg *msg;
            int msgs_left;
            while ((msg = curl_multi_info_read(lp.multi, &msgs_left)) != NULL) {
                if (msg->msg == CURLMSG_DONE) {
                    /* Got response */
                    if (msg->data.result == CURLE_OK && lp.resp.data)
                        call_on_network(ctx, lp.resp.data);

                    /* Get next URL from JS (onNetwork may update offset) */
                    const char *get_url =
                        "typeof getNextUrl === 'function' ? getNextUrl() : globalThis.__cr_init.url";
                    JSValue nu = JS_Eval(ctx, get_url, strlen(get_url), "<nu>", JS_EVAL_RETVAL);
                    JSCStringBuf nubuf;
                    const char *next = JS_ToCString(ctx, nu, &nubuf);
                    if (next && next[0] && g_running)
                        lp_start_request(&lp, next);
                    else
                        lp.active = 0;
                }
            }

            /* If no active transfer and still running, restart */
            if (!lp.active && g_running && poll_url && poll_url[0]) {
                const char *get_url =
                    "typeof getNextUrl === 'function' ? getNextUrl() : globalThis.__cr_init.url";
                JSValue nu = JS_Eval(ctx, get_url, strlen(get_url), "<nu>", JS_EVAL_RETVAL);
                JSCStringBuf nubuf;
                const char *next = JS_ToCString(ctx, nu, &nubuf);
                if (next && next[0])
                    lp_start_request(&lp, next);
            }
        }

        lp_cleanup(&lp);
        curl_global_cleanup();

    } else if (poll_type == POLL_LISTEN) {
        /* NOT YET FUNCTIONAL — minimal TCP server, no HTTP parsing */
        /* TCP server socket */
        int port = 0;
        const char *get_port = "globalThis.__cr_init.port || 8080";
        JSValue pv = JS_Eval(ctx, get_port, strlen(get_port), "<port>", JS_EVAL_RETVAL);
        JS_ToInt32(ctx, &port, pv);

        int srv = socket(AF_INET, SOCK_STREAM, 0);
        if (srv < 0) { perror("socket"); goto cleanup; }
        int opt = 1;
        setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                                   .sin_addr.s_addr = INADDR_ANY};
        if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("bind"); close(srv); goto cleanup;
        }
        listen(srv, 16);
        fprintf(stderr, "[channel_runner] listening on port %d\n", port);

        while (g_running) {
            struct pollfd fds[2];
            int nfds = 0;
            fds[nfds].fd = srv; fds[nfds].events = POLLIN; nfds++;
            if (outbox_fd >= 0) { fds[nfds].fd = outbox_fd; fds[nfds].events = POLLIN; nfds++; }

            int r = poll(fds, (nfds_t)nfds, 1000);
            if (r < 0) { if (errno == EINTR) continue; break; }

            /* Check server socket */
            if (fds[0].revents & POLLIN) {
                int client = accept(srv, NULL, NULL);
                if (client >= 0) {
                    /* Read request body */
                    char buf[65536];
                    ssize_t n = read(client, buf, sizeof(buf) - 1);
                    if (n > 0) {
                        buf[n] = '\0';
                        call_on_network(ctx, buf);
                    }
                    /* TODO: send HTTP response */
                    const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
                    (void)write(client, resp, strlen(resp));
                    close(client);
                }
            }

            /* Check outbox */
            if (outbox_fd >= 0 && nfds > 1 && (fds[1].revents & POLLIN)) {
                char drain[64];
                while (read(outbox_fd, drain, sizeof(drain)) > 0) {}
                ChannelOutboxRow *row;
                while ((row = channel_next_outbox(g_ctx)) != NULL) {
                    call_on_outbox(ctx, row);
                    channel_outbox_row_free(row);
                }
            }
        }
        close(srv);

    } else if (poll_type == POLL_WEBSOCKET) {
        /* WebSocket — use curl for initial HTTP upgrade, then raw fd */
        fprintf(stderr, "[channel_runner] websocket mode: not yet implemented\n");
        /* Placeholder: poll outbox only */
        while (g_running) {
            struct pollfd fds[1];
            if (outbox_fd < 0) { sleep(1); continue; }
            fds[0].fd = outbox_fd; fds[0].events = POLLIN;
            int r = poll(fds, 1, 1000);
            if (r > 0 && (fds[0].revents & POLLIN)) {
                char drain[64];
                while (read(outbox_fd, drain, sizeof(drain)) > 0) {}
                ChannelOutboxRow *row;
                while ((row = channel_next_outbox(g_ctx)) != NULL) {
                    call_on_outbox(ctx, row);
                    channel_outbox_row_free(row);
                }
            }
        }
    }

cleanup:
    free(poll_url);
    if (outbox_fd >= 0)
        channel_outbox_fifo_close(outbox_fd, db_path, channel_name);
    JS_FreeContext(ctx);
    free(heap);
    channel_ctx_free(g_ctx);
    fprintf(stderr, "[channel_runner] stopped\n");
    return 0;
}
