#ifndef CCLAW_JSMN_UTIL_H
#define CCLAW_JSMN_UTIL_H

#include "jsmn.h"

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

#endif
