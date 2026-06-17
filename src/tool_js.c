#define _GNU_SOURCE
#include "tool_js.h"
#include "tool_parse.h"
#include <mquickjs.h>
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

extern const JSSTDLibraryDef js_std_library_main;

static int interrupt_handler(JSContext *ctx, void *opaque) {
    (void)ctx;
    JsHostCtx *hctx = (JsHostCtx *)opaque;
    hctx->instruction_count++;
    return hctx->instruction_count > hctx->instruction_limit;
}

/* Eval JS code in a fresh sandboxed context. Returns heap-allocated result. */
static char *js_eval_code_with_hosts(const char *code, char **hosts, size_t hosts_count) {
    size_t code_len = strlen(code);

    void *heap = malloc(JS_HEAP_SIZE);
    if (!heap) return strdup("error: out of memory");

    JSContext *ctx = JS_NewContext(heap, JS_HEAP_SIZE, &js_std_library_main);
    if (!ctx) { free(heap); return strdup("error: JS context creation failed"); }

    JsHostCtx hctx = {.instruction_count = 0, .instruction_limit = JS_MAX_INSTRUCTIONS,
                       .allowed_hosts = hosts, .allowed_hosts_count = hosts_count};
    JS_SetInterruptHandler(ctx, interrupt_handler);
    JS_SetContextOpaque(ctx, &hctx);

    JSValue val = JS_Eval(ctx, code, code_len, "<eval>", JS_EVAL_RETVAL);

    char *result;
    if (JS_IsException(val)) {
        JSValue exc = JS_GetException(ctx);
        JSCStringBuf buf;
        const char *msg = JS_ToCString(ctx, exc, &buf);
        if (msg) {
            size_t len = strlen(msg) + 16;
            result = malloc(len);
            if (result) snprintf(result, len, "error: %s", msg);
            else result = strdup("error: OOM");
        } else {
            result = strdup("error: exception (no message)");
        }
    } else if (hctx.instruction_count > JS_MAX_INSTRUCTIONS) {
        result = strdup("error: instruction limit exceeded (10M)");
    } else if (JS_IsUndefined(val)) {
        result = strdup("undefined");
    } else if (JS_IsNull(val)) {
        result = strdup("null");
    } else {
        JSCStringBuf buf;
        const char *str = JS_ToCString(ctx, val, &buf);
        result = str ? strdup(str) : strdup("error: cannot convert result to string");
    }

    JS_FreeContext(ctx);
    free(heap);
    return result;
}

/* Eval JS code in a persistent session runtime. Returns heap-allocated result. */
static char *js_eval_in_runtime(JsSessionRuntime *rt, const char *code) {
    if (!rt || !rt->ctx) return js_eval_code_with_hosts(code, NULL, 0);

    JSContext *ctx = (JSContext *)rt->ctx;
    size_t code_len = strlen(code);

    JSValue val = JS_Eval(ctx, code, code_len, "<tool>", JS_EVAL_RETVAL);

    char *result;
    if (JS_IsException(val)) {
        JSValue exc = JS_GetException(ctx);
        JSCStringBuf buf;
        const char *msg = JS_ToCString(ctx, exc, &buf);
        if (msg) {
            size_t len = strlen(msg) + 16;
            result = malloc(len);
            if (result) snprintf(result, len, "error: %s", msg);
            else result = strdup("error: OOM");
        } else {
            result = strdup("error: exception (no message)");
        }
    } else if (JS_IsUndefined(val)) {
        result = strdup("undefined");
    } else if (JS_IsNull(val)) {
        result = strdup("null");
    } else {
        JSCStringBuf buf;
        const char *str = JS_ToCString(ctx, val, &buf);
        result = str ? strdup(str) : strdup("error: cannot convert result to string");
    }

    return result;
}

static const char *JSEVAL_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"code\":{\"type\":\"string\",\"description\":\"JavaScript code to execute (inline)\"},"
    "\"filename\":{\"type\":\"string\",\"description\":\"Workspace-relative .js file to execute\"},"
    "\"args\":{\"type\":\"object\",\"description\":\"Arguments object passed to file (only with filename)\"}"
    "}}";

