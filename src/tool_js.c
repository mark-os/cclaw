#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "tool_js.h"
#include "tool_parse.h"
#include "qjs_helpers.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* V5: 1MB heap cap, 10M instruction limit */
#define JS_HEAP_SIZE (1024 * 1024)
#define JS_MAX_INSTRUCTIONS 10000000

static const char *JSEVAL_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"code\":{\"type\":\"string\",\"description\":\"JavaScript code to execute (inline)\"},"
    "\"filename\":{\"type\":\"string\",\"description\":\"Workspace-relative .qjs file to execute\"},"
    "\"args\":{\"type\":\"object\",\"description\":\"Arguments object passed to file (only with filename)\"}"
    "}}";

#define JSEVAL_MAX_OUTPUT (64 * 1024)
#define JSEVAL_TIMEOUT 120

/* js_eval is an SBX_JS tool: the daemon dispatch builds a blob and the
 * --run-tool broker evals it in-process via qjs_eval_run inside the sandbox.
 * This handler is therefore NOT the production execution path — the dispatcher
 * never calls it for an EXEC_SANDBOX recipe. It remains as (a) the identity
 * marker js_tool_resolve_request keys on and (b) a host-mode, in-process
 * evaluator for unit tests. No fork, no re-exec — the fork-bomb hazard is gone. */
char *tool_js_eval_handler(const char *arguments, void *user_data) {
    (void)user_data;

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0) return strdup("error: invalid JSON arguments");

    const char *code = targ_str(&ta, "code");
    const char *filename = targ_str(&ta, "filename");
    if ((!code || !code[0]) && (!filename || !filename[0])) {
        tool_parse_free(&ta);
        return strdup("error: must provide 'code' or 'filename'");
    }
    if (filename && filename[0]) {
        size_t flen = strlen(filename);
        if (flen < 5 || strcmp(filename + flen - 4, ".qjs") != 0) {
            tool_parse_free(&ta);
            return strdup("error: filename must end in .qjs");
        }
    }

    char *args_str = NULL;
    const char *args_raw = NULL;
    size_t args_raw_len = 0;
    if (filename && targ_raw(&ta, "args", &args_raw, &args_raw_len) == 0) {
        args_str = malloc(args_raw_len + 1);
        if (args_str) { memcpy(args_str, args_raw, args_raw_len); args_str[args_raw_len] = '\0'; }
    }

    char *result = qjs_eval_run(code, filename, args_str);
    free(args_str);
    tool_parse_free(&ta);
    return result;
}

int tool_js_eval_register(ToolRegistry *reg, JsEvalCtx *ctx) {
    int rc = tools_register(reg, "js_eval",
                          "Run JavaScript in QuickJS (ES2025). "
                          "http_request(url[, opts]) is synchronous HTTP. "
                          "File globals: fs.readdir(path[,cb]), fs.readFile(path[,cb]), fs.writeFile(path, data[,cb]), "
                          "fs.stat(path[,cb]), fs.lstat(path[,cb]), fs.cwd(). Only allow-listed hosts work. "
                          "Returns the last expression value (or printed output). "
                          "When using 'filename', must be a .qjs file.",
                          JSEVAL_PARAMS_JSON, tool_js_eval_handler, ctx);
    if (rc == 0)  /* sandboxed broker; qjs runs in-process, egress via proxy */
        tools_set_recipe(reg, "js_eval", (ToolRecipe){EXEC_SANDBOX, SBX_JS, NULL});
    return rc;
}

/* --- JS-defined tool support (extension-path) --- */

typedef struct {
    char *path;       /* absolute handler .qjs path in the shared store */
    JsEvalCtx *ectx;
} JsToolData;

/* An extension tool runs its handler file in the same forked, sandboxed child
 * as js_eval: build {"filename": <path>, "args": <call args>} and reuse the
 * file-eval path. The handler file evaluates with `args` in scope; its last
 * expression (or printed output) is the result. */
static char *js_defined_tool_handler(const char *arguments, void *user_data) {
    JsToolData *td = (JsToolData *)user_data;
    if (!td || !td->path) return strdup("error: no handler path for this tool");
    const char *args_str = (arguments && arguments[0]) ? arguments : "{}";

    size_t esc_cap = strlen(td->path) * 2 + 8;
    char *esc = malloc(esc_cap);
    if (!esc) return strdup("error: OOM");
    size_t esc_len = json_escape(esc, esc_cap, td->path, strlen(td->path));

    size_t blob_len = esc_len + strlen(args_str) + 32;
    char *blob = malloc(blob_len);
    if (!blob) { free(esc); return strdup("error: OOM"); }
    snprintf(blob, blob_len, "{\"filename\":\"%s\",\"args\":%s}", esc, args_str);
    free(esc);

    char *result = tool_js_eval_handler(blob, td->ectx);
    free(blob);
    return result;
}

static void js_tool_data_free(void *user_data) {
    JsToolData *td = (JsToolData *)user_data;
    if (td) { free(td->path); free(td); }
}

