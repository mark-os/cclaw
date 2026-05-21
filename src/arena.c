#include "arena.h"
#include <stdlib.h>
#include <stdint.h>

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

void arena_reset(Arena *a) {
    a->used = 0;
}

void arena_destroy(Arena *a) {
    if (a) {
        free(a->buf);
        free(a);
    }
}