#define JSEVAL_MAX_OUTPUT (64 * 1024)
#define JSEVAL_TIMEOUT 120

char *tool_js_eval_handler(const char *arguments, void *user_data) {
    JsEvalCtx *ectx = (JsEvalCtx *)user_data;

    /* Fork-bomb guard. We re-exec the cclaw binary (/proc/self/exe) as
     * `cclaw --mjs_eval`, which main() intercepts and runs to completion — it
     * never re-enters this handler. But if the host binary is NOT cclaw (e.g. a
     * test binary that ignores argv and re-runs its own main), the re-exec would
     * call this handler again and fork without bound. The child sets
     * CCLAW_MJS_GUARD before exec; if we ever see it set on entry, the host
     * failed to intercept --mjs_eval, so refuse to fork. */
    if (getenv("CCLAW_MJS_GUARD"))
        return strdup("error: js_eval recursion guard (host binary did not intercept --mjs_eval)");

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0) return strdup("error: invalid JSON arguments");

    const char *code = targ_str(&ta, "code");
    const char *filename = targ_str(&ta, "filename");
    if ((!code || !code[0]) && (!filename || !filename[0])) {
        tool_parse_free(&ta);
        return strdup("error: must provide 'code' or 'filename'");
    }

    /* Get raw args JSON if present */
    const char *args_raw = NULL;
    size_t args_raw_len = 0;
    char *args_str = NULL;
    if (filename && targ_raw(&ta, "args", &args_raw, &args_raw_len) == 0) {
        args_str = malloc(args_raw_len + 1);
        if (args_str) { memcpy(args_str, args_raw, args_raw_len); args_str[args_raw_len] = '\0'; }
    }

    /* Pipe for child output */
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        tool_parse_free(&ta);
        free(args_str);
        return strdup("error: pipe() failed");
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        tool_parse_free(&ta);
        free(args_str);
        return strdup("error: fork() failed");
    }

    if (pid == 0) {
        /* Child */
        setpgid(0, 0);
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        /* Defuse any accidental self-exec fork bomb: a re-exec that fails to
         * intercept --mjs_eval will see this and refuse to fork again. */
        setenv("CCLAW_MJS_GUARD", "1", 1);

        if (ectx && ectx->host_mode)
            setenv("CCLAW_MJS_HOST", "1", 1);

        /* Set allowed hosts */
        if (ectx && ectx->allowed_hosts_count > 0) {
            size_t csv_len = 0;
            for (size_t i = 0; i < ectx->allowed_hosts_count; i++)
                csv_len += strlen(ectx->allowed_hosts[i]) + 1;
            char *csv = malloc(csv_len);
            if (csv) {
                csv[0] = '\0';
                for (size_t i = 0; i < ectx->allowed_hosts_count; i++) {
                    if (i > 0) strcat(csv, ",");
                    strcat(csv, ectx->allowed_hosts[i]);
                }
                setenv("CCLAW_ALLOWED_HOSTS", csv, 1);
                free(csv);
            }
        } else {
            setenv("CCLAW_ALLOWED_HOSTS", "", 1);
        }

        /* Default to /proc/self/exe (robust to rename/PATH); tests override
         * with CCLAW_MJS_EXE to point at the real cclaw binary. */
        const char *self_exe = getenv("CCLAW_MJS_EXE");
        if (!self_exe || !self_exe[0]) self_exe = "/proc/self/exe";

        if (code) {
            execl(self_exe, "cclaw", "--mjs_eval", "-e", code, (char *)NULL);
        } else if (args_str) {
            execl(self_exe, "cclaw", "--mjs_eval", filename, args_str, (char *)NULL);
        } else {
            execl(self_exe, "cclaw", "--mjs_eval", filename, (char *)NULL);
        }
        _exit(127);
    }

    /* Parent */
    setpgid(pid, pid);
    close(pipefd[1]);
    tool_parse_free(&ta);
    free(args_str);

    char *output = malloc(JSEVAL_MAX_OUTPUT + 1);
    if (!output) {
        close(pipefd[0]);
        kill(-pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return strdup("error: out of memory");
    }

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += JSEVAL_TIMEOUT;

    size_t out_len = 0;
    int timed_out = 0;
    int status = 0;

    while (1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            timed_out = 1;
            break;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(pipefd[0], &rfds);

        struct timeval tv;
        long remaining = deadline.tv_sec - now.tv_sec;
        if (remaining > 1) remaining = 1;
        tv.tv_sec = remaining > 0 ? remaining : 0;
        tv.tv_usec = 100000;

        int sel = select(pipefd[0] + 1, &rfds, NULL, NULL, &tv);
        if (sel > 0) {
            ssize_t n = read(pipefd[0], output + out_len, JSEVAL_MAX_OUTPUT - out_len);
            if (n <= 0) break;
            out_len += (size_t)n;
            if (out_len >= JSEVAL_MAX_OUTPUT) break;
        } else if (sel < 0 && errno != EINTR) {
            break;
        }

        int wr = waitpid(pid, &status, WNOHANG);
        if (wr > 0) {
            while (out_len < JSEVAL_MAX_OUTPUT) {
                ssize_t n = read(pipefd[0], output + out_len, JSEVAL_MAX_OUTPUT - out_len);
                if (n <= 0) break;
                out_len += (size_t)n;
            }
            close(pipefd[0]);
            goto done;
        }
    }

    close(pipefd[0]);

    if (timed_out) {
        kill(-pid, SIGKILL);
        waitpid(pid, NULL, 0);
        output[out_len] = '\0';
        size_t needed = out_len + 64;
        char *result = malloc(needed);
        if (!result) { free(output); return strdup("error: timeout + OOM"); }
        snprintf(result, needed, "[timeout after %ds]\n%s", JSEVAL_TIMEOUT, output);
        free(output);
        return result;
    }

    waitpid(pid, &status, 0);

done:
    output[out_len] = '\0';

    /* Strip trailing newline from child output for clean result */
    if (out_len > 0 && output[out_len - 1] == '\n') output[--out_len] = '\0';

    char *result = strdup(output);
    free(output);
    return result ? result : strdup("error: OOM");
}

