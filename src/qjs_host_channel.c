/* Host C functions for the channel runner (cclaw --channel mode).
 * Registered as cclaw.* and admin.* globals. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "qjs_helpers.h"
#include "channel.h"
#include "util.h"           /* split_and_trim */
#include "channel_api.h"
#include "channel_runner.h"
#include "admin_api.h"
#include "dashboard.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern ChannelCtx *g_ctx;

/* Forward decl from channel_runner.c */
extern char *get_str_prop(JSContext *ctx, JSValue obj, const char *name);
extern int get_int_prop(JSContext *ctx, JSValue obj, const char *name, int dflt);


/* ── cclaw.emit(type, payload[, external_id]) ──────────────────── */

static JSValue js_ch_emit(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "emit(type, payload)");
    const char *type = JS_ToCString(ctx, argv[0]);
    const char *payload = JS_ToCString(ctx, argv[1]);
    if (!type || !payload) {
        if (type) JS_FreeCString(ctx, type);
        if (payload) JS_FreeCString(ctx, payload);
        return JS_ThrowTypeError(ctx, "emit: string args required");
    }
    const char *external_id = NULL;
    if (argc >= 3 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2]))
        external_id = JS_ToCString(ctx, argv[2]);

    int rc = channel_emit(g_ctx, type, payload, external_id);
    JS_FreeCString(ctx, type);
    JS_FreeCString(ctx, payload);
    if (external_id) JS_FreeCString(ctx, external_id);
    return JS_NewInt32(ctx, rc);
}

/* ── cclaw.getConfig(key) — registry key <ext>.<key>, read-only ── */

static JSValue js_ch_get_config(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "getConfig(key)");
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_NULL;
    char *val = channel_config_get(g_ctx->db, g_ctx->channel_name, key);
    JS_FreeCString(ctx, key);
    if (!val) return JS_NULL;
    JSValue r = JS_NewString(ctx, val);
    free(val);
    return r;
}

/* ── cclaw.getState(key) / cclaw.setState(key, value) — channel_state
 *    runtime scratch (tg_offset, webhook registration, ...) ─────── */

static JSValue js_ch_get_state(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "getState(key)");
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_NULL;
    char *val = channel_get_config(g_ctx, key);
    JS_FreeCString(ctx, key);
    if (!val) return JS_NULL;
    JSValue r = JS_NewString(ctx, val);
    free(val);
    return r;
}

static JSValue js_ch_set_state(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "setState(key, value)");
    const char *key = JS_ToCString(ctx, argv[0]);
    const char *val = JS_ToCString(ctx, argv[1]);
    if (!key || !val) {
        if (key) JS_FreeCString(ctx, key);
        if (val) JS_FreeCString(ctx, val);
        return JS_ThrowTypeError(ctx, "setState: string args required");
    }
    int rc = channel_set_config(g_ctx, key, val);
    JS_FreeCString(ctx, key);
    JS_FreeCString(ctx, val);
    return JS_NewInt32(ctx, rc);
}

/* ── cclaw.ackOutbox(id) ───────────────────────────────────────── */

static JSValue js_ch_ack_outbox(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "ackOutbox(id)");
    int id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    int rc = channel_ack_outbox(g_ctx, (int64_t)id);
    return JS_NewInt32(ctx, rc);
}

/* ── cclaw.failOutbox(id, error) ───────────────────────────────── */

static JSValue js_ch_fail_outbox(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "failOutbox(id, error)");
    int id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    const char *err = JS_ToCString(ctx, argv[1]);
    int rc = channel_fail_outbox(g_ctx, (int64_t)id, err ? err : "unknown");
    if (err) JS_FreeCString(ctx, err);
    return JS_NewInt32(ctx, rc);
}

/* ── cclaw.log(msg) ────────────────────────────────────────────── */

static JSValue js_ch_log(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    const char *msg = JS_ToCString(ctx, argv[0]);
    /* Syslog, not stderr: the channel runner is daemon-forked, so its stderr
     * goes nowhere — journald is the only place these lines are visible. */
    if (msg) { LOG_INFO_("channel_js name=%s msg=%s", g_ctx->channel_name, msg); JS_FreeCString(ctx, msg); }
    return JS_UNDEFINED;
}

/* ── cclaw.send(request) / cclaw.http(request) ─────────────────── */

/* Cap on a JS-supplied header array. Note the semantics below: exceeding it
 * drops *every* header, not just the surplus. */
#define JS_MAX_HEADERS 32

/* Pull the string array at o.headers into a fresh char* vector (caller frees
 * each element and the vector). Shared by send()/http() and conn.open(), which
 * carried line-for-line identical copies.
 *
 * Behaviour preserved deliberately, including its wart: more than
 * JS_MAX_HEADERS means no headers are sent at all. That used to be silent —
 * a request quietly losing its Authorization header is near-impossible to
 * debug from the handler side — so it now warns. Whether it should instead
 * throw, or send the first 32, is a JS API contract change for both call
 * sites and not folded into this dedup. */
