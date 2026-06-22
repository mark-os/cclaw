/* Host C functions for the --qjs_eval child process.
 * Registered into the QuickJS context as globals: http_request, fs.*, console.log, print. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "qjs_helpers.h"
#include "js_http_fetch.h"
#include "external_content.h"
#include "tool_web_fetch.h"
#include "tool_js.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── http_request(url[, opts]) ─────────────────────────────────── */

static JSValue js_http_request(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    JsHostCtx *hctx = (JsHostCtx *)JS_GetContextOpaque(ctx);
    char **hosts = hctx ? hctx->allowed_hosts : NULL;
    size_t hosts_count = hctx ? hctx->allowed_hosts_count : 0;

    if (argc < 1)
        return JS_ThrowTypeError(ctx, "http_request: url argument required");

    const char *url = JS_ToCString(ctx, argv[0]);
    if (!url)
        return JS_ThrowTypeError(ctx, "http_request: url must be a string");

    const char *method = NULL, *body = NULL;
    int sanitize = 0;

    if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        JSValue m = JS_GetPropertyStr(ctx, argv[1], "method");
        if (!JS_IsUndefined(m) && !JS_IsNull(m)) method = JS_ToCString(ctx, m);
        JS_FreeValue(ctx, m);
        JSValue b = JS_GetPropertyStr(ctx, argv[1], "body");
        if (!JS_IsUndefined(b) && !JS_IsNull(b)) body = JS_ToCString(ctx, b);
        JS_FreeValue(ctx, b);
        JSValue s = JS_GetPropertyStr(ctx, argv[1], "sanitize");
        if (JS_ToBool(ctx, s)) sanitize = 1;
        JS_FreeValue(ctx, s);
    }

    JsHttpResult r = js_http_fetch_exec(url, method, body, hosts, hosts_count);
    JS_FreeCString(ctx, url);
    if (method) JS_FreeCString(ctx, method);
    if (body) JS_FreeCString(ctx, body);

    if (r.status < 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "http_request: %s", r.error ? r.error : "unknown error");
        js_http_result_free(&r);
        return JS_ThrowTypeError(ctx, "%s", msg);
    }

    JSValue result = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, result, "status", JS_NewInt32(ctx, r.status));

    if (r.body && sanitize) {
        size_t cap = r.body_len + 1;
        char *text = malloc(cap);
        if (text) {
            size_t tlen = html_strip_tags(r.body, text, cap);
            char *wrapped = wrap_external_content(text, tlen, "http_request");
            if (wrapped) {
                JS_SetPropertyStr(ctx, result, "body", JS_NewStringLen(ctx, wrapped, strlen(wrapped)));
                free(wrapped);
            } else {
                JS_SetPropertyStr(ctx, result, "body", JS_NewStringLen(ctx, text, tlen));
            }
            free(text);
        } else {
            JS_SetPropertyStr(ctx, result, "body", JS_NewStringLen(ctx, r.body, r.body_len));
        }
    } else if (r.body) {
        JS_SetPropertyStr(ctx, result, "body", JS_NewStringLen(ctx, r.body, r.body_len));
    } else {
        JS_SetPropertyStr(ctx, result, "body", JS_NewString(ctx, ""));
    }

    js_http_result_free(&r);
    return result;
}

/* ── fs.readFile(path) → [content, errno] ──────────────────────── */

static JSValue make_err_tuple(JSContext *ctx, int err) {
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NULL);
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, err));
    return arr;
}

static JSValue js_fs_readFile(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "fs.readFile: path required");
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_ThrowTypeError(ctx, "fs.readFile: path must be a string");

    FILE *f = fopen(path, "rb");
    if (!f) { int e = errno; JS_FreeCString(ctx, path); return make_err_tuple(ctx, e); }
    JS_FreeCString(ctx, path);

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz > 0 ? (size_t)sz : 1);
    if (!buf) { fclose(f); return make_err_tuple(ctx, ENOMEM); }
    size_t nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewStringLen(ctx, buf, nread));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, 0));
    free(buf);
    return arr;
}

/* ── fs.writeFile(path, content) → [written, errno] ────────────── */

static JSValue js_fs_writeFile(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "fs.writeFile: path and content required");
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_ThrowTypeError(ctx, "fs.writeFile: path must be a string");

    size_t clen = 0;
    const char *content = JS_ToCStringLen(ctx, &clen, argv[1]);
    if (!content) { JS_FreeCString(ctx, path); return JS_ThrowTypeError(ctx, "fs.writeFile: content must be a string"); }

    FILE *f = fopen(path, "wb");
    if (!f) { int e = errno; JS_FreeCString(ctx, path); JS_FreeCString(ctx, content); return make_err_tuple(ctx, e); }
    JS_FreeCString(ctx, path);
    size_t written = fwrite(content, 1, clen, f);
    int err = ferror(f) ? errno : 0;
    fclose(f);
    JS_FreeCString(ctx, content);

    if (err) return make_err_tuple(ctx, err);
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewInt32(ctx, (int32_t)written));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, 0));
    return arr;
}

/* ── fs.readDir(path) → [names[], errno] ───────────────────────── */

