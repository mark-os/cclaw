/* T254-T257: Extension discovery, loading, and cclaw API object.
 * V109-V112: Extensions are JS modules evaluated in shared QuickJS context.
 * The cclaw API object provides registerTool, registerHook. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "extension.h"
#include <sqlite3.h>
#include "qjs_helpers.h"
#include "log.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

char **extension_discover(const char *workspace, size_t *count) {
    *count = 0;
    if (!workspace) return NULL;

    size_t cap = 8;
    char **paths = malloc(cap * sizeof(char *));
    if (!paths) return NULL;

    /* Scan workspace/extensions/ for local drafts (index.qjs or *.qjs) */
    char ext_dir[1024];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", workspace);

    DIR *d = opendir(ext_dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char full[2048];
            snprintf(full, sizeof(full), "%s/%s", ext_dir, ent->d_name);
            struct stat st;
            if (stat(full, &st) != 0) continue;

            const char *to_add = NULL;
            char idx_path[2080];
            if (S_ISREG(st.st_mode)) {
                size_t len = strlen(ent->d_name);
                if (len > 4 && strcmp(ent->d_name + len - 4, ".qjs") == 0)
                    to_add = full;
            } else if (S_ISDIR(st.st_mode)) {
                snprintf(idx_path, sizeof(idx_path), "%s/index.qjs", full);
                if (stat(idx_path, &st) == 0 && S_ISREG(st.st_mode))
                    to_add = idx_path;
            }
            if (to_add) {
                if (*count >= cap) { cap *= 2; paths = realloc(paths, cap * sizeof(char *)); }
                paths[*count] = strdup(to_add);
                if (paths[*count]) (*count)++;
            }
        }
        closedir(d);
    }

    if (*count > 1)
        qsort(paths, *count, sizeof(char *), cmp_str);
    return paths;
}

/* DB-aware discovery: load extensions attached to an agent */
char **extension_discover_for_agent(sqlite3 *db, const char *agent_name,
                                    const char *workspace, size_t *count) {
    *count = 0;
    size_t cap = 8;
    char **paths = malloc(cap * sizeof(char *));
    if (!paths) return NULL;

    /* Global extensions attached to this agent */
    if (db && agent_name) {
        const char *sql =
            "SELECT e.path FROM extensions e"
            " JOIN agent_extensions ae ON ae.extension_name=e.name"
            " WHERE ae.agent_name=? AND e.enabled=1;";
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, agent_name, -1, SQLITE_STATIC);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *ext_path = (const char *)sqlite3_column_text(stmt, 0);
                if (!ext_path) continue;
                char idx[2048];
                snprintf(idx, sizeof(idx), "%s/index.qjs", ext_path);
                struct stat st;
                if (stat(idx, &st) == 0) {
                    if (*count >= cap) { cap *= 2; paths = realloc(paths, cap * sizeof(char *)); }
                    paths[*count] = strdup(idx);
                    if (paths[*count]) (*count)++;
                }
            }
            sqlite3_finalize(stmt);
        }
    }

    /* Also scan workspace/extensions/ for local drafts */
    if (workspace) {
        char ext_dir[1024];
        snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", workspace);
        DIR *d = opendir(ext_dir);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL) {
                if (ent->d_name[0] == '.') continue;
                char full[2048];
                snprintf(full, sizeof(full), "%s/%s", ext_dir, ent->d_name);
                struct stat st;
                if (stat(full, &st) != 0) continue;
                const char *to_add = NULL;
                char idx_path[2080];
                if (S_ISREG(st.st_mode)) {
                    size_t len = strlen(ent->d_name);
                    if (len > 4 && strcmp(ent->d_name + len - 4, ".qjs") == 0)
                        to_add = full;
                } else if (S_ISDIR(st.st_mode)) {
                    snprintf(idx_path, sizeof(idx_path), "%s/index.qjs", full);
                    if (stat(idx_path, &st) == 0 && S_ISREG(st.st_mode))
                        to_add = idx_path;
                }
                if (to_add) {
                    if (*count >= cap) { cap *= 2; paths = realloc(paths, cap * sizeof(char *)); }
                    paths[*count] = strdup(to_add);
                    if (paths[*count]) (*count)++;
                }
            }
            closedir(d);
        }
    }

    if (*count > 1)
        qsort(paths, *count, sizeof(char *), cmp_str);
    return paths;
}

