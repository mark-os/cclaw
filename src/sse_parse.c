#include "sse_parse.h"
#define JSMN_STATIC
#include "jsmn.h"
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

/* Helper: skip a token and all its children, return next index */
static int tok_skip(const jsmntok_t *tokens, int i, int ntok) {
    if (i >= ntok) return ntok;
    if (tokens[i].type == JSMN_OBJECT) {
        int pairs = tokens[i].size;
        int j = i + 1;
        for (int p = 0; p < pairs && j < ntok; p++) {
            j++; /* key */
            j = tok_skip(tokens, j, ntok); /* value */
        }
        return j;
    }
    if (tokens[i].type == JSMN_ARRAY) {
        int elems = tokens[i].size;
        int j = i + 1;
        for (int e = 0; e < elems && j < ntok; e++)
            j = tok_skip(tokens, j, ntok);
        return j;
    }
    return i + 1;
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
        j = tok_skip(tokens, j, ntok); /* skip value */
    }
    return -1;
}

/* ─── OpenAI ─────────────────────────────────────────────────────────── */

int sse_parse_openai(const char *json, size_t len, SseChunk *out) {
    memset(out, 0, sizeof(*out));
    out->tc_index = -1;

    jsmntok_t tokens[MAX_TOKENS];
    jsmn_parser parser;
    jsmn_init(&parser);
    int ntok = jsmn_parse(&parser, json, len, tokens, MAX_TOKENS);
    if (ntok < 1) return -1;

    /* Root object */
    int choices_i = obj_find(json, tokens, 0, ntok, "choices");
    if (choices_i >= 0 && tokens[choices_i].type == JSMN_ARRAY && tokens[choices_i].size > 0) {
        int c0 = choices_i + 1; /* first element of choices array */
        /* choices[0] is an object */
        int delta_i = obj_find(json, tokens, c0, ntok, "delta");
        if (delta_i >= 0 && tokens[delta_i].type == JSMN_OBJECT) {
            /* content */
            int ci = obj_find(json, tokens, delta_i, ntok, "content");
            if (ci >= 0 && tokens[ci].type == JSMN_STRING) {
                out->text = json + tokens[ci].start;
                out->text_len = (size_t)(tokens[ci].end - tokens[ci].start);
            }
            /* reasoning_content or reasoning */
            int ri = obj_find(json, tokens, delta_i, ntok, "reasoning_content");
            if (ri < 0 || tokens[ri].type != JSMN_STRING)
                ri = obj_find(json, tokens, delta_i, ntok, "reasoning");
            if (ri >= 0 && tokens[ri].type == JSMN_STRING) {
                out->reasoning = json + tokens[ri].start;
                out->reasoning_len = (size_t)(tokens[ri].end - tokens[ri].start);
            }
            /* tool_calls */
            int tci = obj_find(json, tokens, delta_i, ntok, "tool_calls");
            if (tci >= 0 && tokens[tci].type == JSMN_ARRAY && tokens[tci].size > 0) {
                int tc0 = tci + 1; /* first tool_call object */
                if (tokens[tc0].type == JSMN_OBJECT) {
                    int idx_i = obj_find(json, tokens, tc0, ntok, "index");
                    if (idx_i >= 0)
                        out->tc_index = tok_int(json, &tokens[idx_i]);
                    int id_i = obj_find(json, tokens, tc0, ntok, "id");
                    if (id_i >= 0 && tokens[id_i].type == JSMN_STRING) {
                        out->tc_id = json + tokens[id_i].start;
                        out->tc_id_len = (size_t)(tokens[id_i].end - tokens[id_i].start);
                    }
                    int fn_i = obj_find(json, tokens, tc0, ntok, "function");
                    if (fn_i >= 0 && tokens[fn_i].type == JSMN_OBJECT) {
                        int nm_i = obj_find(json, tokens, fn_i, ntok, "name");
                        if (nm_i >= 0 && tokens[nm_i].type == JSMN_STRING) {
                            out->tc_name = json + tokens[nm_i].start;
                            out->tc_name_len = (size_t)(tokens[nm_i].end - tokens[nm_i].start);
                        }
                        int arg_i = obj_find(json, tokens, fn_i, ntok, "arguments");
                        if (arg_i >= 0 && tokens[arg_i].type == JSMN_STRING) {
                            out->tc_args = json + tokens[arg_i].start;
                            out->tc_args_len = (size_t)(tokens[arg_i].end - tokens[arg_i].start);
                        }
                    }
                }
            }
        }
        /* finish_reason */
        int fr_i = obj_find(json, tokens, c0, ntok, "finish_reason");
        if (fr_i >= 0 && tokens[fr_i].type == JSMN_STRING) {
            out->finish = json + tokens[fr_i].start;
            out->finish_len = (size_t)(tokens[fr_i].end - tokens[fr_i].start);
        }
    }

    /* usage (top-level) */
    int usage_i = obj_find(json, tokens, 0, ntok, "usage");
    if (usage_i >= 0 && tokens[usage_i].type == JSMN_OBJECT) {
        int pt = obj_find(json, tokens, usage_i, ntok, "prompt_tokens");
        if (pt >= 0) out->prompt_tokens = tok_int(json, &tokens[pt]);
        int ct = obj_find(json, tokens, usage_i, ntok, "completion_tokens");
        if (ct >= 0) out->completion_tokens = tok_int(json, &tokens[ct]);
        int tt = obj_find(json, tokens, usage_i, ntok, "total_tokens");
        if (tt >= 0) out->total_tokens = tok_int(json, &tokens[tt]);
        /* cost */
        int cost_i = obj_find(json, tokens, usage_i, ntok, "cost");
        if (cost_i >= 0 && tokens[cost_i].type == JSMN_PRIMITIVE) {
            char tmp[64];
            size_t len = (size_t)(tokens[cost_i].end - tokens[cost_i].start);
            if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
            memcpy(tmp, json + tokens[cost_i].start, len);
            tmp[len] = '\0';
            double c = strtod(tmp, NULL);
            out->cost_nano = (int64_t)(c * 1e9);
        }
        /* cached tokens */
        int ptd_i = obj_find(json, tokens, usage_i, ntok, "prompt_tokens_details");
        if (ptd_i >= 0 && tokens[ptd_i].type == JSMN_OBJECT) {
            int crt = obj_find(json, tokens, ptd_i, ntok, "cached_tokens");
            if (crt >= 0) out->cache_read_tokens = tok_int(json, &tokens[crt]);
            int cwt = obj_find(json, tokens, ptd_i, ntok, "cache_write_tokens");
            if (cwt >= 0) out->cache_write_tokens = tok_int(json, &tokens[cwt]);
        }
        /* DeepSeek direct fields */
        int pch = obj_find(json, tokens, usage_i, ntok, "prompt_cache_hit_tokens");
        if (pch >= 0) out->cache_read_tokens = tok_int(json, &tokens[pch]);
    }

    return 0;
}

