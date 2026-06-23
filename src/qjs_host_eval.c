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

/* ── Dual-mode return: tuple or callback ───────────────────────── */
/* If cb is a function, invoke cb(err, result) Node-style and return undefined.
 * Otherwise return [result, errno] tuple (Go-style). */

static JSValue fs_return(JSContext *ctx, JSValueConst cb, JSValue result, int err) {
    if (JS_IsFunction(ctx, cb)) {
        JSValue args[2];
        args[0] = err ? JS_NewString(ctx, strerror(err)) : JS_NULL;
        args[1] = err ? JS_NULL : result;
        JSValue ret = JS_Call(ctx, cb, JS_UNDEFINED, 2, args);
        JS_FreeValue(ctx, args[0]);
        if (err) JS_FreeValue(ctx, result);
        if (JS_IsException(ret)) {
            JSValue ex = JS_GetException(ctx);
            JS_FreeValue(ctx, ex);
        }
        JS_FreeValue(ctx, ret);
        return JS_UNDEFINED;
    }
    /* Tuple mode */
    if (err) {
        JS_FreeValue(ctx, result);
        JSValue arr = JS_NewArray(ctx);
        JS_SetPropertyUint32(ctx, arr, 0, JS_NULL);
        JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, err));
        return arr;
    }
    JSValue arr = JS_NewArray(ctx);
    JS_SetPropertyUint32(ctx, arr, 0, result);
    JS_SetPropertyUint32(ctx, arr, 1, JS_NewInt32(ctx, 0));
    return arr;
}

/* Detect callback: last arg if it's a function */
static JSValueConst fs_get_cb(JSContext *ctx, int argc, JSValueConst *argv, int cb_pos) {
    if (argc > cb_pos && JS_IsFunction(ctx, argv[cb_pos]))
        return argv[cb_pos];
    return JS_UNDEFINED;
}

/* ── fs.readFile(path[, cb]) ────────────────────────────────────── */

static JSValue js_fs_readFile(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "fs.readFile: path required");
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_ThrowTypeError(ctx, "fs.readFile: path must be a string");
    JSValueConst cb = fs_get_cb(ctx, argc, argv, 1);

    FILE *f = fopen(path, "rb");
    if (!f) { int e = errno; JS_FreeCString(ctx, path); return fs_return(ctx, cb, JS_NULL, e); }
    JS_FreeCString(ctx, path);

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return fs_return(ctx, cb, JS_NULL, EIO); }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz > 0 ? (size_t)sz : 1);
    if (!buf) { fclose(f); return fs_return(ctx, cb, JS_NULL, ENOMEM); }
    size_t nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    JSValue content = JS_NewStringLen(ctx, buf, nread);
    free(buf);
    return fs_return(ctx, cb, content, 0);
}

/* ── fs.writeFile(path, content[, cb]) ──────────────────────────── */

static JSValue js_fs_writeFile(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "fs.writeFile: path and content required");
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_ThrowTypeError(ctx, "fs.writeFile: path must be a string");
    JSValueConst cb = fs_get_cb(ctx, argc, argv, 2);

    size_t clen = 0;
    const char *content = JS_ToCStringLen(ctx, &clen, argv[1]);
    if (!content) { JS_FreeCString(ctx, path); return JS_ThrowTypeError(ctx, "fs.writeFile: content must be a string"); }

    FILE *f = fopen(path, "wb");
    if (!f) { int e = errno; JS_FreeCString(ctx, path); JS_FreeCString(ctx, content); return fs_return(ctx, cb, JS_NULL, e); }
    JS_FreeCString(ctx, path);
    size_t written = fwrite(content, 1, clen, f);
    int err = ferror(f) ? errno : 0;
    fclose(f);
    JS_FreeCString(ctx, content);

    return fs_return(ctx, cb, JS_NewInt32(ctx, (int32_t)written), err);
}

/* ── fs.readdir(path[, cb]) ─────────────────────────────────────── */

static const char *dtype_str(unsigned char d_type) {
    switch (d_type) {
    case DT_DIR: return "dir";
    case DT_LNK: return "link";
    case DT_REG: return "file";
    default:     return "file";
    }
}

