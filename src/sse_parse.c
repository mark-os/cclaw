#include "sse_parse.h"
#define JSMN_STATIC
#include "jsmn.h"
#include "jsmn_util.h"
#include <string.h>
#include <stdlib.h>

#define MAX_TOKENS 256

/* Helper: compare jsmn token string to key */
static int tok_eq(const char *json, const jsmntok_t *t, const char *key) {
    size_t klen = strlen(key);
    return t->type == JSMN_STRING && (size_t)(t->end - t->start) == klen &&
           memcmp(json + t->start, key, klen) == 0;
}

/* Helper: get int value from a primitive token */
static int tok_int(const char *json, const jsmntok_t *t) {
    if (t->type != JSMN_PRIMITIVE) return 0;
    char tmp[32];
    size_t len = (size_t)(t->end - t->start);
    if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, json + t->start, len);
    tmp[len] = '\0';
    return atoi(tmp);
}

/* Find key in object starting at tokens[obj_idx], return value token index or -1 */
static int obj_find(const char *json, const jsmntok_t *tokens, int obj_idx, int ntok, const char *key) {
    if (obj_idx >= ntok || tokens[obj_idx].type != JSMN_OBJECT) return -1;
    int pairs = tokens[obj_idx].size;
    int j = obj_idx + 1;
    for (int p = 0; p < pairs && j < ntok; p++) {
        if (tok_eq(json, &tokens[j], key))
            return j + 1; /* value is next token */
        j++; /* skip key */
        j = jsmn_skip(tokens, j, ntok); /* skip value */
    }
    return -1;
}

/* Parse JSON with stack-allocated tokens as fast path, heap fallback on overflow */
static int parse_with_fallback(const char *json, size_t len,
                               jsmntok_t *stack_toks, int stack_cap,
                               jsmntok_t **out) {
    jsmn_parser p;
    jsmn_init(&p);
    int n = jsmn_parse(&p, json, len, stack_toks, (unsigned)stack_cap);
    if (n != JSMN_ERROR_NOMEM) { *out = stack_toks; return n; }

    jsmn_init(&p);
    int need = jsmn_parse(&p, json, len, NULL, 0);
    if (need < 1) return need;
    jsmntok_t *heap = malloc((size_t)need * sizeof(*heap));
    if (!heap) return -1;
    jsmn_init(&p);
    n = jsmn_parse(&p, json, len, heap, (unsigned)need);
    *out = heap;
    return n;
}

/* ─── OpenAI ─────────────────────────────────────────────────────────── */