void extension_list_free(char **paths, size_t count) {
    if (!paths) return;
    for (size_t i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len <= 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_len) *out_len = rd;
    return buf;
}

/* V111: JS source that defines the cclaw API object. */
static const char CCLAW_API_INIT[] =
    "globalThis.__cclaw_tools = [];\n"
    "globalThis.__cclaw_hooks = {};\n"
    "globalThis.__cclaw_api = {\n"
    "  registerTool: function(def) {\n"
    "    if (!def || !def.name || !def.handler)\n"
    "      throw new Error('registerTool requires {name, handler}');\n"
    "    var code = def.handler;\n"
    "    if (typeof code === 'function') {\n"
    "      throw new Error('registerTool handler must be a string (function body), not a function. '"
    "        + 'QuickJS tools fork+exec — pass a code string.');\n"
    "    }\n"
    "    globalThis.__cclaw_tools.push({\n"
    "      name: def.name,\n"
    "      description: def.description || '',\n"
    "      parameters: def.parameters || {},\n"
    "      policy: def.policy || null,\n"
    "      handler: code\n"
    "    });\n"
    "  },\n"
    "  registerHook: function(event, fn) {\n"
    "    if (typeof fn !== 'function')\n"
    "      throw new Error('registerHook requires a function');\n"
    "    var valid = ['beforeRequest','afterResponse','beforeToolCall',\n"
    "                 'afterToolCall','turnStart','turnEnd'];\n"
    "    if (valid.indexOf(event) < 0)\n"
    "      throw new Error('unknown hook event: ' + event);\n"
    "    if (!globalThis.__cclaw_hooks[event])\n"
    "      globalThis.__cclaw_hooks[event] = [];\n"
    "    globalThis.__cclaw_hooks[event].push(fn);\n"
    "  },\n"
    "};\n";

int hook_event_from_name(const char *name) {
    if (!name) return -1;
    if (strcmp(name, "beforeRequest") == 0) return HOOK_BEFORE_REQUEST;
    if (strcmp(name, "afterResponse") == 0) return HOOK_AFTER_RESPONSE;
    if (strcmp(name, "beforeToolCall") == 0) return HOOK_BEFORE_TOOL_CALL;
    if (strcmp(name, "afterToolCall") == 0) return HOOK_AFTER_TOOL_CALL;
    if (strcmp(name, "turnStart") == 0) return HOOK_TURN_START;
    if (strcmp(name, "turnEnd") == 0) return HOOK_TURN_END;
    return -1;
}

static const char *hook_event_names[] = {
    "beforeRequest", "afterResponse", "beforeToolCall",
    "afterToolCall", "turnStart", "turnEnd"
};

void extension_ctx_init(ExtensionCtx *ctx, JsSessionRuntime *rt) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->rt = rt;
}

void extension_ctx_destroy(ExtensionCtx *ctx) {
    for (int i = 0; i < HOOK_EVENT_COUNT; i++) {
        for (size_t j = 0; j < ctx->hooks[i].count; j++)
            free(ctx->hooks[i].fns[j]);
        free(ctx->hooks[i].fns);
    }
}