static JSValue js_fs_readDir(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_ThrowTypeError(ctx, "fs.readdir: path required");
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_ThrowTypeError(ctx, "fs.readdir: path must be a string");
    JSValueConst cb = fs_get_cb(ctx, argc, argv, 1);

    DIR *d = opendir(path);
    if (!d) { int e = errno; JS_FreeCString(ctx, path); return fs_return(ctx, cb, JS_NULL, e); }
    JS_FreeCString(ctx, path);

    JSValue entries = JS_NewArray(ctx);
    struct dirent *ent;
    uint32_t idx = 0;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' || (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "name", JS_NewString(ctx, ent->d_name));
        JS_SetPropertyStr(ctx, obj, "type", JS_NewString(ctx, dtype_str(ent->d_type)));
        JS_SetPropertyUint32(ctx, entries, idx++, obj);
    }
    closedir(d);

    return fs_return(ctx, cb, entries, 0);
}

/* ── fs.stat / fs.lstat shared implementation ──────────────────── */

static JSValue js_fs_stat_impl(JSContext *ctx, int argc, JSValueConst *argv,
                               int use_lstat) {
    const char *fn = use_lstat ? "fs.lstat" : "fs.stat";
    if (argc < 1) return JS_ThrowTypeError(ctx, "%s: path required", fn);
    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_ThrowTypeError(ctx, "%s: path must be a string", fn);
    JSValueConst cb = fs_get_cb(ctx, argc, argv, 1);

    struct stat st;
    int rc = use_lstat ? lstat(path, &st) : stat(path, &st);
    if (rc != 0) { int e = errno; JS_FreeCString(ctx, path); return fs_return(ctx, cb, JS_NULL, e); }
    JS_FreeCString(ctx, path);

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "size", JS_NewFloat64(ctx, (double)st.st_size));
    JS_SetPropertyStr(ctx, obj, "mode", JS_NewInt32(ctx, (int32_t)st.st_mode));
    JS_SetPropertyStr(ctx, obj, "mtime", JS_NewFloat64(ctx, (double)st.st_mtime));
    JS_SetPropertyStr(ctx, obj, "isDir", JS_NewBool(ctx, S_ISDIR(st.st_mode)));
    JS_SetPropertyStr(ctx, obj, "isLink", JS_NewBool(ctx, S_ISLNK(st.st_mode)));

    return fs_return(ctx, cb, obj, 0);
}

static JSValue js_fs_stat(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv) {
    (void)this_val;
    return js_fs_stat_impl(ctx, argc, argv, 0);
}

static JSValue js_fs_lstat(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv) {
    (void)this_val;
    return js_fs_stat_impl(ctx, argc, argv, 1);
}

/* ── fs.cwd() ──────────────────────────────────────────────────── */

static JSValue js_fs_cwd(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    char buf[4096];
    if (!getcwd(buf, sizeof(buf))) return fs_return(ctx, JS_UNDEFINED, JS_NULL, errno);
    return fs_return(ctx, JS_UNDEFINED, JS_NewString(ctx, buf), 0);
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
            if (JS_IsException(json)) {
                JSValue ex = JS_GetException(ctx);
                JS_FreeValue(ctx, ex);
                s = JS_ToCString(ctx, argv[i]);
            } else {
                s = JS_ToCString(ctx, json);
                JS_FreeValue(ctx, json);
            }
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

    /* fs object — both Node callback style and Sync style point to same impl */
    JSValue fs = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, fs, "readFile", JS_NewCFunction(ctx, js_fs_readFile, "readFile", 2));
    JS_SetPropertyStr(ctx, fs, "readFileSync", JS_NewCFunction(ctx, js_fs_readFile, "readFileSync", 1));
    JS_SetPropertyStr(ctx, fs, "writeFile", JS_NewCFunction(ctx, js_fs_writeFile, "writeFile", 3));
    JS_SetPropertyStr(ctx, fs, "writeFileSync", JS_NewCFunction(ctx, js_fs_writeFile, "writeFileSync", 2));
    JS_SetPropertyStr(ctx, fs, "readdir", JS_NewCFunction(ctx, js_fs_readDir, "readdir", 2));
    JS_SetPropertyStr(ctx, fs, "readdirSync", JS_NewCFunction(ctx, js_fs_readDir, "readdirSync", 1));
    JS_SetPropertyStr(ctx, fs, "stat", JS_NewCFunction(ctx, js_fs_stat, "stat", 2));
    JS_SetPropertyStr(ctx, fs, "statSync", JS_NewCFunction(ctx, js_fs_stat, "statSync", 1));
    JS_SetPropertyStr(ctx, fs, "lstat", JS_NewCFunction(ctx, js_fs_lstat, "lstat", 2));
    JS_SetPropertyStr(ctx, fs, "lstatSync", JS_NewCFunction(ctx, js_fs_lstat, "lstatSync", 1));
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
