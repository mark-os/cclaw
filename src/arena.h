#ifndef CCLAW_ARENA_H
#define CCLAW_ARENA_H

#include <stddef.h>

#define ARENA_DEFAULT_SIZE (512 * 1024)  /* V6: 512KB per-turn scratch */

typedef struct {
    char *buf;
    size_t cap;
    size_t used;
} Arena;

/* Create arena with given capacity. Returns NULL on failure. */
Arena *arena_create(size_t capacity);

/* Allocate n bytes (8-byte aligned). Returns NULL if exhausted. */
void *arena_alloc(Arena *a, size_t n);

/* Duplicate string into arena. Returns NULL if s is NULL or allocation fails. */
char *arena_strdup(Arena *a, const char *s);

/* Free arena and its buffer. */
void arena_destroy(Arena *a);

#endif
