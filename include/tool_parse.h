#ifndef CCLAW_TOOL_PARSE_H
#define CCLAW_TOOL_PARSE_H

/*
 * Minimal jsmn-based argument parser for built-in tool handlers.
 * Arguments are pre-validated JSON from SQLite — parsing won't fail
 * unless the LLM produced garbage that json_valid() let through.
 *
 * Usage:
 *   ToolArgs ta;
 *   if (tool_parse(arguments, &ta) != 0) return strdup("error: invalid JSON");
 *   const char *url = targ_str(&ta, "url");       // NUL-terminated, owned by ta
 *   int n = targ_int(&ta, "max_chars", 20000);    // with default
 *   // ... use values ...
 *   tool_parse_free(&ta);
 */

#define JSMN_STATIC
#include "jsmn.h"
#include "json_escape.h"
#include <stdlib.h>
#include <string.h>

#define TOOL_PARSE_MAX_TOKENS 64

typedef struct {
    const char *json;
    jsmntok_t tokens[TOOL_PARSE_MAX_TOKENS];
    int ntok;
    /* Small arena for NUL-terminated string copies */
    char *strs[16];
    int nstrs;
} ToolArgs;

/* Parse arguments JSON. Returns 0 on success, -1 on failure. */
static inline int tool_parse(const char *arguments, ToolArgs *ta) {
    memset(ta, 0, sizeof(*ta));
    if (!arguments) return -1;
    ta->json = arguments;
    jsmn_parser p;
    jsmn_init(&p);
    ta->ntok = jsmn_parse(&p, arguments, strlen(arguments),
                          ta->tokens, TOOL_PARSE_MAX_TOKENS);
    if (ta->ntok < 1 || ta->tokens[0].type != JSMN_OBJECT) return -1;
    return 0;
}

/* Free any string copies. */
static inline void tool_parse_free(ToolArgs *ta) {
    for (int i = 0; i < ta->nstrs; i++) free(ta->strs[i]);
}

/* Find a key in the top-level object. Returns token index of value, or -1. */
static inline int targ_find(const ToolArgs *ta, const char *key) {
    size_t klen = strlen(key);
    int pairs = ta->tokens[0].size;
    int i = 1;
    for (int p = 0; p < pairs && i + 1 < ta->ntok; p++) {
        jsmntok_t *kt = &((jsmntok_t *)ta->tokens)[i];
        if (kt->type == JSMN_STRING &&
            (size_t)(kt->end - kt->start) == klen &&
            memcmp(ta->json + kt->start, key, klen) == 0)
            return i + 1;
        /* Skip value */
        i += 2; /* key + primitive/string value */
        /* For object/array values, skip children */
        jsmntok_t *vt = &((jsmntok_t *)ta->tokens)[i - 1];
        if (vt->type == JSMN_OBJECT || vt->type == JSMN_ARRAY) {
            /* Count all nested tokens */
            int depth = 1, j = i;
            while (j < ta->ntok && depth > 0) {
                if (ta->tokens[j].type == JSMN_OBJECT || ta->tokens[j].type == JSMN_ARRAY)
                    depth += ta->tokens[j].size;
                depth--;
                j++;
            }
            i = j;
        }
    }
    return -1;
}

/* Get string value for key. Returns NUL-terminated, unescaped string (owned by ta), or NULL. */
static inline const char *targ_str(ToolArgs *ta, const char *key) {
    int vi = targ_find(ta, key);
    if (vi < 0) return NULL;
    jsmntok_t *t = &ta->tokens[vi];
    if (t->type != JSMN_STRING) return NULL;
    size_t len = (size_t)(t->end - t->start);
    if (ta->nstrs >= 16) return NULL;
    char *s = malloc(len + 1);
    if (!s) return NULL;
    size_t ulen = json_unescape(s, len + 1, ta->json + t->start, len);
    s[ulen] = '\0';
    ta->strs[ta->nstrs++] = s;
    return s;
}

/* Get int value for key, with default. */
static inline int targ_int(const ToolArgs *ta, const char *key, int def) {
    int vi = targ_find(ta, key);
    if (vi < 0) return def;
    jsmntok_t *t = &((jsmntok_t *)ta->tokens)[vi];
    if (t->type != JSMN_PRIMITIVE) return def;
    return atoi(ta->json + t->start);
}

/* Get bool value for key, with default. */
static inline int targ_bool(const ToolArgs *ta, const char *key, int def) {
    int vi = targ_find(ta, key);
    if (vi < 0) return def;
    jsmntok_t *t = &((jsmntok_t *)ta->tokens)[vi];
    if (t->type != JSMN_PRIMITIVE) return def;
    return ta->json[t->start] == 't';
}

/* Get raw JSON substring for a key (object/array). Not NUL-terminated.
 * Writes pointer + length. Returns 0 on success, -1 if not found. */
static inline int targ_raw(const ToolArgs *ta, const char *key,
                           const char **out, size_t *out_len) {
    int vi = targ_find(ta, key);
    if (vi < 0) return -1;
    jsmntok_t *t = &((jsmntok_t *)ta->tokens)[vi];
    *out = ta->json + t->start;
    *out_len = (size_t)(t->end - t->start);
    return 0;
}

#endif