int sse_parse_openai(const char *json, size_t len, SseChunk *out) {
    memset(out, 0, sizeof(*out));
    out->tc_index = -1;

    jsmntok_t tokens[MAX_TOKENS];
    jsmntok_t *toks;
    jsmn_parser parser;
    jsmn_init(&parser);
    int ntok = parse_with_fallback(json, len, tokens, MAX_TOKENS, &toks);
    if (ntok < 1) return -1;

    /* Root object */
    int choices_i = obj_find(json, toks, 0, ntok, "choices");
    if (choices_i >= 0 && toks[choices_i].type == JSMN_ARRAY && toks[choices_i].size > 0) {
        int c0 = choices_i + 1; /* first element of choices array */
        /* choices[0] is an object */
        int delta_i = obj_find(json, toks, c0, ntok, "delta");
        if (delta_i >= 0 && toks[delta_i].type == JSMN_OBJECT) {
            /* content */
            int ci = obj_find(json, toks, delta_i, ntok, "content");
            if (ci >= 0 && toks[ci].type == JSMN_STRING) {
                out->text = json + toks[ci].start;
                out->text_len = (size_t)(toks[ci].end - toks[ci].start);
            }
            /* reasoning_content or reasoning */
            int ri = obj_find(json, toks, delta_i, ntok, "reasoning_content");
            if (ri < 0 || toks[ri].type != JSMN_STRING)
                ri = obj_find(json, toks, delta_i, ntok, "reasoning");
            if (ri >= 0 && toks[ri].type == JSMN_STRING) {
                out->reasoning = json + toks[ri].start;
                out->reasoning_len = (size_t)(toks[ri].end - toks[ri].start);
            }
            /* tool_calls */
            int tci = obj_find(json, toks, delta_i, ntok, "tool_calls");
            if (tci >= 0 && toks[tci].type == JSMN_ARRAY && toks[tci].size > 0) {
                int tc0 = tci + 1; /* first tool_call object */
                if (toks[tc0].type == JSMN_OBJECT) {
                    int idx_i = obj_find(json, toks, tc0, ntok, "index");
                    if (idx_i >= 0)
                        out->tc_index = tok_int(json, &toks[idx_i]);
                    int id_i = obj_find(json, toks, tc0, ntok, "id");
                    if (id_i >= 0 && toks[id_i].type == JSMN_STRING) {
                        out->tc_id = json + toks[id_i].start;
                        out->tc_id_len = (size_t)(toks[id_i].end - toks[id_i].start);
                    }
                    int fn_i = obj_find(json, toks, tc0, ntok, "function");
                    if (fn_i >= 0 && toks[fn_i].type == JSMN_OBJECT) {
                        int nm_i = obj_find(json, toks, fn_i, ntok, "name");
                        if (nm_i >= 0 && toks[nm_i].type == JSMN_STRING) {
                            out->tc_name = json + toks[nm_i].start;
                            out->tc_name_len = (size_t)(toks[nm_i].end - toks[nm_i].start);
                        }
                        int arg_i = obj_find(json, toks, fn_i, ntok, "arguments");
                        if (arg_i >= 0 && toks[arg_i].type == JSMN_STRING) {
                            out->tc_args = json + toks[arg_i].start;
                            out->tc_args_len = (size_t)(toks[arg_i].end - toks[arg_i].start);
                        }
                    }
                }
            }
        }
        /* finish_reason */
        int fr_i = obj_find(json, toks, c0, ntok, "finish_reason");
        if (fr_i >= 0 && toks[fr_i].type == JSMN_STRING) {
            out->finish = json + toks[fr_i].start;
            out->finish_len = (size_t)(toks[fr_i].end - toks[fr_i].start);
        }
    }

    /* usage (top-level) */
    int usage_i = obj_find(json, toks, 0, ntok, "usage");
    if (usage_i >= 0 && toks[usage_i].type == JSMN_OBJECT) {
        int pt = obj_find(json, toks, usage_i, ntok, "prompt_tokens");
        if (pt >= 0) out->prompt_tokens = tok_int(json, &toks[pt]);
        int ct = obj_find(json, toks, usage_i, ntok, "completion_tokens");
        if (ct >= 0) out->completion_tokens = tok_int(json, &toks[ct]);
        int tt = obj_find(json, toks, usage_i, ntok, "total_tokens");
        if (tt >= 0) out->total_tokens = tok_int(json, &toks[tt]);
        /* cost */
        int cost_i = obj_find(json, toks, usage_i, ntok, "cost");
        if (cost_i >= 0 && toks[cost_i].type == JSMN_PRIMITIVE) {
            char tmp[64];
            size_t clen = (size_t)(toks[cost_i].end - toks[cost_i].start);
            if (clen >= sizeof(tmp)) clen = sizeof(tmp) - 1;
            memcpy(tmp, json + toks[cost_i].start, clen);
            tmp[clen] = '\0';
            double c = strtod(tmp, NULL);
            out->cost_nano = (int64_t)(c * 1e9);
        }
        /* cached tokens */
        int ptd_i = obj_find(json, toks, usage_i, ntok, "prompt_tokens_details");
        if (ptd_i >= 0 && toks[ptd_i].type == JSMN_OBJECT) {
            int crt = obj_find(json, toks, ptd_i, ntok, "cached_tokens");
            if (crt >= 0) out->cache_read_tokens = tok_int(json, &toks[crt]);
            int cwt = obj_find(json, toks, ptd_i, ntok, "cache_write_tokens");
            if (cwt >= 0) out->cache_write_tokens = tok_int(json, &toks[cwt]);
        }
        /* DeepSeek direct fields */
        int pch = obj_find(json, toks, usage_i, ntok, "prompt_cache_hit_tokens");
        if (pch >= 0) out->cache_read_tokens = tok_int(json, &toks[pch]);
    }

    if (toks != tokens) free(toks);
    return 0;
}

/* ─── Gemini ─────────────────────────────────────────────────────────── */

