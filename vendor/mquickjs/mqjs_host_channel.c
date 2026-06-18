/* Host-provided JS functions for channel_runner binary.
 * Provides baked native channel.* and channel.admin.* objects,
 * plus http_request (throws) and Date.now(). */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "channel_api.h"
#include "admin_api.h"
#include "db.h"

extern ChannelCtx *g_ctx;

/* ── Forward decl for helpers defined in channel_runner.c ────── */

extern char *get_str_prop(JSContext *ctx, JSValue obj, const char *name);
extern int get_int_prop(JSContext *ctx, JSValue obj, const char *name, int dflt);

/* ── Send queue (defined in channel_runner.c) ────────────────── */

typedef struct SendReq {
    char *method;
    char *url;
    char *body;
    char *tag;
    char **headers;
    int n_headers;
    int64_t outbox_id;
    int is_final;
    long timeout;
    struct SendReq *next;
} SendReq;

extern void send_queue_push(SendReq *r);

/* ── JS host functions: channel API ──────────────────────────── */

JSValue js_ch_emit(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "emit(type, payload)");
    JSCStringBuf tbuf, pbuf;
    const char *type = JS_ToCString(ctx, argv[0], &tbuf);
    const char *payload = JS_ToCString(ctx, argv[1], &pbuf);
    if (!type || !payload) return JS_ThrowTypeError(ctx, "emit: string args required");
    const char *external_id = NULL;
    JSCStringBuf eid_buf;
    if (argc >= 3) {
        external_id = JS_ToCString(ctx, argv[2], &eid_buf);
    }
    int rc = channel_emit(g_ctx, type, payload, external_id);
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

/* ── JS host functions: admin API ────────────────────────────── */

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

/* http_request — blocking network I/O is not available in channel JS. */
JSValue js_http_fetch(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "no blocking fetch in channels; use channel.send()");
}

/* Date.now() */
JSValue js_date_now(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    double ms = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
    return JS_NewFloat64(ctx, ms);
}
