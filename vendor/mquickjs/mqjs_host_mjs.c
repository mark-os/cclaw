/* Host-provided JS functions for standalone mjs binary.
 * fetch() communicates over inherited fd (MJS_FETCH_FD env var).
 * V49: zero direct network capability — all requests proxied via fd.
 * V50: protocol: <4B len big-endian><JSON> for request and response. */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <poll.h>

#define FETCH_TIMEOUT_MS 30000  /* V50: 30s default timeout per fetch call */
#define FETCH_MAX_RESPONSE (10 * 1024 * 1024)  /* 10MB sanity limit */

typedef struct {
    int instruction_count;
    int instruction_limit;
    int fetch_fd;
    void *tool_registry;  /* unused in mjs — padding for struct compat */
    int call_depth;       /* unused in mjs */
} JsHostCtx;

/* V116: callTool stub — not available in standalone mjs */
JSValue js_call_tool(JSContext *ctx, JSValue *this_val,
                     int argc, JSValue *argv)
{
    (void)this_val; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "callTool not available in standalone mjs");
}

/* Date.now() — milliseconds since epoch */
JSValue js_date_now(JSContext *ctx, JSValue *this_val,
                    int argc, JSValue *argv)
{
    (void)this_val; (void)argc; (void)argv;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    double ms = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
    return JS_NewFloat64(ctx, ms);
}

/* Write exactly n bytes to fd. Returns 0 on success, -1 on error. */
static int write_all(int fd, const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* Read exactly n bytes from fd with timeout. Returns 0 on success, -1 on error/timeout. */
static int read_all_timeout(int fd, void *buf, size_t n, int timeout_ms)
{
    char *p = (char *)buf;
    while (n > 0) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pr == 0) return -1; /* timeout */
        ssize_t r = read(fd, p, n);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

/* V50: fetch(url, opts) — serialize request over fd, read response.
 * Returns {status, headers, body, error} object.
 * On timeout/fd error, returns {status:0, error:"..."} instead of throwing. */
JSValue js_http_fetch(JSContext *ctx, JSValue *this_val,
                      int argc, JSValue *argv)
{
    (void)this_val;

    JsHostCtx *hctx = (JsHostCtx *)JS_GetContextOpaque(ctx);
    if (!hctx || hctx->fetch_fd < 0)
        return JS_ThrowTypeError(ctx, "fetch: no proxy fd available (MJS_FETCH_FD not set)");

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "fetch: url argument required");

    JSCStringBuf url_buf;
    const char *url = JS_ToCString(ctx, argv[0], &url_buf);
    if (!url)
        return JS_ThrowTypeError(ctx, "fetch: url must be a string");

    /* Build request JSON: {"url","method","headers","body"} */
    const char *method = NULL;
    const char *body = NULL;
    JSCStringBuf method_buf, body_buf;

    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        JSValue m = JS_GetPropertyStr(ctx, argv[1], "method");
        if (!JS_IsUndefined(m) && !JS_IsNull(m))
            method = JS_ToCString(ctx, m, &method_buf);
        JSValue b = JS_GetPropertyStr(ctx, argv[1], "body");
        if (!JS_IsUndefined(b) && !JS_IsNull(b))
            body = JS_ToCString(ctx, b, &body_buf);
    }

    /* Construct JSON request manually (no cJSON dependency) */
    char req[8192];
    int req_len = snprintf(req, sizeof(req),
        "{\"url\":\"%s\",\"method\":\"%s\",\"headers\":{},\"body\":%s%s%s}",
        url,
        method ? method : "GET",
        body ? "\"" : "null",
        body ? body : "",
        body ? "\"" : "");

    if (req_len < 0 || (size_t)req_len >= sizeof(req))
        return JS_ThrowTypeError(ctx, "fetch: request too large");

    /* V50: send <4B len big-endian><JSON> */
    uint32_t net_len = htonl((uint32_t)req_len);
    if (write_all(hctx->fetch_fd, &net_len, 4) < 0) {
        /* Return error object instead of throwing — graceful handling */
        JSValue err_obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, err_obj, "status", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, err_obj, "error",
                          JS_NewString(ctx, "fetch: write to proxy fd failed"));
        return err_obj;
    }
    if (write_all(hctx->fetch_fd, req, (size_t)req_len) < 0) {
        JSValue err_obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, err_obj, "status", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, err_obj, "error",
                          JS_NewString(ctx, "fetch: write to proxy fd failed"));
        return err_obj;
    }

    /* Read response: <4B len big-endian><JSON> with timeout */
    uint32_t resp_net_len;
    if (read_all_timeout(hctx->fetch_fd, &resp_net_len, 4, FETCH_TIMEOUT_MS) < 0) {
        JSValue err_obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, err_obj, "status", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, err_obj, "error",
                          JS_NewString(ctx, "fetch: timeout or read error from proxy"));
        return err_obj;
    }

    uint32_t resp_len = ntohl(resp_net_len);
    if (resp_len > FETCH_MAX_RESPONSE) {
        JSValue err_obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, err_obj, "status", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, err_obj, "error",
                          JS_NewString(ctx, "fetch: response too large"));
        return err_obj;
    }

    char *resp = (char *)malloc(resp_len + 1);
    if (!resp)
        return JS_ThrowTypeError(ctx, "fetch: out of memory");

    if (read_all_timeout(hctx->fetch_fd, resp, resp_len, FETCH_TIMEOUT_MS) < 0) {
        free(resp);
        JSValue err_obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, err_obj, "status", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, err_obj, "error",
                          JS_NewString(ctx, "fetch: timeout or read error from proxy"));
        return err_obj;
    }
    resp[resp_len] = '\0';

    /* Parse response JSON via JS_Eval(JSON.parse(...)) */
    size_t escaped_cap = resp_len * 2 + 32;
    char *eval_buf = (char *)malloc(escaped_cap);
    if (!eval_buf) {
        free(resp);
        return JS_ThrowTypeError(ctx, "fetch: out of memory");
    }
    size_t pos = 0;
    memcpy(eval_buf + pos, "JSON.parse('", 12); pos += 12;
    for (uint32_t i = 0; i < resp_len; i++) {
        if (resp[i] == '\'' || resp[i] == '\\') {
            eval_buf[pos++] = '\\';
        } else if (resp[i] == '\n') {
            eval_buf[pos++] = '\\';
            eval_buf[pos++] = 'n';
            continue;
        } else if (resp[i] == '\r') {
            eval_buf[pos++] = '\\';
            eval_buf[pos++] = 'r';
            continue;
        }
        eval_buf[pos++] = resp[i];
    }
    memcpy(eval_buf + pos, "')", 2); pos += 2;
    eval_buf[pos] = '\0';
    free(resp);

    JSValue resp_obj = JS_Eval(ctx, eval_buf, pos, "<fetch_resp>", JS_EVAL_RETVAL);
    free(eval_buf);

    if (JS_IsException(resp_obj)) {
        /* Clear exception, return error object */
        JS_GetException(ctx);
        JSValue err_obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, err_obj, "status", JS_NewInt32(ctx, 0));
        JS_SetPropertyStr(ctx, err_obj, "error",
                          JS_NewString(ctx, "fetch: failed to parse proxy response"));
        return err_obj;
    }

    return resp_obj;
}
