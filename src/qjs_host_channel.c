/* Host C functions for the channel runner (cclaw --channel mode).
 * Registered as cclaw.* and admin.* globals. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "qjs_helpers.h"
#include "channel_api.h"
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

/* Send queue (defined in channel_runner.c) */
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

/* ── cclaw.getConfig(key) ──────────────────────────────────────── */

static JSValue js_ch_get_config(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "getConfig(key)");
    const char *key = JS_ToCString(ctx, argv[0]);
    if (!key) return JS_NULL;
    char *val = channel_get_config(g_ctx, key);
    JS_FreeCString(ctx, key);
    if (!val) return JS_NULL;
    JSValue r = JS_NewString(ctx, val);
    free(val);
    return r;
}

/* ── cclaw.setConfig(key, value) ───────────────────────────────── */

static JSValue js_ch_set_config(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "setConfig(key, value)");
    const char *key = JS_ToCString(ctx, argv[0]);
    const char *val = JS_ToCString(ctx, argv[1]);
    if (!key || !val) {
        if (key) JS_FreeCString(ctx, key);
        if (val) JS_FreeCString(ctx, val);
        return JS_ThrowTypeError(ctx, "setConfig: string args required");
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

/* ── cclaw.send(request) ───────────────────────────────────────── */

static JSValue js_ch_send(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "send(request)");
    JSValueConst o = argv[0];

    char *url = get_str_prop(ctx, (JSValue)o, "url");
    if (!url || !url[0]) { free(url); return JS_ThrowTypeError(ctx, "send: url required"); }

    SendReq *r = calloc(1, sizeof(SendReq));
    if (!r) { free(url); return JS_ThrowTypeError(ctx, "send: OOM"); }
    r->url = url;
    r->method = get_str_prop(ctx, (JSValue)o, "method");
    r->body = get_str_prop(ctx, (JSValue)o, "body");
    r->tag = get_str_prop(ctx, (JSValue)o, "tag");
    r->outbox_id = get_int_prop(ctx, (JSValue)o, "outbox_id", 0);
    r->is_final = get_int_prop(ctx, (JSValue)o, "final", 0);
    r->timeout = get_int_prop(ctx, (JSValue)o, "timeout", 0);

    JSValue h = JS_GetPropertyStr(ctx, o, "headers");
    if (!JS_IsException(h) && !JS_IsUndefined(h) && !JS_IsNull(h)) {
        int n = get_int_prop(ctx, h, "length", 0);
        if (n > 0 && n <= 32) {
            r->headers = calloc((size_t)n, sizeof(char *));
            if (r->headers) {
                for (int i = 0; i < n; i++) {
                    JSValue hv = JS_GetPropertyUint32(ctx, h, (uint32_t)i);
                    const char *hs = JS_ToCString(ctx, hv);
                    JS_FreeValue(ctx, hv);
                    if (hs) { r->headers[r->n_headers++] = strdup(hs); JS_FreeCString(ctx, hs); }
                }
            }
        }
    }
    JS_FreeValue(ctx, h);

    send_queue_push(r);
    return JS_NewInt32(ctx, 0);
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
    const char *channel_id = JS_ToCString(ctx, argv[0]);
    if (!channel_id) return JS_NewBool(ctx, 0);
    char *admins = channel_get_config(g_ctx, "admin_ids");
    if (!admins) { JS_FreeCString(ctx, channel_id); return JS_NewBool(ctx, 0); }
    size_t cid_len = strlen(channel_id);
    int found = 0;
    char *p = admins;
    while (*p) {
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        char *end = p;
        while (*end && *end != ',' && *end != ' ') end++;
        if ((size_t)(end - p) == cid_len && strncmp(p, channel_id, cid_len) == 0) { found = 1; break; }
        p = end;
    }
    free(admins);
    JS_FreeCString(ctx, channel_id);
    return JS_NewBool(ctx, found);
}

/* ── Registration entry point ──────────────────────────────────── */

void qjs_register_channel_host_functions(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);

    /* channel object */
    JSValue ch = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, ch, "emit", JS_NewCFunction(ctx, js_ch_emit, "emit", 3));
    JS_SetPropertyStr(ctx, ch, "send", JS_NewCFunction(ctx, js_ch_send, "send", 1));
    JS_SetPropertyStr(ctx, ch, "getConfig", JS_NewCFunction(ctx, js_ch_get_config, "getConfig", 1));
    JS_SetPropertyStr(ctx, ch, "setConfig", JS_NewCFunction(ctx, js_ch_set_config, "setConfig", 2));
    JS_SetPropertyStr(ctx, ch, "ackOutbox", JS_NewCFunction(ctx, js_ch_ack_outbox, "ackOutbox", 1));
    JS_SetPropertyStr(ctx, ch, "failOutbox", JS_NewCFunction(ctx, js_ch_fail_outbox, "failOutbox", 2));
    JS_SetPropertyStr(ctx, ch, "log", JS_NewCFunction(ctx, js_ch_log, "log", 1));
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