/* ─── Gemini ─────────────────────────────────────────────────────────── */

int sse_parse_gemini(const char *json, size_t len, SseChunk *out) {
    memset(out, 0, sizeof(*out));
    out->tc_index = -1;

    jsmntok_t tokens[MAX_TOKENS];
    jsmn_parser parser;
    jsmn_init(&parser);
    int ntok = jsmn_parse(&parser, json, len, tokens, MAX_TOKENS);
    if (ntok < 1) return -1;

    /* candidates[0].content.parts[] */
    int cand_i = obj_find(json, tokens, 0, ntok, "candidates");
    if (cand_i >= 0 && tokens[cand_i].type == JSMN_ARRAY && tokens[cand_i].size > 0) {
        int c0 = cand_i + 1;
        int content_i = obj_find(json, tokens, c0, ntok, "content");
        if (content_i >= 0 && tokens[content_i].type == JSMN_OBJECT) {
            int parts_i = obj_find(json, tokens, content_i, ntok, "parts");
            if (parts_i >= 0 && tokens[parts_i].type == JSMN_ARRAY) {
                int nparts = tokens[parts_i].size;
                int pi = parts_i + 1;
                for (int p = 0; p < nparts && pi < ntok; p++) {
                    if (tokens[pi].type == JSMN_OBJECT) {
                        /* Check for thought:true */
                        int thought_i = obj_find(json, tokens, pi, ntok, "thought");
                        int is_thought = (thought_i >= 0 && tokens[thought_i].type == JSMN_PRIMITIVE &&
                                          json[tokens[thought_i].start] == 't');
                        /* functionCall */
                        int fc_i = obj_find(json, tokens, pi, ntok, "functionCall");
                        if (fc_i >= 0 && tokens[fc_i].type == JSMN_OBJECT) {
                            int nm_i = obj_find(json, tokens, fc_i, ntok, "name");
                            if (nm_i >= 0 && tokens[nm_i].type == JSMN_STRING) {
                                out->tc_name = json + tokens[nm_i].start;
                                out->tc_name_len = (size_t)(tokens[nm_i].end - tokens[nm_i].start);
                            }
                            int id_i = obj_find(json, tokens, fc_i, ntok, "id");
                            if (id_i >= 0 && tokens[id_i].type == JSMN_STRING) {
                                out->tc_id = json + tokens[id_i].start;
                                out->tc_id_len = (size_t)(tokens[id_i].end - tokens[id_i].start);
                            }
                            int args_i = obj_find(json, tokens, fc_i, ntok, "args");
                            if (args_i >= 0) {
                                /* Raw JSON substring for args object */
                                out->tc_args = json + tokens[args_i].start;
                                out->tc_args_len = (size_t)(tokens[args_i].end - tokens[args_i].start);
                                out->tc_args_complete = 1;
                            }
                            out->tc_index = 0;
                        } else {
                            /* text part */
                            int text_i = obj_find(json, tokens, pi, ntok, "text");
                            if (text_i >= 0 && tokens[text_i].type == JSMN_STRING) {
                                if (is_thought) {
                                    out->reasoning = json + tokens[text_i].start;
                                    out->reasoning_len = (size_t)(tokens[text_i].end - tokens[text_i].start);
                                } else {
                                    out->text = json + tokens[text_i].start;
                                    out->text_len = (size_t)(tokens[text_i].end - tokens[text_i].start);
                                }
                            }
                        }
                    }
                    pi = tok_skip(tokens, pi, ntok);
                }
            }
        }
        /* finishReason */
        int fr_i = obj_find(json, tokens, c0, ntok, "finishReason");
        if (fr_i >= 0 && tokens[fr_i].type == JSMN_STRING) {
            out->finish = json + tokens[fr_i].start;
            out->finish_len = (size_t)(tokens[fr_i].end - tokens[fr_i].start);
        }
    }

    /* usageMetadata */
    int um_i = obj_find(json, tokens, 0, ntok, "usageMetadata");
    if (um_i >= 0 && tokens[um_i].type == JSMN_OBJECT) {
        int pt = obj_find(json, tokens, um_i, ntok, "promptTokenCount");
        if (pt >= 0) out->prompt_tokens = tok_int(json, &tokens[pt]);
        int ct = obj_find(json, tokens, um_i, ntok, "candidatesTokenCount");
        if (ct >= 0) out->completion_tokens = tok_int(json, &tokens[ct]);
        int tt = obj_find(json, tokens, um_i, ntok, "totalTokenCount");
        if (tt >= 0) out->total_tokens = tok_int(json, &tokens[tt]);
        int crt = obj_find(json, tokens, um_i, ntok, "cachedContentTokenCount");
        if (crt >= 0) out->cache_read_tokens = tok_int(json, &tokens[crt]);
    }

    return 0;
}

