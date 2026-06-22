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

char *tool_js_eval_handler(const char *arguments, void *user_data) {
    JsEvalCtx *ectx = (JsEvalCtx *)user_data;

    /* Fork-bomb guard. */
    if (getenv("CCLAW_QJS_GUARD"))
        return strdup("error: js_eval recursion guard (host binary did not intercept --qjs_eval)");

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0) return strdup("error: invalid JSON arguments");

    const char *code = targ_str(&ta, "code");
    const char *filename = targ_str(&ta, "filename");
    if ((!code || !code[0]) && (!filename || !filename[0])) {
        tool_parse_free(&ta);
        return strdup("error: must provide 'code' or 'filename'");
    }

    /* Enforce .qjs extension */
    if (filename && filename[0]) {
        size_t flen = strlen(filename);
        if (flen < 5 || strcmp(filename + flen - 4, ".qjs") != 0) {
            tool_parse_free(&ta);
            return strdup("error: filename must end in .qjs");
        }
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

        setenv("CCLAW_QJS_GUARD", "1", 1);

        if (ectx && ectx->host_mode)
            setenv("CCLAW_MJS_HOST", "1", 1);

        if (ectx && ectx->trust_level)
            setenv("CCLAW_TRUST_LEVEL", ectx->trust_level, 1);

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

        /* Layer 2: pass read/write paths to forked child via env */
        if (ectx && ectx->read_path_count > 0) {
            size_t csv_len = 0;
            for (size_t i = 0; i < ectx->read_path_count; i++)
                csv_len += strlen(ectx->read_paths[i]) + 1;
            char *csv = malloc(csv_len);
            if (csv) {
                csv[0] = '\0';
                for (size_t i = 0; i < ectx->read_path_count; i++) {
                    if (i > 0) strcat(csv, ",");
                    strcat(csv, ectx->read_paths[i]);
                }
                setenv("CCLAW_READ_PATHS", csv, 1);
                free(csv);
            }
        }
        if (ectx && ectx->write_path_count > 0) {
            size_t csv_len = 0;
            for (size_t i = 0; i < ectx->write_path_count; i++)
                csv_len += strlen(ectx->write_paths[i]) + 1;
            char *csv = malloc(csv_len);
            if (csv) {
                csv[0] = '\0';
                for (size_t i = 0; i < ectx->write_path_count; i++) {
                    if (i > 0) strcat(csv, ",");
                    strcat(csv, ectx->write_paths[i]);
                }
                setenv("CCLAW_WRITE_PATHS", csv, 1);
                free(csv);
            }
        }

        const char *self_exe = getenv("CCLAW_QJS_EXE");
        if (!self_exe || !self_exe[0]) self_exe = "/proc/self/exe";

        if (code) {
            execl(self_exe, "cclaw", "--qjs_eval", "-e", code, (char *)NULL);
        } else if (args_str) {
            execl(self_exe, "cclaw", "--qjs_eval", filename, args_str, (char *)NULL);
        } else {
            execl(self_exe, "cclaw", "--qjs_eval", filename, (char *)NULL);
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

    /* Strip trailing newline from child output */
    if (out_len > 0 && output[out_len - 1] == '\n') output[--out_len] = '\0';

    char *result = strdup(output);
    free(output);
    return result ? result : strdup("error: OOM");
}

int tool_js_eval_register(ToolRegistry *reg, JsEvalCtx *ctx) {
    return tools_register(reg, "js_eval",
                          "Run JavaScript in QuickJS (ES2025). "
                          "http_request(url[, opts]) is synchronous HTTP. "
                          "File globals: fs.readDir(path), fs.readFile(path), fs.writeFile(path, data), "
                          "fs.stat(path), fs.cwd(). Only allow-listed hosts work. "
                          "Returns the last expression value (or printed output). "
                          "When using 'filename', must be a .qjs file.",
                          JSEVAL_PARAMS_JSON, tool_js_eval_handler, ctx);
}

/* --- JS-defined tool support (extension-path) --- */

typedef struct {
    char *code;
    JsEvalCtx *ectx;
} JsToolData;

static char *js_defined_tool_handler(const char *arguments, void *user_data) {
    JsToolData *td = (JsToolData *)user_data;
    if (!td || !td->code) return strdup("error: no code for this tool");
    const char *args_str = (arguments && arguments[0]) ? arguments : "{}";

    size_t wrap_len = strlen(td->code) + strlen(args_str) + 32;
    char *wrapped = malloc(wrap_len);
    if (!wrapped) return strdup("error: OOM");
    snprintf(wrapped, wrap_len, "(function(args){%s})(%s)", td->code, args_str);

    size_t esc_cap = strlen(wrapped) * 2 + 8;
    char *esc = malloc(esc_cap);
    if (!esc) { free(wrapped); return strdup("error: OOM"); }
    size_t esc_len = json_escape(esc, esc_cap, wrapped, strlen(wrapped));
    free(wrapped);

    size_t blob_len = esc_len + 16;
    char *blob = malloc(blob_len);
    if (!blob) { free(esc); return strdup("error: OOM"); }
    snprintf(blob, blob_len, "{\"code\":\"%s\"}", esc);
    free(esc);

    char *result = tool_js_eval_handler(blob, td->ectx);
    free(blob);
    return result;
}

static void js_tool_data_free(void *user_data) {
    JsToolData *td = (JsToolData *)user_data;
    if (td) { free(td->code); free(td); }
}

int js_tool_register_ext(ToolRegistry *reg, const char *name,
                         const char *description, const char *parameters_json,
                         const char *code, JsEvalCtx *ectx,
                         const char *policy_json) {
    ToolEntry *existing = tools_lookup(reg, name);
    if (existing) {
        JsToolData *td = (JsToolData *)existing->user_data;
        free(td->code);
        td->code = strdup(code);
        td->ectx = ectx;
        free(existing->description);
        free(existing->parameters_json);
        free(existing->policy_json);
        existing->description = description ? strdup(description) : NULL;
        existing->parameters_json = parameters_json ? strdup(parameters_json) : NULL;
        existing->policy_json = policy_json ? strdup(policy_json) : NULL;
        existing->handler = js_defined_tool_handler;
        return 0;
    }
    JsToolData *td = malloc(sizeof(JsToolData));
    if (!td) return -1;
    td->code = strdup(code);
    td->ectx = ectx;
    if (!td->code) { free(td); return -1; }
    int rc = tools_register(reg, name, description, parameters_json,
                            js_defined_tool_handler, td);
    if (rc == 0) {
        ToolEntry *e = tools_lookup(reg, name);
        if (e) {
            e->free_fn = js_tool_data_free;
            e->policy_json = policy_json ? strdup(policy_json) : NULL;
        }
    }
    return rc;
}

/* --- Session runtime (used by extensions for hooks context) --- */

JsSessionRuntime *js_runtime_create(void) {
    JsSessionRuntime *rt = calloc(1, sizeof(JsSessionRuntime));
    if (!rt) return NULL;
    rt->heap = qjs_runtime_create(JS_HEAP_SIZE);
    if (!rt->heap) { free(rt); return NULL; }
    QjsRuntime *qrt = (QjsRuntime *)rt->heap;
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