static JSValue js_fs_readDir(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "fs.readDir: path required");
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_ThrowTypeError(ctx, "fs.readDir: path must be a string");

    DIR *d = opendir(path);
    if (!d) { int e = errno; JS_FreeCString(ctx, path); return make_err_tuple(ctx, e); }
    JS_FreeCString(ctx, path);

    JSValue names = JS_NewArray(ctx);
    struct dirent *ent;
    uint32_t idx = 0;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' || (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;
        JS_SetPropertyUint32(ctx, names, idx++, JS_NewString(ctx, ent->d_name));
    }
    closedir(d);

    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, names);
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, 0));
    return arr;
}

/* ── fs.stat(path) → [{size,mode,mtime,isDir}, errno] ─────────── */

static JSValue js_fs_stat(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "fs.stat: path required");
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_ThrowTypeError(ctx, "fs.stat: path must be a string");

    struct stat st;
    if (stat(path, &st) != 0) { int e = errno; JS_FreeCString(ctx, path); return make_err_tuple(ctx, e); }
    JS_FreeCString(ctx, path);

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "size", JS_NewFloat64(ctx, (double)st.st_size));
    JS_SetPropertyStr(ctx, obj, "mode", JS_NewInt32(ctx, (int32_t)st.st_mode));
    JS_SetPropertyStr(ctx, obj, "mtime", JS_NewFloat64(ctx, (double)st.st_mtime));
    JS_SetPropertyStr(ctx, obj, "isDir", JS_NewBool(ctx, S_ISDIR(st.st_mode)));

    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, obj);
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, 0));
    return arr;
}

/* ── fs.cwd() → [path, errno] ─────────────────────────────────── */

static JSValue js_fs_cwd(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    char buf[4096];
    if (!getcwd(buf, sizeof(buf))) return make_err_tuple(ctx, errno);
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, JS_NewString(ctx, buf));
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, 0));
    return arr;
}

/* ── console.log / print ───────────────────────────────────────── */

static JSValue js_console_log(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    (void)this_val;
    /* Append to __console_buf global (prelude creates it) */
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue buf_arr = JS_GetPropertyStr(ctx, global, "__console_buf");

    /* Build the line: stringify each arg, join with space */
    JSValue parts = JS_NewArray(ctx);
    for (int i = 0; i < argc; i++) {
        const char *s;
        if (JS_IsObject(argv[i])) {
            JSValue json = JS_JSONStringify(ctx, argv[i], JS_UNDEFINED, JS_UNDEFINED);
            s = JS_ToCString(ctx, json);
            JS_FreeValue(ctx, json);
        } else {
            s = JS_ToCString(ctx, argv[i]);
        }
        if (s) {
            JS_SetPropertyUint32(ctx, parts, (uint32_t)i, JS_NewString(ctx, s));
            JS_FreeCString(ctx, s);
        }
    }
    /* Join parts with space via JS */
    JSValue join_fn = JS_GetPropertyStr(ctx, parts, "join");
    JSValue space = JS_NewString(ctx, " ");
    JSValue line = JS_Call(ctx, join_fn, parts, 1, &space);
    JS_FreeValue(ctx, space);
    JS_FreeValue(ctx, join_fn);
    JS_FreeValue(ctx, parts);

    /* Push to __console_buf */
    JSValue push_fn = JS_GetPropertyStr(ctx, buf_arr, "push");
    JS_Call(ctx, push_fn, buf_arr, 1, &line);
    JS_FreeValue(ctx, push_fn);
    JS_FreeValue(ctx, line);
    JS_FreeValue(ctx, buf_arr);
    JS_FreeValue(ctx, global);
    return JS_UNDEFINED;
}

/* ── Registration entry point ──────────────────────────────────── */

void qjs_register_eval_host_functions(JSContext *ctx) {
    JSValue global = JS_GetGlobalObject(ctx);

    /* http_request */
    JS_SetPropertyStr(ctx, global, "http_request",
        JS_NewCFunction(ctx, js_http_request, "http_request", 2));

    /* fs object */
    JSValue fs = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, fs, "readFile", JS_NewCFunction(ctx, js_fs_readFile, "readFile", 1));
    JS_SetPropertyStr(ctx, fs, "writeFile", JS_NewCFunction(ctx, js_fs_writeFile, "writeFile", 2));
    JS_SetPropertyStr(ctx, fs, "readDir", JS_NewCFunction(ctx, js_fs_readDir, "readDir", 1));
    JS_SetPropertyStr(ctx, fs, "stat", JS_NewCFunction(ctx, js_fs_stat, "stat", 1));
    JS_SetPropertyStr(ctx, fs, "cwd", JS_NewCFunction(ctx, js_fs_cwd, "cwd", 0));
    JS_SetPropertyStr(ctx, global, "fs", fs);

    /* console.log + print */
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, js_console_log, "log", 0));
    JS_SetPropertyStr(ctx, console, "warn", JS_NewCFunction(ctx, js_console_log, "warn", 0));
    JS_SetPropertyStr(ctx, console, "error", JS_NewCFunction(ctx, js_console_log, "error", 0));
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_SetPropertyStr(ctx, global, "print", JS_NewCFunction(ctx, js_console_log, "print", 0));

    JS_FreeValue(ctx, global);
}