/* ─── Anthropic ──────────────────────────────────────────────────────── */

int sse_parse_anthropic(const char *event_type, const char *json, size_t len, SseChunk *out) {
    memset(out, 0, sizeof(*out));
    out->tc_index = -1;

    if (!event_type || !json) return -1;

    jsmntok_t tokens[MAX_TOKENS];
    jsmn_parser parser;
    jsmn_init(&parser);
    int ntok = jsmn_parse(&parser, json, len, tokens, MAX_TOKENS);
    if (ntok < 1) return -1;

    if (strcmp(event_type, "content_block_start") == 0) {
        /* index + content_block.type/id/name */
        int idx_i = obj_find(json, tokens, 0, ntok, "index");
        if (idx_i >= 0) out->tc_index = tok_int(json, &tokens[idx_i]);
        int cb_i = obj_find(json, tokens, 0, ntok, "content_block");
        if (cb_i >= 0 && tokens[cb_i].type == JSMN_OBJECT) {
            int type_i = obj_find(json, tokens, cb_i, ntok, "type");
            if (type_i >= 0 && tokens[type_i].type == JSMN_STRING) {
                size_t tlen = (size_t)(tokens[type_i].end - tokens[type_i].start);
                if (tlen == 8 && memcmp(json + tokens[type_i].start, "tool_use", 8) == 0) {
                    int id_i = obj_find(json, tokens, cb_i, ntok, "id");
                    if (id_i >= 0 && tokens[id_i].type == JSMN_STRING) {
                        out->tc_id = json + tokens[id_i].start;
                        out->tc_id_len = (size_t)(tokens[id_i].end - tokens[id_i].start);
                    }
                    int nm_i = obj_find(json, tokens, cb_i, ntok, "name");
                    if (nm_i >= 0 && tokens[nm_i].type == JSMN_STRING) {
                        out->tc_name = json + tokens[nm_i].start;
                        out->tc_name_len = (size_t)(tokens[nm_i].end - tokens[nm_i].start);
                    }
                }
            }
        }
    } else if (strcmp(event_type, "content_block_delta") == 0) {
        int idx_i = obj_find(json, tokens, 0, ntok, "index");
        if (idx_i >= 0) out->tc_index = tok_int(json, &tokens[idx_i]);
        int delta_i = obj_find(json, tokens, 0, ntok, "delta");
        if (delta_i >= 0 && tokens[delta_i].type == JSMN_OBJECT) {
            int type_i = obj_find(json, tokens, delta_i, ntok, "type");
            if (type_i >= 0 && tokens[type_i].type == JSMN_STRING) {
                size_t tlen = (size_t)(tokens[type_i].end - tokens[type_i].start);
                const char *tstr = json + tokens[type_i].start;
                if (tlen == 10 && memcmp(tstr, "text_delta", 10) == 0) {
                    int ti = obj_find(json, tokens, delta_i, ntok, "text");
                    if (ti >= 0 && tokens[ti].type == JSMN_STRING) {
                        out->text = json + tokens[ti].start;
                        out->text_len = (size_t)(tokens[ti].end - tokens[ti].start);
                    }
                } else if (tlen == 14 && memcmp(tstr, "thinking_delta", 14) == 0) {
                    int ti = obj_find(json, tokens, delta_i, ntok, "thinking");
                    if (ti >= 0 && tokens[ti].type == JSMN_STRING) {
                        out->reasoning = json + tokens[ti].start;
                        out->reasoning_len = (size_t)(tokens[ti].end - tokens[ti].start);
                    }
                } else if (tlen == 16 && memcmp(tstr, "input_json_delta", 16) == 0) {
                    int ti = obj_find(json, tokens, delta_i, ntok, "partial_json");
                    if (ti >= 0 && tokens[ti].type == JSMN_STRING) {
                        out->tc_args = json + tokens[ti].start;
                        out->tc_args_len = (size_t)(tokens[ti].end - tokens[ti].start);
                    }
                }
            }
        }
    } else if (strcmp(event_type, "message_delta") == 0) {
        int delta_i = obj_find(json, tokens, 0, ntok, "delta");
        if (delta_i >= 0 && tokens[delta_i].type == JSMN_OBJECT) {
            int sr_i = obj_find(json, tokens, delta_i, ntok, "stop_reason");
            if (sr_i >= 0 && tokens[sr_i].type == JSMN_STRING) {
                out->finish = json + tokens[sr_i].start;
                out->finish_len = (size_t)(tokens[sr_i].end - tokens[sr_i].start);
            }
        }
        int usage_i = obj_find(json, tokens, 0, ntok, "usage");
        if (usage_i >= 0 && tokens[usage_i].type == JSMN_OBJECT) {
            int ot = obj_find(json, tokens, usage_i, ntok, "output_tokens");
            if (ot >= 0) out->completion_tokens = tok_int(json, &tokens[ot]);
        }
    } else if (strcmp(event_type, "message_start") == 0) {
        int msg_i = obj_find(json, tokens, 0, ntok, "message");
        if (msg_i >= 0 && tokens[msg_i].type == JSMN_OBJECT) {
            int usage_i = obj_find(json, tokens, msg_i, ntok, "usage");
            if (usage_i >= 0 && tokens[usage_i].type == JSMN_OBJECT) {
                int it = obj_find(json, tokens, usage_i, ntok, "input_tokens");
                if (it >= 0) out->prompt_tokens = tok_int(json, &tokens[it]);
                int crt = obj_find(json, tokens, usage_i, ntok, "cache_read_input_tokens");
                if (crt >= 0) out->cache_read_tokens = tok_int(json, &tokens[crt]);
                int cwt = obj_find(json, tokens, usage_i, ntok, "cache_creation_input_tokens");
                if (cwt >= 0) out->cache_write_tokens = tok_int(json, &tokens[cwt]);
            }
        }
    }

    return 0;
}
