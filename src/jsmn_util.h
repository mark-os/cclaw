#ifndef CCLAW_JSMN_UTIL_H
#define CCLAW_JSMN_UTIL_H

/* Inline jsmn token-walk helpers (skip-subtree, string compare). Used only
 * in db-less corners — never on a path that has a db handle (AGENTS.md).
 */

#include "jsmn.h"
#include <string.h>

/* Skip a token and all its children, return index of next token after i. */
static inline int jsmn_skip(const jsmntok_t *tokens, int i, int ntok) {
    if (i >= ntok) return ntok;
    if (tokens[i].type == JSMN_OBJECT) {
        int pairs = tokens[i].size;
        int j = i + 1;
        for (int p = 0; p < pairs && j < ntok; p++) {
            j++; /* key */
            j = jsmn_skip(tokens, j, ntok); /* value */
        }
        return j;
    }
    if (tokens[i].type == JSMN_ARRAY) {
        int elems = tokens[i].size;
        int j = i + 1;
        for (int e = 0; e < elems && j < ntok; e++)
            j = jsmn_skip(tokens, j, ntok);
        return j;
    }
    return i + 1;
}

/* True if token t is a JSON string equal to key. */
static inline int jtok_key_eq(const jsmntok_t *t, const char *json, const char *key) {
    size_t klen = strlen(key);
    return t->type == JSMN_STRING && (size_t)(t->end - t->start) == klen &&
           memcmp(json + t->start, key, klen) == 0;
}

/* Find key's value token index in the object at t[0]. Returns -1 if absent
 * or t[0] isn't an object. */
static inline int jtok_find(const jsmntok_t *t, int ntok, const char *json, const char *key) {
    if (t[0].type != JSMN_OBJECT) return -1;
    int j = 1;
    for (int k = 0; k < t[0].size; k++) {
        int vi = j + 1;
        if (jtok_key_eq(&t[j], json, key)) return vi;
        j = jsmn_skip(t, vi, ntok);
    }
    return -1;
}

#endif