static void js_headers_to_strv(JSContext *ctx, JSValueConst o,
                               char ***out_v, int *out_n) {
    *out_v = NULL;
    *out_n = 0;
    JSValue h = JS_GetPropertyStr(ctx, o, "headers");
    if (!JS_IsException(h) && !JS_IsUndefined(h) && !JS_IsNull(h)) {
        int n = get_int_prop(ctx, h, "length", 0);
        if (n > JS_MAX_HEADERS) {
            LOG_WARN_("channel js: %d headers exceeds the %d cap — sending none",
                      n, JS_MAX_HEADERS);
        } else if (n > 0) {
            char **v = calloc((size_t)n, sizeof(char *));
            if (v) {
                int k = 0;
                for (int i = 0; i < n; i++) {
                    JSValue hv = JS_GetPropertyUint32(ctx, h, (uint32_t)i);
                    const char *hs = JS_ToCString(ctx, hv);
                    JS_FreeValue(ctx, hv);
                    if (hs) { v[k++] = strdup(hs); JS_FreeCString(ctx, hs); }
                }
                *out_v = v;
                *out_n = k;
            }
        }
    }
    JS_FreeValue(ctx, h);
}

/* Parse the fields common to send() and http() (url/method/body/timeout/
 * headers) into a fresh SendReq. NULL if url is missing (caller throws). */
static SendReq *send_req_from_js(JSContext *ctx, JSValueConst o) {
    char *url = get_str_prop(ctx, (JSValue)o, "url");
    if (!url || !url[0]) { free(url); return NULL; }
    SendReq *r = calloc(1, sizeof(SendReq));
    if (!r) { free(url); return NULL; }
    r->url = url;
    r->method = get_str_prop(ctx, (JSValue)o, "method");
    r->body = get_str_prop(ctx, (JSValue)o, "body");
    r->timeout = get_int_prop(ctx, (JSValue)o, "timeout", 0);
    js_headers_to_strv(ctx, o, &r->headers, &r->n_headers);
    return r;
}

static JSValue js_ch_send(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "send(request)");
    JSValueConst o = argv[0];
    SendReq *r = send_req_from_js(ctx, o);
    if (!r) return JS_ThrowTypeError(ctx, "send: url required");
    r->outbox_id = get_int_prop(ctx, (JSValue)o, "outbox_id", 0);
    r->is_final = get_int_prop(ctx, (JSValue)o, "final", 0);
    send_queue_push(r);
    return JS_NewInt32(ctx, 0);
}

/* channel.http(req) -> Promise<{status, body, path, bytes, error}>.
 * The transfer runs on the runner's curl loop; the promise always resolves
 * (transport failure = status 0 + error), never rejects. save_to streams the
 * body to the channel's media spool and resolves with path instead of body —
 * the payload never enters the JS heap. */
static JSValue js_ch_http(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "http(request)");
    JSValueConst o = argv[0];
    SendReq *r = send_req_from_js(ctx, o);
    if (!r) return JS_ThrowTypeError(ctx, "http: url required");

    char *save = get_str_prop(ctx, (JSValue)o, "save_to");
    if (save) {
        r->save_to = channel_save_path(save);
        free(save);
        if (!r->save_to) {
            send_req_free(r);
            return JS_ThrowTypeError(ctx, "http: invalid save_to (plain filename, no '/' or leading '.')");
        }
    }

    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) { send_req_free(r); return promise; }
    r->js_ctx = ctx;
    r->p_resolve = funcs[0];
    r->p_reject = funcs[1];
    send_queue_push(r);
    return promise;
}

/* ── channel.conn.* — persistent connection primitive ─────────────
 * Thin JS→C shims over cr_conn_* (channel_runner.c owns the transport).
 * See specs/channel-transports.md. */

static JSValue js_ch_conn_open(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "conn.open(spec)");
    JSValueConst o = argv[0];
    char *url = get_str_prop(ctx, (JSValue)o, "url");
    if (!url || !url[0]) { free(url); return JS_ThrowTypeError(ctx, "conn.open: url required"); }
    char *framing = get_str_prop(ctx, (JSValue)o, "framing");
    long timeout = get_int_prop(ctx, (JSValue)o, "timeout", 0);

    char **headers = NULL;
    int n_headers = 0;
    js_headers_to_strv(ctx, o, &headers, &n_headers);

    int id = cr_conn_open(url, framing, headers, n_headers, timeout);
    free(url);
    free(framing);
    for (int i = 0; i < n_headers; i++) free(headers[i]);
    free(headers);
    if (id < 1) return JS_ThrowTypeError(ctx, "conn.open failed");
    return JS_NewInt32(ctx, id);
}

static JSValue js_ch_conn_send(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "conn.send(id, text)");
    int id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    const char *text = JS_ToCString(ctx, argv[1]);
    if (!text) return JS_ThrowTypeError(ctx, "conn.send: text required");
    int ok = cr_conn_send(id, text);
    JS_FreeCString(ctx, text);
    return JS_NewBool(ctx, ok);
}