int tool_js_eval_register(ToolRegistry *reg, JsEvalCtx *ctx) {
    return tools_register(reg, "js_eval",
                          "Run JavaScript in MicroQuickJS (ES5 only: var, function(){}, string concat; "
                          "NO let/const, arrow =>, template literals, for...of, destructuring, async). "
                          "Top-level 'this' is not enumerable (do not iterate it). "
                          "Globals: fs.readDir(path), fs.readFile(path), fs.writeFile(path, data), "
                          "fs.stat(path), fs.cwd(). "
                          "Network: fetch(url, opts) / http_fetch(url, opts) — only allow-listed hosts work; "
                          "to add one, call request_config with {\"action\":\"grant_host\",\"host\":\"...\"}. "
                          "Returns the last expression value (or console.log output).",
                          JSEVAL_PARAMS_JSON, tool_js_eval_handler, ctx);
}

/* --- JS-defined tool support (extension-path) --- */

/* User data for JS-defined tool handler: code + runtime pointer */
typedef struct {
    char *code;
    JsSessionRuntime *rt;
} JsToolData;

/* Handler for JS-defined tools. user_data points to JsToolData. */
static char *js_defined_tool_handler(const char *arguments, void *user_data) {
    JsToolData *td = (JsToolData *)user_data;
    if (!td || !td->code) return strdup("error: no code for this tool");

    /* Wrap: (function(args){<code>})(JSON.parse('<escaped_args>')) */
    const char *args_str = arguments ? arguments : "{}";

    /* Escape single quotes and backslashes for embedding in JS string literal */
    size_t args_len = strlen(args_str);
    size_t escaped_cap = args_len * 2 + 1;
    char *escaped = malloc(escaped_cap);
    if (!escaped) { return strdup("error: OOM"); }
    size_t j = 0;
    for (size_t i = 0; i < args_len; i++) {
        if (args_str[i] == '\'' || args_str[i] == '\\') {
            escaped[j++] = '\\';
        }
        escaped[j++] = args_str[i];
    }
    escaped[j] = '\0';

    size_t wrap_len = strlen(td->code) + j + 64;
    char *wrapped = malloc(wrap_len);
    if (!wrapped) { free(escaped); return strdup("error: OOM"); }
    snprintf(wrapped, wrap_len, "(function(args){%s})(JSON.parse('%s'))", td->code, escaped);
    free(escaped);

    char *result = js_eval_in_runtime(td->rt, wrapped);
    free(wrapped);
    return result;
}



