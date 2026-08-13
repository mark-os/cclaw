#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "tool_js.h"
#include "run_tool.h"
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

/* 1MB heap cap, 10M instruction limit */
#define JS_HEAP_SIZE (1024 * 1024)
#define JS_MAX_INSTRUCTIONS 10000000

static const char *JSEVAL_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"code\":{\"type\":\"string\",\"description\":\"JavaScript code to execute (inline)\"},"
    "\"filename\":{\"type\":\"string\",\"description\":\"Workspace-relative .qjs file to execute\"},"
    "\"args\":{\"type\":\"object\",\"description\":\"Arguments object passed to file (only with filename)\"},"
    "\"save_secret\":{\"type\":\"string\",\"description\":\"Capture a credential from this eval's result: NAME (^[A-Z][A-Z0-9_]*$) stores it encrypted and masks it to {{SECRET:NAME}} — the raw value never enters context\"},"
    "\"save_secret_path\":{\"type\":\"string\",\"description\":\"With save_secret: JSON path (e.g. $.token) selecting the credential in a JSON result; omit to capture the whole trimmed result\"}"
    "}}";

#define JSEVAL_MAX_OUTPUT (64 * 1024)
#define JSEVAL_TIMEOUT 120

/* js_eval is an SBX_JS tool: the daemon dispatch builds a blob and the
 * --run-tool broker evals it in-process via qjs_eval_run inside the sandbox.
 * This handler is therefore NOT the production execution path — the dispatcher
 * never calls it for an EXEC_SANDBOX recipe. It remains as (a) the identity
 * marker js_tool_resolve_request keys on and (b) a host-mode, in-process
 * evaluator for unit tests. No fork, no re-exec — the fork-bomb hazard is gone. */
/* js_eval is an SBX_JS tool: the daemon dispatch ships pre-extracted params
 * and the --run-tool broker evals in-process via qjs_eval_run inside the
 * sandbox. This function is never executed — it exists as the identity
 * marker js_tool_resolve_request keys on (js_eval vs extension tool). */
char *tool_js_eval_handler(const char *arguments, void *user_data) {
    return tool_sandboxed_stub(arguments, user_data);
}

int tool_js_eval_register(ToolRegistry *reg, JsEvalCtx *ctx) {
    int rc = tools_register(reg, "js_eval",
                          "Run JavaScript in QuickJS (modern JS; no modules, no top-level await). "
                          "Examples:\n"
                          "  var data = JSON.parse(fs.readFile('file.json'));\n"
                          "  var r = http_request('https://api.example.com/data'); var obj = JSON.parse(r.body);\n"
                          "  fs.writeFile('out.txt', result); // returns bytes written\n"
                          "http_request(url[, {method, body, headers:{Name:Value}, markdownify}]) "
                          "is synchronous; returns {status, body, error}; "
                          "markdownify:true converts an HTML body to markdown. "
                          "File globals: fs.readFile(path) returns string, "
                          "fs.writeFile(path, data) returns bytes written, "
                          "fs.readdir(path) returns [{name,type},...], "
                          "fs.stat(path) returns {size,isDir,mtime}, fs.cwd() returns string. "
                          "XML.parse(str) parses XML (RSS/Atom/sitemaps) into a "
                          "fast-xml-parser-style object: element names as keys, attributes "
                          "as '@_name', repeated siblings as arrays, text-only elements as "
                          "strings, mixed content under '#text'. "
                          "All throw on error; all accept optional callback(err, result) as last arg. "
                          "Heap is 4MB — JSON.parse works up to ~1MB source; larger data will OOM. "
                          "Only allow-listed hosts work. "
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
/* Identity marker for extension tools (never executed — see above). */
static char *js_defined_tool_handler(const char *arguments, void *user_data) {
    return tool_sandboxed_stub(arguments, user_data);
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

/* Resolve a JS-tier entry: which JsEvalCtx to sandbox with, and — for an
 * extension tool — the handler .qjs path the child must run. js_eval itself
 * returns *out_path=NULL (the model's own code/filename params drive the
 * eval). No JSON is built here: the dispatch site ships wire params. */
int js_tool_resolve_request(const ToolEntry *te, JsEvalCtx **out_ctx,
                            const char **out_path) {
    if (!te || !out_ctx || !out_path) return -1;
    if (te->handler == tool_js_eval_handler) {
        *out_ctx = (JsEvalCtx *)te->user_data;
        *out_path = NULL;
        return 0;
    }
    JsToolData *td = (JsToolData *)te->user_data;
    if (!td || !td->path) return -1;
    *out_ctx = td->ectx;
    *out_path = td->path;
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

char *tool_js_tier_run(const RunToolParsed *q) {
    /* qjs runs in-process in the inner fork (web's twin): netns + proxy +
     * mounts are already applied. code/filename/args arrive as pre-extracted
     * wire params; args is an opaque JSON blob QuickJS itself parses.
     * http_request's curl honors HTTP_PROXY → decide(); fs.* paths are real
     * bind-mounts from the blob's read/write paths. */
    const char *code = run_tool_param_str(q, "code");
    const char *filename = run_tool_param_str(q, "filename");
    if ((!code || !code[0]) && (!filename || !filename[0]))
        return strdup("error: must provide 'code' or 'filename'");
    if (filename && filename[0]) {
        size_t flen = strlen(filename);
        if (flen < 5 || strcmp(filename + flen - 4, ".qjs") != 0)
            return strdup("error: filename must end in .qjs");
    }
    const char *args = filename ? run_tool_param_json(q, "args") : NULL;
    /* The owning extension's config, resolved parent-side (this child has no
     * DB) — absent for a plain js_eval call, which then sees an empty one. */
    const char *ext_config = run_tool_param_json(q, "config");
    char *r = qjs_eval_run(code, filename, args, ext_config);
    return r ? r : strdup("error: js_eval returned null");
}