static JSValue js_ch_conn_close(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    int id = 0, code = 0;   /* 0 = payload-less CLOSE */
    JS_ToInt32(ctx, &id, argv[0]);
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]))
        JS_ToInt32(ctx, &code, argv[1]);
    cr_conn_close(id, code);
    return JS_UNDEFINED;
}

/* ── admin.* functions ─────────────────────────────────────────── */

static JSValue admin_approvals_to_js(JSContext *ctx, const AdminApproval *list, size_t count) {
    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < count; i++) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "id", JS_NewInt64(ctx, list[i].id));
        JS_SetPropertyStr(ctx, obj, "session_id", JS_NewInt64(ctx, list[i].session_id));
        JS_SetPropertyStr(ctx, obj, "agent", JS_NewString(ctx, list[i].agent_name ? list[i].agent_name : ""));
        JS_SetPropertyStr(ctx, obj, "tool_name", JS_NewString(ctx, list[i].tool_name ? list[i].tool_name : ""));
        JS_SetPropertyStr(ctx, obj, "action", JS_NewString(ctx, list[i].action ? list[i].action : ""));
        JS_SetPropertyStr(ctx, obj, "args_json", JS_NewString(ctx, list[i].args_json ? list[i].args_json : "{}"));
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, obj);
    }
    return arr;
}

static JSValue js_admin_list_pending_approvals(JSContext *ctx, JSValueConst this_val,
                                               int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    AdminApproval *list = NULL;
    size_t count = 0;
    if (admin_list_pending_approvals(g_ctx->db, g_ctx->channel_name, &list, &count) != 0)
        return JS_NewArray(ctx);
    JSValue arr = admin_approvals_to_js(ctx, list, count);
    admin_approvals_free(list, count);
    return arr;
}

/* admin.dashboardUrl() — tokenized /admin URL for the /admin chat command
 * (same trust as the old /key flow, which passed API keys through chat). */
static JSValue js_admin_dashboard_url(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    char *url = dashboard_url(g_ctx->db);
    if (!url) return JS_NULL;
    JSValue r = JS_NewString(ctx, url);
    free(url);
    return r;
}

static JSValue js_admin_is_admin(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NewBool(ctx, 0);
    const char *user_id = JS_ToCString(ctx, argv[0]);
    if (!user_id) return JS_NewBool(ctx, 0);
    int found = channel_id_is_admin(g_ctx->db, g_ctx->channel_name, user_id);
    JS_FreeCString(ctx, user_id);
    return JS_NewBool(ctx, found);
}

/* ── Registration entry point ──────────────────────────────────── */

void qjs_register_channel_host_functions(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);

    /* channel object */
    JSValue ch = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ch, "emit", JS_NewCFunction(ctx, js_ch_emit, "emit", 3));
    JS_SetPropertyStr(ctx, ch, "send", JS_NewCFunction(ctx, js_ch_send, "send", 1));
    JS_SetPropertyStr(ctx, ch, "http", JS_NewCFunction(ctx, js_ch_http, "http", 1));
    JS_SetPropertyStr(ctx, ch, "getConfig", JS_NewCFunction(ctx, js_ch_get_config, "getConfig", 1));
    JS_SetPropertyStr(ctx, ch, "getState", JS_NewCFunction(ctx, js_ch_get_state, "getState", 1));
    JS_SetPropertyStr(ctx, ch, "setState", JS_NewCFunction(ctx, js_ch_set_state, "setState", 2));
    JS_SetPropertyStr(ctx, ch, "ackOutbox", JS_NewCFunction(ctx, js_ch_ack_outbox, "ackOutbox", 1));
    JS_SetPropertyStr(ctx, ch, "failOutbox", JS_NewCFunction(ctx, js_ch_fail_outbox, "failOutbox", 2));
    JS_SetPropertyStr(ctx, ch, "log", JS_NewCFunction(ctx, js_ch_log, "log", 1));

    /* channel.conn.* — persistent bidirectional connections (WS in v1) */
    JSValue conn = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, conn, "open", JS_NewCFunction(ctx, js_ch_conn_open, "open", 1));
    JS_SetPropertyStr(ctx, conn, "send", JS_NewCFunction(ctx, js_ch_conn_send, "send", 2));
    JS_SetPropertyStr(ctx, conn, "close", JS_NewCFunction(ctx, js_ch_conn_close, "close", 1));
    JS_SetPropertyStr(ctx, ch, "conn", conn);

    JS_SetPropertyStr(ctx, global, "channel", ch);

    /* admin object */
    JSValue admin = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, admin, "isAdmin", JS_NewCFunction(ctx, js_admin_is_admin, "isAdmin", 1));
    JS_SetPropertyStr(ctx, admin, "listPendingApprovals", JS_NewCFunction(ctx, js_admin_list_pending_approvals, "listPendingApprovals", 0));
    JS_SetPropertyStr(ctx, admin, "dashboardUrl", JS_NewCFunction(ctx, js_admin_dashboard_url, "dashboardUrl", 0));
    JS_SetPropertyStr(ctx, ch, "admin", admin);

    /* Date.now() — already available via JS_AddIntrinsicDate in full context */
    JS_FreeValue(ctx, global);
}