/* Free function for JsToolData */
static void js_tool_data_free(void *user_data) {
    JsToolData *td = (JsToolData *)user_data;
    if (td) { free(td->code); free(td); }
}

/* Register a single JS-defined tool into the registry with shared runtime.
 * Exported for use by extension.c (T256). */
int js_tool_register_ext(ToolRegistry *reg, const char *name,
                                const char *description, const char *parameters_json,
                                const char *code, JsSessionRuntime *rt) {
    /* Check if already registered (re-define overwrites) */
    ToolEntry *existing = tools_lookup(reg, name);
    if (existing) {
        JsToolData *td = (JsToolData *)existing->user_data;
        free(td->code);
        td->code = strdup(code);
        td->rt = rt;
        free(existing->description);
        free(existing->parameters_json);
        existing->description = description ? strdup(description) : NULL;
        existing->parameters_json = parameters_json ? strdup(parameters_json) : NULL;
        existing->handler = js_defined_tool_handler;
        return 0;
    }
    JsToolData *td = malloc(sizeof(JsToolData));
    if (!td) return -1;
    td->code = strdup(code);
    td->rt = rt;
    if (!td->code) { free(td); return -1; }
    int rc = tools_register(reg, name, description, parameters_json,
                            js_defined_tool_handler, td);
    if (rc == 0) {
        ToolEntry *e = tools_lookup(reg, name);
        if (e) e->free_fn = js_tool_data_free;
    }
    return rc;
}







/* --- Session runtime --- */

JsSessionRuntime *js_runtime_create(void) {
    JsSessionRuntime *rt = malloc(sizeof(JsSessionRuntime));
    if (!rt) return NULL;
    rt->heap = malloc(JS_HEAP_SIZE);
    if (!rt->heap) { free(rt); return NULL; }
    rt->ctx = JS_NewContext(rt->heap, JS_HEAP_SIZE, &js_std_library_main);
    if (!rt->ctx) { free(rt->heap); free(rt); return NULL; }
    return rt;
}

void js_runtime_destroy(JsSessionRuntime *rt) {
    if (!rt) return;
    if (rt->ctx) {
        /* Free the JsHostCtx if set */
        void *opaque = JS_GetContextOpaque((JSContext *)rt->ctx);
        free(opaque);
        JS_FreeContext((JSContext *)rt->ctx);
    }
    free(rt->heap);
    free(rt);
}

void js_runtime_set_hosts(JsSessionRuntime *rt, char **hosts, size_t count) {
    if (!rt || !rt->ctx) return;
    JSContext *ctx = (JSContext *)rt->ctx;
    JsHostCtx *hctx = (JsHostCtx *)JS_GetContextOpaque(ctx);
    if (!hctx) {
        hctx = calloc(1, sizeof(JsHostCtx));
        if (!hctx) return;
        JS_SetContextOpaque(ctx, hctx);
    }
    hctx->allowed_hosts = hosts;
    hctx->allowed_hosts_count = count;
}

void js_runtime_set_registry(JsSessionRuntime *rt, ToolRegistry *reg) {
    if (!rt || !rt->ctx) return;
    JSContext *ctx = (JSContext *)rt->ctx;
    JsHostCtx *hctx = (JsHostCtx *)JS_GetContextOpaque(ctx);
    if (!hctx) {
        hctx = calloc(1, sizeof(JsHostCtx));
        if (!hctx) return;
        JS_SetContextOpaque(ctx, hctx);
    }
    hctx->tool_registry = reg;
}


