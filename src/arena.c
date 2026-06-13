#include "arena.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

Arena *arena_create(size_t capacity) {
    Arena *a = malloc(sizeof(Arena));
    if (!a) return NULL;
    a->buf = malloc(capacity);
    if (!a->buf) { free(a); return NULL; }
    a->cap = capacity;
    a->used = 0;
    return a;
}

void *arena_alloc(Arena *a, size_t n) {
    /* 8-byte alignment */
    size_t aligned = (n + 7) & ~(size_t)7;
    if (a->used + aligned > a->cap) return NULL;
    void *ptr = a->buf + a->used;
    a->used += aligned;
    return ptr;
}


char *arena_strdup(Arena *a, const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *dst = arena_alloc(a, len + 1);
    if (dst) memcpy(dst, s, len + 1);
    return dst;
}

char *arena_strndup(Arena *a, const char *s, size_t n) {
    if (!s) return NULL;
    size_t len = 0;
    while (len < n && s[len]) len++;
    char *dst = arena_alloc(a, len + 1);
    if (dst) { memcpy(dst, s, len); dst[len] = '\0'; }
    return dst;
}

void arena_destroy(Arena *a) {
    if (a) {
        free(a->buf);
        free(a);
    }
}