int sse_parse_gemini(const char *json, size_t len, SseChunk *out) {
    memset(out, 0, sizeof(*out));
    out->tc_index = -1;

    jsmntok_t tokens[MAX_TOKENS];
    jsmntok_t *toks;
    jsmn_parser parser;
    jsmn_init(&parser);
    int ntok = parse_with_fallback(json, len, tokens, MAX_TOKENS, &toks);
    if (ntok < 1) return -1;

    /* candidates[0].content.parts[] */
    int cand_i = obj_find(json, toks, 0, ntok, "candidates");
    if (cand_i >= 0 && toks[cand_i].type == JSMN_ARRAY && toks[cand_i].size > 0) {
        int c0 = cand_i + 1;
        int content_i = obj_find(json, toks, c0, ntok, "content");
        if (content_i >= 0 && toks[content_i].type == JSMN_OBJECT) {
            int parts_i = obj_find(json, toks, content_i, ntok, "parts");
            if (parts_i >= 0 && toks[parts_i].type == JSMN_ARRAY) {
                int nparts = toks[parts_i].size;
                int pi = parts_i + 1;
                for (int p = 0; p < nparts && pi < ntok; p++) {
                    if (toks[pi].type == JSMN_OBJECT) {
                        /* Check for thought:true */
                        int thought_i = obj_find(json, toks, pi, ntok, "thought");
                        int is_thought = (thought_i >= 0 && toks[thought_i].type == JSMN_PRIMITIVE &&
                                          json[toks[thought_i].start] == 't');
                        /* functionCall */
                        int fc_i = obj_find(json, toks, pi, ntok, "functionCall");
                        if (fc_i >= 0 && toks[fc_i].type == JSMN_OBJECT) {
                            int nm_i = obj_find(json, toks, fc_i, ntok, "name");
                            if (nm_i >= 0 && toks[nm_i].type == JSMN_STRING) {
                                out->tc_name = json + toks[nm_i].start;
                                out->tc_name_len = (size_t)(toks[nm_i].end - toks[nm_i].start);
                            }
                            int id_i = obj_find(json, toks, fc_i, ntok, "id");
                            if (id_i >= 0 && toks[id_i].type == JSMN_STRING) {
                                out->tc_id = json + toks[id_i].start;
                                out->tc_id_len = (size_t)(toks[id_i].end - toks[id_i].start);
                            }
                            int args_i = obj_find(json, toks, fc_i, ntok, "args");
                            if (args_i >= 0) {
                                /* Raw JSON substring for args object */
                                out->tc_args = json + toks[args_i].start;
                                out->tc_args_len = (size_t)(toks[args_i].end - toks[args_i].start);
                                out->tc_args_complete = 1;
                            }
                            out->tc_index = 0;
                        } else {
                            /* text part */
                            int text_i = obj_find(json, toks, pi, ntok, "text");
                            if (text_i >= 0 && toks[text_i].type == JSMN_STRING) {
                                if (is_thought) {
                                    out->reasoning = json + toks[text_i].start;
                                    out->reasoning_len = (size_t)(toks[text_i].end - toks[text_i].start);
                                } else {
                                    out->text = json + toks[text_i].start;
                                    out->text_len = (size_t)(toks[text_i].end - toks[text_i].start);
                                }
                            }
                        }
                    }
                    pi = jsmn_skip(toks, pi, ntok);
                }
            }
        }
        /* finishReason */
        int fr_i = obj_find(json, toks, c0, ntok, "finishReason");
        if (fr_i >= 0 && toks[fr_i].type == JSMN_STRING) {
            out->finish = json + toks[fr_i].start;
            out->finish_len = (size_t)(toks[fr_i].end - toks[fr_i].start);
        }
    }

    /* usageMetadata */
    int um_i = obj_find(json, toks, 0, ntok, "usageMetadata");
    if (um_i >= 0 && toks[um_i].type == JSMN_OBJECT) {
        int pt = obj_find(json, toks, um_i, ntok, "promptTokenCount");
        if (pt >= 0) out->prompt_tokens = tok_int(json, &toks[pt]);
        int ct = obj_find(json, toks, um_i, ntok, "candidatesTokenCount");
        if (ct >= 0) out->completion_tokens = tok_int(json, &toks[ct]);
        int tt = obj_find(json, toks, um_i, ntok, "totalTokenCount");
        if (tt >= 0) out->total_tokens = tok_int(json, &toks[tt]);
        int crt = obj_find(json, toks, um_i, ntok, "cachedContentTokenCount");
        if (crt >= 0) out->cache_read_tokens = tok_int(json, &toks[crt]);
    }

    if (toks != tokens) free(toks);
    return 0;
}

/* ─── Anthropic ──────────────────────────────────────────────────────── */