int js_tool_register_ext(ToolRegistry *reg, const char *name,
                         const char *description, const char *parameters_json,
                         const char *path, JsEvalCtx *ectx,
                         const char *policy_json) {
    ToolEntry *existing = tools_lookup(reg, name);
    if (existing) {
        JsToolData *td = (JsToolData *)existing->user_data;
        free(td->path);
        td->path = strdup(path);
        td->ectx = ectx;
        free(existing->description);
        free(existing->parameters_json);
        free(existing->policy_json);
        existing->description = description ? strdup(description) : NULL;
        existing->parameters_json = parameters_json ? strdup(parameters_json) : NULL;
        existing->policy_json = policy_json ? strdup(policy_json) : NULL;
        existing->handler = js_defined_tool_handler;
        existing->recipe = (ToolRecipe){EXEC_SANDBOX, SBX_JS, NULL};
        return 0;
    }
    JsToolData *td = malloc(sizeof(JsToolData));
    if (!td) return -1;
    td->path = strdup(path);
    td->ectx = ectx;
    if (!td->path) { free(td); return -1; }
    int rc = tools_register(reg, name, description, parameters_json,
                            js_defined_tool_handler, td);
    if (rc == 0) {
        ToolEntry *e = tools_lookup(reg, name);
        if (e) {
            e->free_fn = js_tool_data_free;
            e->policy_json = policy_json ? strdup(policy_json) : NULL;
            e->recipe = (ToolRecipe){EXEC_SANDBOX, SBX_JS, NULL};
        }
    }
    return rc;
}

/* Resolve a JS-tier entry into its profile + the eval-request JSON for the blob.
 * js_eval (handler == tool_js_eval_handler): args pass through, profile is the
 * JsEvalCtx user_data. Extension tool (handler == js_defined_tool_handler):
 * wrap as {"filename":<path>,"args":<args>}, profile is td->ectx. */
int js_tool_resolve_request(const ToolEntry *te, const char *arguments,
                            JsEvalCtx **out_ctx, char **out_args) {
    if (!te || !out_ctx || !out_args) return -1;
    const char *args = (arguments && arguments[0]) ? arguments : "{}";

    if (te->handler == tool_js_eval_handler) {
        *out_ctx = (JsEvalCtx *)te->user_data;
        *out_args = strdup(args);
        return *out_args ? 0 : -1;
    }

    /* Extension tool: te->user_data is JsToolData {path, ectx}. */
    JsToolData *td = (JsToolData *)te->user_data;
    if (!td || !td->path) return -1;
    *out_ctx = td->ectx;

    size_t esc_cap = strlen(td->path) * 2 + 8;
    char *esc = malloc(esc_cap);
    if (!esc) return -1;
    size_t esc_len = json_escape(esc, esc_cap, td->path, strlen(td->path));
    size_t blen = esc_len + strlen(args) + 32;
    char *blob = malloc(blen);
    if (!blob) { free(esc); return -1; }
    snprintf(blob, blen, "{\"filename\":\"%s\",\"args\":%s}", esc, args);
    free(esc);
    *out_args = blob;
    return 0;
}

/* --- Session runtime (used by extensions for hooks context) --- */

JsSessionRuntime *js_runtime_create(void) {
    JsSessionRuntime *rt = calloc(1, sizeof(JsSessionRuntime));
    if (!rt) return NULL;
    rt->heap = qjs_runtime_create(JS_HEAP_SIZE);
    if (!rt->heap) { free(rt); return NULL; }
    QjsRuntime *qrt = (QjsRuntime *)rt->heap;
    qjs_set_interrupt_limit(qrt, JS_MAX_INSTRUCTIONS);
    rt->ctx = qjs_context_create(qrt, QJS_PROFILE_HOOKS);
    if (!rt->ctx) { qjs_runtime_destroy(qrt); free(rt); return NULL; }
    return rt;
}

void js_runtime_destroy(JsSessionRuntime *rt) {
    if (!rt) return;
    if (rt->ctx) JS_FreeContext((JSContext *)rt->ctx);
    if (rt->heap) qjs_runtime_destroy((QjsRuntime *)rt->heap);
    free(rt);
}

void js_runtime_set_hosts(JsSessionRuntime *rt, char **hosts, size_t count) {
    (void)rt; (void)hosts; (void)count;
    /* Hosts are passed via env to forked child, not needed in session runtime */
}

char *tool_js_tier_run(const RunToolParsed *q) {
    /* qjs runs in-process in the inner fork (web's twin): netns + proxy + mounts
     * are already applied. tool_js_eval_handler parses {code|filename,args} and
     * evals via qjs_eval_run; http_request's curl honors HTTP_PROXY → decide().
     * fs.* paths are real bind-mounts from the blob's read/write paths. */
    char *r = tool_js_eval_handler(q->arguments, NULL);
    return r ? r : strdup("error: js_eval returned null");
}
