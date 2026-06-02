/* mjs — standalone MicroQuickJS evaluator for CClaw.
 * V48: accepts -e 'code' or filename; no libcurl.
 * V49: fetch() binding uses inherited fd from MJS_FETCH_FD env var.
 * V51: composable with unix pipes; fetch fd is side-channel. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mquickjs.h"

#define MJS_HEAP_SIZE (1024 * 1024)
#define MJS_MAX_INSTRUCTIONS 10000000

extern const JSSTDLibraryDef js_std_library;

typedef struct {
    int instruction_count;
    int instruction_limit;
    int fetch_fd;
    void *tool_registry;  /* unused — struct padding for ABI compat */
    int call_depth;       /* unused */
} JsHostCtx;

static int interrupt_handler(JSContext *ctx, void *opaque)
{
    (void)ctx;
    JsHostCtx *hctx = (JsHostCtx *)opaque;
    hctx->instruction_count++;
    return hctx->instruction_count > hctx->instruction_limit;
}

static void usage(void)
{
    fprintf(stderr, "Usage: mjs [-e 'code'] [file.js]\n");
    exit(1);
}

static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "mjs: cannot open '%s': ", path);
        perror(NULL);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[n] = '\0';
    *out_len = n;
    return buf;
}

int main(int argc, char **argv)
{
    const char *code = NULL;
    const char *filename = NULL;
    size_t code_len = 0;
    char *file_buf = NULL;

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-e") == 0) {
            if (i + 1 >= argc) usage();
            code = argv[++i];
            code_len = strlen(code);
        } else if (argv[i][0] == '-') {
            usage();
        } else {
            filename = argv[i];
        }
    }

    if (!code && !filename) usage();

    if (filename && !code) {
        file_buf = read_file(filename, &code_len);
        if (!file_buf) return 1;
        code = file_buf;
    }

    /* V49: get fetch fd from env */
    int fetch_fd = -1;
    const char *fd_env = getenv("MJS_FETCH_FD");
    if (fd_env) fetch_fd = atoi(fd_env);

    /* Create JS context */
    void *heap = malloc(MJS_HEAP_SIZE);
    if (!heap) {
        fprintf(stderr, "mjs: out of memory\n");
        free(file_buf);
        return 1;
    }

    JSContext *ctx = JS_NewContext(heap, MJS_HEAP_SIZE, &js_std_library);
    if (!ctx) {
        fprintf(stderr, "mjs: failed to create JS context\n");
        free(heap);
        free(file_buf);
        return 1;
    }

    JsHostCtx hctx = {
        .instruction_count = 0,
        .instruction_limit = MJS_MAX_INSTRUCTIONS,
        .fetch_fd = fetch_fd
    };
    JS_SetInterruptHandler(ctx, interrupt_handler);
    JS_SetContextOpaque(ctx, &hctx);

    /* Evaluate */
    const char *src_name = filename ? filename : "<eval>";
    JSValue val = JS_Eval(ctx, code, code_len, src_name, JS_EVAL_RETVAL);

    int ret = 0;
    if (JS_IsException(val)) {
        JSValue exc = JS_GetException(ctx);
        JSCStringBuf buf;
        const char *msg = JS_ToCString(ctx, exc, &buf);
        fprintf(stderr, "mjs: %s\n", msg ? msg : "exception");
        ret = 1;
    } else if (!JS_IsUndefined(val) && !JS_IsNull(val)) {
        /* Print result to stdout */
        JSCStringBuf buf;
        const char *str = JS_ToCString(ctx, val, &buf);
        if (str) printf("%s\n", str);
    }

    JS_FreeContext(ctx);
    free(heap);
    free(file_buf);
    return ret;
}
