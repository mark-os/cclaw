#define _POSIX_C_SOURCE 200809L
#include "tool_js.h"
#include <cJSON.h>
#include <mquickjs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* V5: 1MB heap cap, 10M instruction limit */
#define JS_HEAP_SIZE (1024 * 1024)
#define JS_MAX_INSTRUCTIONS 10000000

extern const JSSTDLibraryDef js_std_library;

typedef struct {
    int count;
} InterruptState;

static int interrupt_handler(JSContext *ctx, void *opaque) {
    (void)ctx;
    InterruptState *state = (InterruptState *)opaque;
    state->count++;
    return state->count > JS_MAX_INSTRUCTIONS;
}

static const char *JSEVAL_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"code\":{\"type\":\"string\",\"description\":\"JavaScript code to execute\"}"
    "},\"required\":[\"code\"]}";

char *tool_js_eval_handler(const char *arguments, void *user_data) {
    (void)user_data;

    cJSON *json = cJSON_Parse(arguments);
    if (!json) return strdup("error: invalid JSON arguments");

    cJSON *code_item = cJSON_GetObjectItemCaseSensitive(json, "code");
    if (!cJSON_IsString(code_item) || !code_item->valuestring[0]) {
        cJSON_Delete(json);
        return strdup("error: missing or empty 'code' field");
    }
    const char *code = code_item->valuestring;
    size_t code_len = strlen(code);

    void *heap = malloc(JS_HEAP_SIZE);
    if (!heap) { cJSON_Delete(json); return strdup("error: out of memory"); }

    JSContext *ctx = JS_NewContext(heap, JS_HEAP_SIZE, &js_std_library);
    if (!ctx) { free(heap); cJSON_Delete(json); return strdup("error: JS context creation failed"); }

    InterruptState istate = {0};
    JS_SetInterruptHandler(ctx, interrupt_handler);
    JS_SetContextOpaque(ctx, &istate);

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
    } else if (istate.count > JS_MAX_INSTRUCTIONS) {
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
    cJSON_Delete(json);
    return result;
}

int tool_js_eval_register(ToolRegistry *reg) {
    return tools_register(reg, "js_eval",
                          "Execute JavaScript code in a sandboxed environment and return the result",
                          JSEVAL_PARAMS_JSON, tool_js_eval_handler, NULL);
}