int sse_parse_anthropic(const char *event_type, const char *json, size_t len, SseChunk *out) {
    memset(out, 0, sizeof(*out));
    out->tc_index = -1;

    if (!event_type || !json) return -1;

    jsmntok_t tokens[MAX_TOKENS];
    jsmntok_t *toks;
    jsmn_parser parser;
    jsmn_init(&parser);
    int ntok = parse_with_fallback(json, len, tokens, MAX_TOKENS, &toks);
    if (ntok < 1) return -1;

    if (strcmp(event_type, "content_block_start") == 0) {
        /* index + content_block.type/id/name */
        int idx_i = obj_find(json, toks, 0, ntok, "index");
        if (idx_i >= 0) out->tc_index = tok_int(json, &toks[idx_i]);
        int cb_i = obj_find(json, toks, 0, ntok, "content_block");
        if (cb_i >= 0 && toks[cb_i].type == JSMN_OBJECT) {
            int type_i = obj_find(json, toks, cb_i, ntok, "type");
            if (type_i >= 0 && toks[type_i].type == JSMN_STRING) {
                size_t tlen = (size_t)(toks[type_i].end - toks[type_i].start);
                if (tlen == 8 && memcmp(json + toks[type_i].start, "tool_use", 8) == 0) {
                    int id_i = obj_find(json, toks, cb_i, ntok, "id");
                    if (id_i >= 0 && toks[id_i].type == JSMN_STRING) {
                        out->tc_id = json + toks[id_i].start;
                        out->tc_id_len = (size_t)(toks[id_i].end - toks[id_i].start);
                    }
                    int nm_i = obj_find(json, toks, cb_i, ntok, "name");
                    if (nm_i >= 0 && toks[nm_i].type == JSMN_STRING) {
                        out->tc_name = json + toks[nm_i].start;
                        out->tc_name_len = (size_t)(toks[nm_i].end - toks[nm_i].start);
                    }
                }
            }
        }
    } else if (strcmp(event_type, "content_block_delta") == 0) {
        int idx_i = obj_find(json, toks, 0, ntok, "index");
        if (idx_i >= 0) out->tc_index = tok_int(json, &toks[idx_i]);
        int delta_i = obj_find(json, toks, 0, ntok, "delta");
        if (delta_i >= 0 && toks[delta_i].type == JSMN_OBJECT) {
            int type_i = obj_find(json, toks, delta_i, ntok, "type");
            if (type_i >= 0 && toks[type_i].type == JSMN_STRING) {
                size_t tlen = (size_t)(toks[type_i].end - toks[type_i].start);
                const char *tstr = json + toks[type_i].start;
                if (tlen == 10 && memcmp(tstr, "text_delta", 10) == 0) {
                    int ti = obj_find(json, toks, delta_i, ntok, "text");
                    if (ti >= 0 && toks[ti].type == JSMN_STRING) {
                        out->text = json + toks[ti].start;
                        out->text_len = (size_t)(toks[ti].end - toks[ti].start);
                    }
                } else if (tlen == 14 && memcmp(tstr, "thinking_delta", 14) == 0) {
                    int ti = obj_find(json, toks, delta_i, ntok, "thinking");
                    if (ti >= 0 && toks[ti].type == JSMN_STRING) {
                        out->reasoning = json + toks[ti].start;
                        out->reasoning_len = (size_t)(toks[ti].end - toks[ti].start);
                    }
                } else if (tlen == 16 && memcmp(tstr, "input_json_delta", 16) == 0) {
                    int ti = obj_find(json, toks, delta_i, ntok, "partial_json");
                    if (ti >= 0 && toks[ti].type == JSMN_STRING) {
                        out->tc_args = json + toks[ti].start;
                        out->tc_args_len = (size_t)(toks[ti].end - toks[ti].start);
                    }
                }
            }
        }
    } else if (strcmp(event_type, "message_delta") == 0) {
        int delta_i = obj_find(json, toks, 0, ntok, "delta");
        if (delta_i >= 0 && toks[delta_i].type == JSMN_OBJECT) {
            int sr_i = obj_find(json, toks, delta_i, ntok, "stop_reason");
            if (sr_i >= 0 && toks[sr_i].type == JSMN_STRING) {
                out->finish = json + toks[sr_i].start;
                out->finish_len = (size_t)(toks[sr_i].end - toks[sr_i].start);
            }
        }
        int usage_i = obj_find(json, toks, 0, ntok, "usage");
        if (usage_i >= 0 && toks[usage_i].type == JSMN_OBJECT) {
            int ot = obj_find(json, toks, usage_i, ntok, "output_tokens");
            if (ot >= 0) out->completion_tokens = tok_int(json, &toks[ot]);
        }
    } else if (strcmp(event_type, "message_start") == 0) {
        int msg_i = obj_find(json, toks, 0, ntok, "message");
        if (msg_i >= 0 && toks[msg_i].type == JSMN_OBJECT) {
            int usage_i = obj_find(json, toks, msg_i, ntok, "usage");
            if (usage_i >= 0 && toks[usage_i].type == JSMN_OBJECT) {
                int it = obj_find(json, toks, usage_i, ntok, "input_tokens");
                if (it >= 0) out->prompt_tokens = tok_int(json, &toks[it]);
                int crt = obj_find(json, toks, usage_i, ntok, "cache_read_input_tokens");
                if (crt >= 0) out->cache_read_tokens = tok_int(json, &toks[crt]);
                int cwt = obj_find(json, toks, usage_i, ntok, "cache_creation_input_tokens");
                if (cwt >= 0) out->cache_write_tokens = tok_int(json, &toks[cwt]);
            }
        }
    }

    if (toks != tokens) free(toks);
    return 0;
}
