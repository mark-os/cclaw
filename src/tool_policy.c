#include "tool_policy.h"

#define JSMN_STATIC
#include "jsmn.h"
#include "jsmn_util.h"
#include <string.h>
#include <stddef.h>

#define MAX_TOKS 128

/* Token-level string equality (no alloc) */
static int tokeq(const char *json, const jsmntok_t *t, const char *s) {
    size_t slen = strlen(s);
    return t->type == JSMN_STRING &&
           (size_t)(t->end - t->start) == slen &&
           memcmp(json + t->start, s, slen) == 0;
}

/* Find a key in an object token at index obj_idx, return value token index or -1 */
static int obj_find(const jsmntok_t *t, int ntok, const char *json,
                    int obj_idx, const char *key) {
    if (obj_idx >= ntok || t[obj_idx].type != JSMN_OBJECT) return -1;
    int pairs = t[obj_idx].size;
    int i = obj_idx + 1;
    for (int p = 0; p < pairs && i + 1 < ntok; p++) {
        if (tokeq(json, &t[i], key)) return i + 1;
        i = jsmn_skip(t, i + 1, ntok);
    }
    return -1;
}

/* Check if arg_val (token in args_json) matches a match_val (token in policy_json).
 * match_val can be: string (exact), array (one-of), or literal "*" (any present). */
static int val_matches(const char *args_json, const jsmntok_t *arg_tok,
                       const char *pol_json, const jsmntok_t *pol_toks, int pol_ntok, int match_vi) {
    const jsmntok_t *mt = &pol_toks[match_vi];

    /* "*" wildcard: key exists in args = match */
    if (mt->type == JSMN_STRING && (mt->end - mt->start) == 1 &&
        pol_json[mt->start] == '*')
        return 1;

    /* String: exact equality with arg value */
    if (mt->type == JSMN_STRING && arg_tok->type == JSMN_STRING) {
        int mlen = mt->end - mt->start;
        int alen = arg_tok->end - arg_tok->start;
        return mlen == alen && memcmp(pol_json + mt->start, args_json + arg_tok->start, (size_t)mlen) == 0;
    }

    /* Array: arg value equals one of the elements */
    if (mt->type == JSMN_ARRAY && arg_tok->type == JSMN_STRING) {
        int elems = mt->size;
        int j = match_vi + 1;
        for (int e = 0; e < elems && j < pol_ntok; e++) {
            const jsmntok_t *el = &pol_toks[j];
            if (el->type == JSMN_STRING) {
                int elen = el->end - el->start;
                int alen = arg_tok->end - arg_tok->start;
                if (elen == alen && memcmp(pol_json + el->start, args_json + arg_tok->start, (size_t)elen) == 0)
                    return 1;
            }
            j = jsmn_skip(pol_toks, j, pol_ntok);
        }
        return 0;
    }

    return 0;
}

/* Check if a single rule's match object matches args */
static int rule_matches(const char *args_json, const jsmntok_t *args_toks, int args_ntok,
                        const char *pol_json, const jsmntok_t *pol_toks, int pol_ntok,
                        int match_idx) {
    if (match_idx < 0 || pol_toks[match_idx].type != JSMN_OBJECT) return 0;
    int pairs = pol_toks[match_idx].size;
    if (pairs == 0) return 1; /* empty match {} = catch-all */

    int i = match_idx + 1;
    for (int p = 0; p < pairs && i + 1 < pol_ntok; p++) {
        /* key in match object = the arg name to check */
        const jsmntok_t *key_tok = &pol_toks[i];
        int klen = key_tok->end - key_tok->start;
        int val_idx = i + 1;

        /* Find this key in args */
        int arg_vi = -1;
        int ai = 1;
        for (int ap = 0; ap < args_toks[0].size && ai + 1 < args_ntok; ap++) {
            const jsmntok_t *ak = &args_toks[ai];
            if (ak->type == JSMN_STRING && (ak->end - ak->start) == klen &&
                memcmp(args_json + ak->start, pol_json + key_tok->start, (size_t)klen) == 0) {
                arg_vi = ai + 1;
                break;
            }
            ai = jsmn_skip(args_toks, ai + 1, args_ntok);
        }

        /* Key not present in args -> no match for this rule */
        if (arg_vi < 0) return 0;

        if (!val_matches(args_json, &args_toks[arg_vi], pol_json, pol_toks, pol_ntok, val_idx))
            return 0;

        i = jsmn_skip(pol_toks, val_idx, pol_ntok);
    }
    return 1;
}

static PolicyEffect effect_from_tok(const char *json, const jsmntok_t *t) {
    if (tokeq(json, t, "deny"))  return POLICY_DENY;
    if (tokeq(json, t, "ask"))   return POLICY_ASK;
    return POLICY_ALLOW;
}

PolicyEffect policy_eval(const char *args_json, const char *policy_json) {
    if (!policy_json || !policy_json[0]) return POLICY_ALLOW;

    jsmntok_t pol_toks[MAX_TOKS];
    jsmn_parser pp;
    jsmn_init(&pp);
    int pol_ntok = jsmn_parse(&pp, policy_json, strlen(policy_json), pol_toks, MAX_TOKS);
    if (pol_ntok < 1 || pol_toks[0].type != JSMN_OBJECT) return POLICY_ALLOW;

    /* Find "rules" array */
    int rules_idx = obj_find(pol_toks, pol_ntok, policy_json, 0, "rules");
    if (rules_idx < 0 || pol_toks[rules_idx].type != JSMN_ARRAY) return POLICY_ALLOW;

    /* Parse args */
    jsmntok_t args_toks[MAX_TOKS];
    jsmn_parser ap;
    jsmn_init(&ap);
    int args_ntok = 0;
    if (args_json && args_json[0]) {
        args_ntok = jsmn_parse(&ap, args_json, strlen(args_json), args_toks, MAX_TOKS);
        /* Oversize args (> MAX_TOKS tokens) are not "empty" — treating them
         * as such would let a padded argument blob slip past a keyed deny
         * rule. Fail closed but recoverable: park for approval. */
        if (args_ntok == JSMN_ERROR_NOMEM)
            return POLICY_ASK;
        if (args_ntok < 1 || args_toks[0].type != JSMN_OBJECT)
            args_ntok = 0;
    }
    /* If args are empty/unparseable, only an empty match {} can hit */
    if (args_ntok == 0) {
        memset(args_toks, 0, sizeof(args_toks[0]));
        args_toks[0].type = JSMN_OBJECT;
        args_toks[0].size = 0;
        args_ntok = 1;
    }

    /* Iterate rules: first match wins */
    int n_rules = pol_toks[rules_idx].size;
    int ri = rules_idx + 1;
    for (int r = 0; r < n_rules && ri < pol_ntok; r++) {
        if (pol_toks[ri].type != JSMN_OBJECT) {
            ri = jsmn_skip(pol_toks, ri, pol_ntok);
            continue;
        }
        int match_idx = obj_find(pol_toks, pol_ntok, policy_json, ri, "match");
        int effect_idx = obj_find(pol_toks, pol_ntok, policy_json, ri, "effect");

        if (match_idx >= 0 && rule_matches(args_json ? args_json : "",
                                           args_toks, args_ntok,
                                           policy_json, pol_toks, pol_ntok, match_idx)) {
            if (effect_idx >= 0 && pol_toks[effect_idx].type == JSMN_STRING)
                return effect_from_tok(policy_json, &pol_toks[effect_idx]);
            return POLICY_ALLOW;
        }
        ri = jsmn_skip(pol_toks, ri, pol_ntok);
    }

    return POLICY_ALLOW;
}
