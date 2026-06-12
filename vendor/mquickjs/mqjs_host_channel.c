/* Host-provided JS functions for channel_runner binary.
 * Provides channel API functions via the standard http_fetch/call_tool/date_now slots,
 * plus additional channel-specific functions accessed via __cclaw_call_tool dispatch. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* Forward declarations — implemented in channel_runner.c */
extern JSValue js_ch_emit(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
extern JSValue js_ch_get_config(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
extern JSValue js_ch_set_config(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
extern JSValue js_ch_ack_outbox(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
extern JSValue js_ch_fail_outbox(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
extern JSValue js_ch_send(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
extern JSValue js_ch_log(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
extern JSValue js_admin_set_key(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
extern JSValue js_admin_set_model(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
extern JSValue js_admin_set_endpoint(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
extern JSValue js_admin_add_host(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
extern JSValue js_admin_remove_host(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
extern JSValue js_admin_list_providers(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
extern JSValue js_admin_list_agents(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);
extern JSValue js_admin_is_admin(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv);

/* __cclaw_call_tool — dispatch channel API calls by name.
 * JS calls: __cclaw_call_tool("emit", args) etc.
 * This multiplexes all channel functions through the single call_tool slot. */
JSValue js_call_tool(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "call_tool: name required");

    JSCStringBuf nbuf;
    const char *name = JS_ToCString(ctx, argv[0], &nbuf);
    if (!name) return JS_ThrowTypeError(ctx, "call_tool: name must be string");

    /* Shift args: pass argv+1 to the handler */
    int sub_argc = argc - 1;
    JSValue *sub_argv = (sub_argc > 0) ? argv + 1 : NULL;

    if (strcmp(name, "emit") == 0) return js_ch_emit(ctx, this_val, sub_argc, sub_argv);
    if (strcmp(name, "getConfig") == 0) return js_ch_get_config(ctx, this_val, sub_argc, sub_argv);
    if (strcmp(name, "setConfig") == 0) return js_ch_set_config(ctx, this_val, sub_argc, sub_argv);
    if (strcmp(name, "ackOutbox") == 0) return js_ch_ack_outbox(ctx, this_val, sub_argc, sub_argv);
    if (strcmp(name, "failOutbox") == 0) return js_ch_fail_outbox(ctx, this_val, sub_argc, sub_argv);
    if (strcmp(name, "send") == 0) return js_ch_send(ctx, this_val, sub_argc, sub_argv);
    if (strcmp(name, "log") == 0) return js_ch_log(ctx, this_val, sub_argc, sub_argv);
    if (strcmp(name, "admin.setKey") == 0) return js_admin_set_key(ctx, this_val, sub_argc, sub_argv);
    if (strcmp(name, "admin.setModel") == 0) return js_admin_set_model(ctx, this_val, sub_argc, sub_argv);
    if (strcmp(name, "admin.setEndpoint") == 0) return js_admin_set_endpoint(ctx, this_val, sub_argc, sub_argv);
    if (strcmp(name, "admin.addHost") == 0) return js_admin_add_host(ctx, this_val, sub_argc, sub_argv);
    if (strcmp(name, "admin.removeHost") == 0) return js_admin_remove_host(ctx, this_val, sub_argc, sub_argv);
    if (strcmp(name, "admin.listProviders") == 0) return js_admin_list_providers(ctx, this_val, sub_argc, sub_argv);
    if (strcmp(name, "admin.listAgents") == 0) return js_admin_list_agents(ctx, this_val, sub_argc, sub_argv);
    if (strcmp(name, "admin.isAdmin") == 0) return js_admin_is_admin(ctx, this_val, sub_argc, sub_argv);

    return JS_ThrowTypeError(ctx, "call_tool: unknown function '%s'", name);
}

/* http_fetch — blocking network I/O is not available in channel JS.
 * Outbound requests are described via cclaw.send() and executed by the
 * C event loop. */
JSValue js_http_fetch(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "no blocking fetch in channels; use cclaw.send()");
}

/* Date.now() */
JSValue js_date_now(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    (void)this_val; (void)argc; (void)argv;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    double ms = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
    return JS_NewFloat64(ctx, ms);
}
