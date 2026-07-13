#define _POSIX_C_SOURCE 200809L
#include "buf.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void buf_append(Buf *b, const char *s, size_t n) {
    if (b->oom) return;
    if (n > SIZE_MAX - b->len - 1) { b->oom = 1; return; }
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->len + n + 1) nc *= 2;
        char *tmp = realloc(b->data, nc);
        if (!tmp) { b->oom = 1; return; }
        b->data = tmp;
        b->cap = nc;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void buf_append_str(Buf *b, const char *s) {
    buf_append(b, s, strlen(s));
}

void buf_append_char(Buf *b, char c) {
    buf_append(b, &c, 1);
}

void buf_appendf(Buf *b, const char *fmt, ...) {
    if (b->oom) return;
    char stack_buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
    va_end(ap);
    if (n < 0) { b->oom = 1; return; }
    if ((size_t)n < sizeof(stack_buf)) {
        buf_append(b, stack_buf, (size_t)n);
        return;
    }
    char *heap_buf = malloc((size_t)n + 1);
    if (!heap_buf) { b->oom = 1; return; }
    va_start(ap, fmt);
    vsnprintf(heap_buf, (size_t)n + 1, fmt, ap);
    va_end(ap);
    buf_append(b, heap_buf, (size_t)n);
    free(heap_buf);
}

char *buf_take(Buf *b) {
    if (b->oom) {
        free(b->data);
        memset(b, 0, sizeof(*b));
        return NULL;
    }
    char *data = b->data;
    if (!data) data = strdup("");
    memset(b, 0, sizeof(*b));
    return data;
}

void buf_free(Buf *b) {
    free(b->data);
    memset(b, 0, sizeof(*b));
}

char *template_render(const char *tmpl, const TemplateVar *vars, size_t nvars) {
    if (!tmpl) return NULL;
    Buf b = {0};
    size_t tlen = strlen(tmpl);
    for (size_t i = 0; i < tlen; ) {
        if (tmpl[i] == '{') {
            size_t j;
            for (j = 0; j < nvars; j++) {
                size_t klen = strlen(vars[j].key);
                if (strncmp(tmpl + i, vars[j].key, klen) == 0) {
                    buf_append_str(&b, vars[j].val);
                    i += klen;
                    break;
                }
            }
            if (j < nvars) continue;
        }
        buf_append_char(&b, tmpl[i]);
        i++;
    }
    return buf_take(&b);
}