/* After extensions loaded, read __cclaw_tools[] and register each. */
static int process_registered_tools(JsSessionRuntime *rt, ToolRegistry *reg,
                                    const Config *cfg, JsEvalCtx *ectx) {
    if (!rt || !rt->ctx) return 0;
    JSContext *ctx = (JSContext *)rt->ctx;

    const char *count_code = "globalThis.__cclaw_tools.length";
    JSValue count_val = JS_Eval(ctx, count_code, strlen(count_code), "<ext>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(count_val)) { JS_FreeValue(ctx, JS_GetException(ctx)); return 0; }
    int count = 0;
    JS_ToInt32(ctx, &count, count_val);
    JS_FreeValue(ctx, count_val);
    if (count <= 0) return 0;

    int registered = 0;
    for (int i = 0; i < count; i++) {
        char code[256];

        snprintf(code, sizeof(code), "globalThis.__cclaw_tools[%d].name", i);
        JSValue name_val = JS_Eval(ctx, code, strlen(code), "<ext>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(name_val)) { JS_FreeValue(ctx, JS_GetException(ctx)); continue; }
        const char *name = JS_ToCString(ctx, name_val);
        JS_FreeValue(ctx, name_val);
        if (!name) continue;

        snprintf(code, sizeof(code), "globalThis.__cclaw_tools[%d].description", i);
        JSValue desc_val = JS_Eval(ctx, code, strlen(code), "<ext>", JS_EVAL_TYPE_GLOBAL);
        const char *desc = "";
        int desc_owned = 0;
        if (!JS_IsException(desc_val)) { desc = JS_ToCString(ctx, desc_val); desc_owned = (desc != NULL); }
        if (!desc) desc = "";

        snprintf(code, sizeof(code), "JSON.stringify(globalThis.__cclaw_tools[%d].parameters)", i);
        JSValue params_val = JS_Eval(ctx, code, strlen(code), "<ext>", JS_EVAL_TYPE_GLOBAL);
        const char *params = "{}";
        int params_owned = 0;
        if (!JS_IsException(params_val)) { params = JS_ToCString(ctx, params_val); params_owned = (params != NULL); }
        if (!params) params = "{}";

        snprintf(code, sizeof(code),
                 "globalThis.__cclaw_tools[%d].policy ? JSON.stringify(globalThis.__cclaw_tools[%d].policy) : null", i, i);
        JSValue pol_val = JS_Eval(ctx, code, strlen(code), "<ext>", JS_EVAL_TYPE_GLOBAL);
        const char *policy = NULL;
        if (!JS_IsException(pol_val)) {
            const char *ps = JS_ToCString(ctx, pol_val);
            if (ps && strcmp(ps, "null") != 0) policy = ps;
            else if (ps) JS_FreeCString(ctx, ps);
        }

        snprintf(code, sizeof(code), "globalThis.__cclaw_tools[%d].handler", i);
        JSValue fn_val = JS_Eval(ctx, code, strlen(code), "<ext>", JS_EVAL_TYPE_GLOBAL);
        const char *fn_src = NULL;
        if (!JS_IsException(fn_val)) fn_src = JS_ToCString(ctx, fn_val);
        JS_FreeValue(ctx, fn_val);
        if (!fn_src) {
            JS_FreeCString(ctx, name);
            if (desc_owned) JS_FreeCString(ctx, desc);
            if (params_owned) JS_FreeCString(ctx, params);
            if (policy) JS_FreeCString(ctx, policy);
            JS_FreeValue(ctx, desc_val);
            JS_FreeValue(ctx, params_val);
            JS_FreeValue(ctx, pol_val);
            continue;
        }

        if (js_tool_register_ext(reg, name, desc, params, fn_src, ectx, policy) == 0) {
            registered++;
            LOG_DEBUG_(cfg, "extension: registered tool '%s'", name);
        }

        JS_FreeCString(ctx, fn_src);
        JS_FreeCString(ctx, name);
        if (desc_owned) JS_FreeCString(ctx, desc);
        if (params_owned) JS_FreeCString(ctx, params);
        if (policy) JS_FreeCString(ctx, policy);
        JS_FreeValue(ctx, desc_val);
        JS_FreeValue(ctx, params_val);
        JS_FreeValue(ctx, pol_val);
    }
    return registered;
}

/* After extensions loaded, read __cclaw_hooks{} and store function source.
 * We extract each hook fn's source via toString() so it can be re-evaled
 * in fresh contexts during dispatch. */
static void process_registered_hooks(JsSessionRuntime *rt, ExtensionCtx *ext_ctx,
                                     const Config *cfg) {
    if (!rt || !rt->ctx || !ext_ctx) return;
    JSContext *ctx = (JSContext *)rt->ctx;

    for (int ev = 0; ev < HOOK_EVENT_COUNT; ev++) {
        char code[128];
        snprintf(code, sizeof(code),
                 "globalThis.__cclaw_hooks['%s'] ? globalThis.__cclaw_hooks['%s'].length : 0",
                 hook_event_names[ev], hook_event_names[ev]);
        JSValue count_val = JS_Eval(ctx, code, strlen(code), "<ext>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(count_val)) { JS_FreeValue(ctx, JS_GetException(ctx)); continue; }
        int count = 0;
        JS_ToInt32(ctx, &count, count_val);
        JS_FreeValue(ctx, count_val);
        if (count <= 0) continue;

        HookList *hl = &ext_ctx->hooks[ev];
        hl->fns = malloc((size_t)count * sizeof(char *));
        if (!hl->fns) continue;
        hl->cap = (size_t)count;

        for (int i = 0; i < count; i++) {
            /* Extract function source: "(function(...){...})" */
            char src_code[256];
            snprintf(src_code, sizeof(src_code),
                     "'(' + globalThis.__cclaw_hooks['%s'][%d].toString() + ')'",
                     hook_event_names[ev], i);
            JSValue src_val = JS_Eval(ctx, src_code, strlen(src_code), "<ext>", JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(src_val)) { JS_FreeValue(ctx, JS_GetException(ctx)); continue; }
            const char *src = JS_ToCString(ctx, src_val);
            JS_FreeValue(ctx, src_val);
            if (src) {
                hl->fns[hl->count] = strdup(src);
                JS_FreeCString(ctx, src);
                if (hl->fns[hl->count]) hl->count++;
            }
        }
        LOG_DEBUG_(cfg, "extension: %zu %s hooks registered",
                  hl->count, hook_event_names[ev]);
    }
}

int extension_load(char **paths, size_t count, JsSessionRuntime *rt,
                   ToolRegistry *reg, const Config *cfg, ExtensionCtx *ext_ctx,
                   JsEvalCtx *ectx) {
    if (!paths || count == 0 || !rt || !rt->ctx) return 0;

    JSContext *ctx = (JSContext *)rt->ctx;

    /* Install cclaw API object before loading extensions */
    JSValue init_val = JS_Eval(ctx, CCLAW_API_INIT, strlen(CCLAW_API_INIT), "<cclaw>",
                               JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(init_val)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        LOG_INFO_(cfg, "extension: failed to init cclaw API");
        return 0;
    }
    JS_FreeValue(ctx, init_val);

    int loaded = 0;
    for (size_t i = 0; i < count; i++) {
        size_t src_len = 0;
        char *src = read_file(paths[i], &src_len);
        if (!src) {
            LOG_INFO_(cfg, "extension: skip %s (unreadable)", paths[i]);
            continue;
        }

        /* Wrap: (function(cclaw){ <source> })(globalThis.__cclaw_api) */
        size_t wrap_len = src_len + 64;
        char *wrapped = malloc(wrap_len);
        if (!wrapped) { free(src); continue; }
        snprintf(wrapped, wrap_len,
                 "(function(cclaw){%s})(globalThis.__cclaw_api)", src);
        free(src);

        JSValue val = JS_Eval(ctx, wrapped, strlen(wrapped), paths[i],
                              JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(val)) {
            JSValue exc = JS_GetException(ctx);
            const char *msg = JS_ToCString(ctx, exc);
            LOG_INFO_(cfg, "extension: skip %s (error: %s)", paths[i],
                     msg ? msg : "unknown");
            if (msg) JS_FreeCString(ctx, msg);
            JS_FreeValue(ctx, exc);
        } else {
            JS_FreeValue(ctx, val);
            loaded++;
            LOG_DEBUG_(cfg, "extension: loaded %s", paths[i]);
        }
        free(wrapped);
    }

    /* Process accumulated registrations from JS → C */
    process_registered_tools(rt, reg, cfg, ectx);
    if (ext_ctx)
        process_registered_hooks(rt, ext_ctx, cfg);

    return loaded;
}
