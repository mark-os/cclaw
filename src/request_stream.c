#define _POSIX_C_SOURCE 200809L
#include "request_stream.h"
#include "json_escape.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* V60/T168: Minimal tool_calls parser — zero-alloc state machine.
 * Walks stored JSON array [{"id":"...","name":"...","args":{...}},...]
 * extracting id, name, args substring offsets without full DOM build.
 * Emits OpenAI wire format directly into buffer.
 * Returns bytes written (may exceed cap if buffer too small). */

/* Skip a JSON string starting at src[pos] (pos points to opening quote).
 * Returns index after closing quote, or 0 on error. */
static size_t skip_json_string(const char *src, size_t pos) {
    pos++; /* skip opening " */
    while (src[pos]) {
        if (src[pos] == '\\') { pos += 2; continue; }
        if (src[pos] == '"') return pos + 1;
        pos++;
    }
    return 0;
}

/* Skip a JSON value starting at src[pos]. Handles strings, objects, arrays, primitives.
 * Returns index after the value, or 0 on error. */
static size_t skip_json_value(const char *src, size_t pos) {
    while (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\n' || src[pos] == '\r') pos++;
    if (src[pos] == '"') return skip_json_string(src, pos);
    if (src[pos] == '{' || src[pos] == '[') {
        char open = src[pos], close = (open == '{') ? '}' : ']';
        int depth = 1;
        pos++;
        while (src[pos] && depth > 0) {
            if (src[pos] == '"') {
                pos = skip_json_string(src, pos);
                if (pos == 0) return 0;
                continue;
            }
            if (src[pos] == open) depth++;
            else if (src[pos] == close) depth--;
            pos++;
        }
        return pos;
    }
    /* number, true, false, null */
    while (src[pos] && src[pos] != ',' && src[pos] != '}' && src[pos] != ']'
           && src[pos] != ' ' && src[pos] != '\t' && src[pos] != '\n' && src[pos] != '\r')
        pos++;
    return pos;
}

/* Match a key name at src[pos] (pos points to opening quote of key).
 * Returns 1 if key matches target, 0 otherwise. */
static int match_key(const char *src, size_t pos, const char *target, size_t tlen) {
    if (src[pos] != '"') return 0;
    return (memcmp(src + pos + 1, target, tlen) == 0 && src[pos + 1 + tlen] == '"');
}

static size_t emit_tool_calls_openai(char *dest, size_t cap, const char *tc_json) {
    if (!tc_json) return 0;

    size_t w = 0;
    #define DEST_AT(off) (dest ? dest + (off) : NULL)
    #define EMIT(s, l) do { \
        for (size_t _i = 0; _i < (l); _i++) { \
            if (w < cap) dest[w] = (s)[_i]; \
            w++; \
        } \
    } while(0)
    #define EMITS(s) EMIT(s, strlen(s))

    size_t p = 0;
    /* Skip whitespace, find opening [ */
    while (tc_json[p] && tc_json[p] != '[') p++;
    if (!tc_json[p]) return 0;
    p++; /* skip [ */

    EMITS(",\"tool_calls\":[");

    int item_idx = 0;
    while (tc_json[p]) {
        /* Skip whitespace */
        while (tc_json[p] == ' ' || tc_json[p] == '\t' || tc_json[p] == '\n' || tc_json[p] == '\r') p++;
        if (tc_json[p] == ']') break;
        if (tc_json[p] == ',') { p++; continue; }
        if (tc_json[p] != '{') break;
        p++; /* skip { */

        /* Parse object: find "id", "name", "args" */
        const char *id_start = NULL; size_t id_len = 0;
        const char *name_start = NULL; size_t name_len = 0;
        const char *args_start = NULL; size_t args_len = 0;

        while (tc_json[p] && tc_json[p] != '}') {
            while (tc_json[p] == ' ' || tc_json[p] == '\t' || tc_json[p] == '\n' || tc_json[p] == '\r' || tc_json[p] == ',') p++;
            if (tc_json[p] == '}') break;
            if (tc_json[p] != '"') break;

            /* Key */
            size_t key_pos = p;
            p = skip_json_string(tc_json, p);
            if (p == 0) goto done;

            /* Skip colon + whitespace */
            while (tc_json[p] == ' ' || tc_json[p] == ':' || tc_json[p] == '\t') p++;

            /* Value */
            size_t val_start = p;
            size_t val_end = skip_json_value(tc_json, p);
            if (val_end == 0) goto done;

            if (match_key(tc_json, key_pos, "id", 2)) {
                /* String value — extract content between quotes */
                if (tc_json[val_start] == '"') {
                    id_start = tc_json + val_start + 1;
                    id_len = val_end - val_start - 2;
                }
            } else if (match_key(tc_json, key_pos, "name", 4)) {
                if (tc_json[val_start] == '"') {
                    name_start = tc_json + val_start + 1;
                    name_len = val_end - val_start - 2;
                }
            } else if (match_key(tc_json, key_pos, "args", 4)) {
                args_start = tc_json + val_start;
                args_len = val_end - val_start;
            }
            p = val_end;
        }
        if (tc_json[p] == '}') p++; /* skip closing } */

        /* Emit OpenAI format */
        if (item_idx > 0) { EMIT(",", 1); }
        item_idx++;

        EMITS("{\"id\":\"");
        if (id_start) EMIT(id_start, id_len);
        EMITS("\",\"type\":\"function\",\"function\":{\"name\":\"");
        if (name_start) EMIT(name_start, name_len);
        EMITS("\",\"arguments\":\"");
        /* OpenAI: args is a JSON string (escaped JSON object) */
        if (args_start && args_len > 0) {
            size_t elen = json_escape_into_n(DEST_AT(w < cap ? w : 0),
                                             w < cap ? cap - w : 0,
                                             args_start, args_len);
            w += elen;
        } else {
            size_t elen = json_escape_into(DEST_AT(w < cap ? w : 0), w < cap ? cap - w : 0, "{}");
            w += elen;
        }
        EMITS("\"}}");
    }

done:
    EMITS("]");
    if (w < cap) dest[w] = '\0';

    #undef EMIT
    #undef EMITS
    #undef DEST_AT
    return w;
}

/* V60/T166: Emit a single entry as OpenAI wire JSON directly into buffer.
 * No cJSON on output path. Returns bytes written (may exceed cap).
 * T289: cache_control=1 → emit system msg content as array with cache_control block. */
static size_t emit_entry_openai(char *dest, size_t cap, int role,
                                const char *content, const char *tool_calls,
                                const char *tool_call_id, int cache_control) {
    size_t w = 0;
    #define DEST_AT(off) (dest ? dest + (off) : NULL)
    #define EMIT(s, l) do { \
        for (size_t _i = 0; _i < (l); _i++) { \
            if (w < cap) dest[w] = (s)[_i]; \
            w++; \
        } \
    } while(0)
    #define EMITS(s) EMIT(s, strlen(s))

    if (role == 3) {
        /* tool result → {"role":"tool","tool_call_id":"...","content":"..."} */
        EMITS("{\"role\":\"tool\",\"tool_call_id\":\"");
        size_t elen = json_escape_into(DEST_AT(w < cap ? w : 0), w < cap ? cap - w : 0,
                                       tool_call_id ? tool_call_id : "");
        w += elen;
        EMITS("\",\"content\":\"");
        elen = json_escape_into(DEST_AT(w < cap ? w : 0), w < cap ? cap - w : 0,
                                content ? content : "");
        w += elen;
        EMITS("\"}");
    } else if (role == 2) {
        /* assistant → {"role":"assistant","content":"..." or null, + optional tool_calls} */
        EMITS("{\"role\":\"assistant\"");
        if (content) {
            EMITS(",\"content\":\"");
            size_t elen = json_escape_into(DEST_AT(w < cap ? w : 0), w < cap ? cap - w : 0, content);
            w += elen;
            EMITS("\"");
        } else {
            EMITS(",\"content\":null");
        }
        if (tool_calls) {
            size_t tc_len = emit_tool_calls_openai(DEST_AT(w < cap ? w : 0),
                                                   w < cap ? cap - w : 0, tool_calls);
            w += tc_len;
        }
        EMITS("}");
    } else {
        /* user (1) / system (0) → {"role":"...","content":"..."} */
        /* T289: if cache_control && system → content as array with cache_control block */
        const char *role_str = (role == 0) ? "system" : "user";
        EMITS("{\"role\":\"");
        EMITS(role_str);
        if (cache_control && role == 0) {
            EMITS("\",\"content\":[{\"type\":\"text\",\"text\":\"");
            size_t elen = json_escape_into(DEST_AT(w < cap ? w : 0), w < cap ? cap - w : 0,
                                           content ? content : "");
            w += elen;
            EMITS("\",\"cache_control\":{\"type\":\"ephemeral\"}}]}");
        } else {
            EMITS("\",\"content\":\"");
            size_t elen = json_escape_into(DEST_AT(w < cap ? w : 0), w < cap ? cap - w : 0,
                                           content ? content : "");
            w += elen;
            EMITS("\"}");
        }
    }

    if (w < cap) dest[w] = '\0';
    #undef EMIT
    #undef EMITS
    #undef DEST_AT
    return w;
}

/* T290: forward declaration for Gemini tool_calls emitter */
static size_t emit_tool_calls_gemini(char *dest, size_t cap, const char *tc_json, int has_prior);

/* T123: detect if model needs explicit cache_control markers.
 * Default ON — providers that don't support it silently ignore the field.
 * T292: gemini-native disables annotations (uses cachedContents API only). */
static int needs_cache_control(const Config *cfg) {
    CacheHints h = cfg->provider.cache_hints;
    if (h == CACHE_HINTS_OFF) return 0;
    if (h == CACHE_HINTS_GEMINI_NATIVE) return 0;
    return 1;
}

/* T290: Emit a single entry as Gemini wire JSON (contents[] element).
 * Gemini format: {"role":"user|model","parts":[{"text":"..."}]}
 * Tool results are functionResponse parts in a user message.
 * Tool calls are functionCall parts in a model message.
 * System messages are NOT emitted here — they go in systemInstruction. */
/* Gemini wire format — streaming variant (manual emit).
 * DOM variant: gemini_cache.c:build_cache_request. Keep in sync. */
static size_t emit_entry_gemini(char *dest, size_t cap, int role,
                                const char *content, const char *tool_calls,
                                const char *tool_call_id) {
    size_t w = 0;
    #define DEST_AT_G(off) (dest ? dest + (off) : NULL)
    #define EMIT_G(s, l) do { \
        for (size_t _i = 0; _i < (l); _i++) { \
            if (w < cap && dest) dest[w] = (s)[_i]; \
            w++; \
        } \
    } while(0)
    #define EMITS_G(s) EMIT_G(s, strlen(s))

    if (role == 3) {
        /* tool result → {"role":"user","parts":[{"functionResponse":{"name":"<id>","response":{"content":"..."}}}]} */
        EMITS_G("{\"role\":\"user\",\"parts\":[{\"functionResponse\":{\"name\":\"");
        size_t elen = json_escape_into(DEST_AT_G(w < cap ? w : 0), w < cap ? cap - w : 0,
                                       tool_call_id ? tool_call_id : "");
        w += elen;
        EMITS_G("\",\"response\":{\"content\":\"");
        elen = json_escape_into(DEST_AT_G(w < cap ? w : 0), w < cap ? cap - w : 0,
                                content ? content : "");
        w += elen;
        EMITS_G("\"}}}]}");
    } else if (role == 2) {
        /* assistant → {"role":"model","parts":[...]} */
        EMITS_G("{\"role\":\"model\",\"parts\":[");
        int has_content = (content && content[0]);
        if (has_content) {
            EMITS_G("{\"text\":\"");
            size_t elen = json_escape_into(DEST_AT_G(w < cap ? w : 0), w < cap ? cap - w : 0, content);
            w += elen;
            EMITS_G("\"}");
        }
        /* Emit functionCall parts from tool_calls JSON */
        if (tool_calls) {
            size_t tc_len = emit_tool_calls_gemini(DEST_AT_G(w < cap ? w : 0),
                                                   w < cap ? cap - w : 0,
                                                   tool_calls, has_content);
            w += tc_len;
        }
        EMITS_G("]}");
    } else if (role == 1) {
        /* user → {"role":"user","parts":[{"text":"..."}]} */
        EMITS_G("{\"role\":\"user\",\"parts\":[{\"text\":\"");
        size_t elen = json_escape_into(DEST_AT_G(w < cap ? w : 0), w < cap ? cap - w : 0,
                                       content ? content : "");
        w += elen;
        EMITS_G("\"}]}");
    }
    /* role == 0 (system) skipped — handled separately as systemInstruction */

    if (w < cap && dest) dest[w] = '\0';
    #undef EMIT_G
    #undef EMITS_G
    #undef DEST_AT_G
    return w;
}

/* T290: emit tool_calls as Gemini functionCall parts.
 * Format: ,{"functionCall":{"name":"...","args":{...}}} for each tool call.
 * The comma prefix is conditional on has_prior (content text before). */
static size_t emit_tool_calls_gemini(char *dest, size_t cap, const char *tc_json, int has_prior) {
    if (!tc_json) return 0;
    size_t w = 0;
    #define EMIT_TG(s, l) do { \
        for (size_t _i = 0; _i < (l); _i++) { \
            if (w < cap && dest) dest[w] = (s)[_i]; \
            w++; \
        } \
    } while(0)
    #define EMITS_TG(s) EMIT_TG(s, strlen(s))

    size_t p = 0;
    while (tc_json[p] && tc_json[p] != '[') p++;
    if (!tc_json[p]) return 0;
    p++;

    int item_idx = 0;
    while (tc_json[p]) {
        while (tc_json[p] == ' ' || tc_json[p] == '\t' || tc_json[p] == '\n' || tc_json[p] == '\r') p++;
        if (tc_json[p] == ']') break;
        if (tc_json[p] == ',') { p++; continue; }
        if (tc_json[p] != '{') break;
        p++;

        const char *name_start = NULL; size_t name_len = 0;
        const char *args_start = NULL; size_t args_len = 0;

        while (tc_json[p] && tc_json[p] != '}') {
            while (tc_json[p] == ' ' || tc_json[p] == '\t' || tc_json[p] == '\n' || tc_json[p] == '\r' || tc_json[p] == ',') p++;
            if (tc_json[p] == '}') break;
            if (tc_json[p] != '"') break;

            size_t key_pos = p;
            p = skip_json_string(tc_json, p);
            if (p == 0) goto done_g;
            while (tc_json[p] == ' ' || tc_json[p] == ':' || tc_json[p] == '\t') p++;
            size_t val_start = p;
            size_t val_end = skip_json_value(tc_json, p);
            if (val_end == 0) goto done_g;

            if (match_key(tc_json, key_pos, "name", 4)) {
                if (tc_json[val_start] == '"') {
                    name_start = tc_json + val_start + 1;
                    name_len = val_end - val_start - 2;
                }
            } else if (match_key(tc_json, key_pos, "args", 4)) {
                args_start = tc_json + val_start;
                args_len = val_end - val_start;
            }
            p = val_end;
        }
        if (tc_json[p] == '}') p++;

        if (has_prior || item_idx > 0) { EMIT_TG(",", 1); }
        item_idx++;

        EMITS_TG("{\"functionCall\":{\"name\":\"");
        if (name_start) EMIT_TG(name_start, name_len);
        EMITS_TG("\",\"args\":");
        /* Emit args as raw object (not stringified) */
        if (args_start && args_len > 0)
            EMIT_TG(args_start, args_len);
        else
            EMITS_TG("{}");
        EMITS_TG("}}");
    }

done_g:
    #undef EMIT_TG
    #undef EMITS_TG
    return w;
}

/* Internal buffer management */
static int buf_ensure(RequestStreamer *rs, size_t need) {
    if (rs->buf_cap >= need) return 0;
    size_t cap = rs->buf_cap ? rs->buf_cap : 4096;
    while (cap < need) cap *= 2;
    char *tmp = realloc(rs->buf, cap);
    if (!tmp) return -1;
    rs->buf = tmp;
    rs->buf_cap = cap;
    return 0;
}

static void buf_set(RequestStreamer *rs, const char *data, size_t len) {
    if (buf_ensure(rs, len) != 0) return;
    memcpy(rs->buf, data, len);
    rs->buf_len = len;
    rs->buf_pos = 0;
}

/* Drain buffered data into dest. Returns bytes copied. */
static size_t buf_drain(RequestStreamer *rs, char *dest, size_t max) {
    size_t avail = rs->buf_len - rs->buf_pos;
    if (avail == 0) return 0;
    size_t n = avail < max ? avail : max;
    memcpy(dest, rs->buf + rs->buf_pos, n);
    rs->buf_pos += n;
    return n;
}

static int buf_empty(const RequestStreamer *rs) {
    return rs->buf_pos >= rs->buf_len;
}

/* Build tools JSON fragment: ,"tools":[...] or empty if no tools */
static char *build_tools_fragment(const ToolSchema *tools, size_t count) {
    if (!tools || count == 0) return NULL; /* V9 */

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "type", "function");
        cJSON *fn = cJSON_CreateObject();
        cJSON_AddStringToObject(fn, "name", tools[i].name);
        if (tools[i].description)
            cJSON_AddStringToObject(fn, "description", tools[i].description);
        if (tools[i].parameters_json) {
            cJSON *params = cJSON_Parse(tools[i].parameters_json);
            if (params)
                cJSON_AddItemToObject(fn, "parameters", params);
        }
        cJSON_AddItemToObject(tool, "function", fn);
        cJSON_AddItemToArray(arr, tool);
    }

    char *arr_str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!arr_str) return NULL;

    /* Return only ,"tools":ARR — caller handles ] and } */
    size_t alen = strlen(arr_str);
    size_t flen = 9 + alen; /* ,"tools": + arr */
    char *frag = malloc(flen + 1);
    if (!frag) { free(arr_str); return NULL; }
    snprintf(frag, flen + 1, ",\"tools\":%s", arr_str);
    free(arr_str);
    return frag;
}

/* T290: Build Gemini tools fragment: ,"tools":[{"functionDeclarations":[...]}] */
static char *build_tools_fragment_gemini(const ToolSchema *tools, size_t count) {
    if (!tools || count == 0) return NULL;

    cJSON *decls = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        cJSON *fn = cJSON_CreateObject();
        cJSON_AddStringToObject(fn, "name", tools[i].name);
        if (tools[i].description)
            cJSON_AddStringToObject(fn, "description", tools[i].description);
        if (tools[i].parameters_json) {
            cJSON *params = cJSON_Parse(tools[i].parameters_json);
            if (params)
                cJSON_AddItemToObject(fn, "parameters", params);
        }
        cJSON_AddItemToArray(decls, fn);
    }

    cJSON *tool_obj = cJSON_CreateObject();
    cJSON_AddItemToObject(tool_obj, "functionDeclarations", decls);
    cJSON *tools_arr = cJSON_CreateArray();
    cJSON_AddItemToArray(tools_arr, tool_obj);

    char *arr_str = cJSON_PrintUnformatted(tools_arr);
    cJSON_Delete(tools_arr);
    if (!arr_str) return NULL;

    size_t alen = strlen(arr_str);
    size_t flen = 9 + alen; /* ,"tools": + arr */
    char *frag = malloc(flen + 1);
    if (!frag) { free(arr_str); return NULL; }
    snprintf(frag, flen + 1, ",\"tools\":%s", arr_str);
    free(arr_str);
    return frag;
}

int rs_init(RequestStreamer *rs, sqlite3 *db, int64_t session_id,
            const Config *cfg, const ContextPlan *plan,
            const ToolSchema *tools, size_t tool_count) {
    memset(rs, 0, sizeof(*rs));
    rs->db = db;
    rs->session_id = session_id;
    rs->cfg = cfg;
    rs->plan = plan;
    rs->tools = tools;
    rs->tool_count = tool_count;
    rs->phase = RS_PHASE_PREAMBLE;
    rs->entry_idx = plan->cut; /* start from cut point */
    rs->first_entry = 1;
    rs->cursor = NULL;
    rs->gemini_mode = (cfg->provider.endpoint_type == ENDPOINT_GEMINI) ? 1 : 0;

    /* T289: find last system message index for per-block cache_control */
    rs->last_sys_idx = -1;
    if (!rs->gemini_mode && needs_cache_control(cfg)) {
        for (int i = plan->cut; i < plan->count; i++) {
            if (plan->entries[i].role == ROLE_SYSTEM)
                rs->last_sys_idx = i;
        }
    }
    return 0;
}

/* Prepare cursor for entry split-column lookup by ID */
static int prepare_cursor(RequestStreamer *rs) {
    if (rs->cursor) return 0;
    const char *sql = "SELECT role, content, tool_calls, tool_call_id FROM entries WHERE id=? AND session_id=?;";
    return sqlite3_prepare_v2(rs->db, sql, -1, &rs->cursor, NULL) == SQLITE_OK ? 0 : -1;
}

/* Advance state machine: fill buffer with next chunk. Returns 0 if data available, 1 if done. */
static int rs_advance(RequestStreamer *rs) {
    switch (rs->phase) {
    case RS_PHASE_PREAMBLE: {
        if (rs->gemini_mode) {
            /* T291: if cached content active, skip systemInstruction (it's in cache) */
            if (rs->gemini_cache_name && rs->gemini_cache_name[0]) {
                size_t cn_len = strlen(rs->gemini_cache_name);
                size_t need = 32 + cn_len * 2;
                if (buf_ensure(rs, need) != 0) { rs->phase = RS_PHASE_DONE; return 1; }
                size_t w = 0;
                int n = snprintf(rs->buf, rs->buf_cap, "{\"cachedContent\":\"");
                w = (size_t)n;
                size_t elen = json_escape_into(rs->buf + w, rs->buf_cap - w, rs->gemini_cache_name);
                w += elen;
                int n2 = snprintf(rs->buf + w, rs->buf_cap - w, "\",\"contents\":[");
                w += (size_t)n2;
                rs->buf_len = w;
                rs->buf_pos = 0;
                rs->phase = RS_PHASE_ENTRIES;
                return 0;
            }

            /* T290: Gemini preamble — collect system msgs into systemInstruction */
            /* First pass: gather system content from plan entries */
            if (prepare_cursor(rs) != 0) { rs->phase = RS_PHASE_DONE; return 1; }

            /* Estimate: accumulate system text */
            char *sys_text = NULL;
            size_t sys_len = 0, sys_cap = 0;
            for (int i = rs->plan->cut; i < rs->plan->count; i++) {
                if (rs->plan->entries[i].role != ROLE_SYSTEM) continue;
                sqlite3_reset(rs->cursor);
                sqlite3_bind_int64(rs->cursor, 1, rs->plan->entries[i].id);
                sqlite3_bind_int64(rs->cursor, 2, rs->session_id);
                if (sqlite3_step(rs->cursor) != SQLITE_ROW) continue;
                const char *c = (const char *)sqlite3_column_text(rs->cursor, 1);
                if (!c || !c[0]) continue;
                size_t clen = strlen(c);
                if (sys_len + clen + 2 > sys_cap) {
                    sys_cap = (sys_cap == 0) ? 4096 : sys_cap * 2;
                    while (sys_cap < sys_len + clen + 2) sys_cap *= 2;
                    sys_text = realloc(sys_text, sys_cap);
                }
                if (sys_len > 0) sys_text[sys_len++] = '\n';
                memcpy(sys_text + sys_len, c, clen);
                sys_len += clen;
                sys_text[sys_len] = '\0';
            }
            /* Also include recall_text in system instruction */
            if (rs->recall_text && rs->recall_text[0]) {
                size_t rlen = strlen(rs->recall_text);
                if (sys_len + rlen + 2 > sys_cap) {
                    sys_cap = sys_len + rlen + 256;
                    sys_text = realloc(sys_text, sys_cap);
                }
                if (sys_len > 0) sys_text[sys_len++] = '\n';
                memcpy(sys_text + sys_len, rs->recall_text, rlen);
                sys_len += rlen;
                sys_text[sys_len] = '\0';
            }

            /* Build: {"systemInstruction":{"parts":[{"text":"..."}]},"contents":[ */
            size_t need = 128 + (sys_len * 6); /* escaped text might be 6x */
            if (buf_ensure(rs, need) != 0) { free(sys_text); return 1; }

            size_t w = 0;
            if (sys_text && sys_len > 0) {
                int n = snprintf(rs->buf, rs->buf_cap, "{\"systemInstruction\":{\"parts\":[{\"text\":\"");
                w = (size_t)n;
                size_t elen = json_escape_into(rs->buf + w, rs->buf_cap - w, sys_text);
                w += elen;
                int n2 = snprintf(rs->buf + w, rs->buf_cap - w, "\"}]},\"contents\":[");
                w += (size_t)n2;
            } else {
                int n = snprintf(rs->buf, rs->buf_cap, "{\"contents\":[");
                w = (size_t)n;
            }
            free(sys_text);
            rs->buf_len = w;
            rs->buf_pos = 0;
            rs->phase = RS_PHASE_ENTRIES;
            return 0;
        }

        /* OpenAI preamble (existing) */
        /* Build: {"model":"...","messages":[ */
        /* Also prepend cutoff notice as system message if truncated */
        const char *model = rs->cfg->provider.model ? rs->cfg->provider.model : "unknown";
        int has_cutoff = rs->plan->cut > 0;

        /* Estimate size */
        size_t need = 64 + strlen(model) + 64;
        if (has_cutoff) need += 128;
        if (rs->recall_text) need += strlen(rs->recall_text) * 2 + 64;
        if (buf_ensure(rs, need) != 0) return 1;

        int written;
        if (has_cutoff) {
            written = snprintf(rs->buf, rs->buf_cap,
                "{\"model\":\"%s\",\"messages\":["
                "{\"role\":\"system\",\"content\":"
                "\"[Earlier messages truncated. Use search for full history.]\"},",
                model);
        } else {
            written = snprintf(rs->buf, rs->buf_cap,
                "{\"model\":\"%s\",\"messages\":[", model);
        }
        rs->buf_len = (size_t)written;
        rs->buf_pos = 0;

        /* T269: auto-recall — inject as system message after preamble */
        if (rs->recall_text && rs->recall_text[0]) {
            size_t rl = strlen(rs->recall_text);
            size_t extra = rl * 2 + 64;
            if (buf_ensure(rs, rs->buf_len + extra) != 0) return 1;
            int w2 = snprintf(rs->buf + rs->buf_len, rs->buf_cap - rs->buf_len,
                "{\"role\":\"system\",\"content\":\"");
            rs->buf_len += (size_t)w2;
            size_t esc = json_escape_into(rs->buf + rs->buf_len,
                                          rs->buf_cap - rs->buf_len,
                                          rs->recall_text);
            rs->buf_len += esc;
            int w3 = snprintf(rs->buf + rs->buf_len, rs->buf_cap - rs->buf_len, "\"},");
            rs->buf_len += (size_t)w3;
        }

        rs->phase = RS_PHASE_ENTRIES;
        return 0;
    }

    case RS_PHASE_ENTRIES: {
        if (rs->entry_idx >= rs->plan->count) {
            rs->phase = RS_PHASE_TOOLS;
            return rs_advance(rs);
        }

        if (prepare_cursor(rs) != 0) {
            rs->phase = RS_PHASE_DONE;
            return 1;
        }

        /* T290: skip system entries in Gemini mode (already in systemInstruction) */
        if (rs->gemini_mode && rs->plan->entries[rs->entry_idx].role == ROLE_SYSTEM) {
            rs->entry_idx++;
            return rs_advance(rs);
        }

        int64_t eid = rs->plan->entries[rs->entry_idx].id;
        sqlite3_reset(rs->cursor);
        sqlite3_bind_int64(rs->cursor, 1, eid);
        sqlite3_bind_int64(rs->cursor, 2, rs->session_id);

        if (sqlite3_step(rs->cursor) != SQLITE_ROW) {
            /* Entry missing — skip */
            rs->entry_idx++;
            return rs_advance(rs);
        }

        /* Read split columns: 0=role, 1=content, 2=tool_calls, 3=tool_call_id */
        int role = sqlite3_column_int(rs->cursor, 0);
        const char *content = (const char *)sqlite3_column_text(rs->cursor, 1);
        const char *tool_calls = (const char *)sqlite3_column_text(rs->cursor, 2);
        const char *tool_call_id = (const char *)sqlite3_column_text(rs->cursor, 3);

        if (rs->gemini_mode) {
            /* T290: Gemini entry emission */
            size_t need = emit_entry_gemini(NULL, 0, role, content, tool_calls, tool_call_id);
            need += 2;
            if (buf_ensure(rs, need) != 0) { rs->entry_idx++; return rs_advance(rs); }

            if (rs->first_entry) {
                size_t written = emit_entry_gemini(rs->buf, rs->buf_cap, role, content, tool_calls, tool_call_id);
                rs->buf_len = written;
                rs->first_entry = 0;
            } else {
                rs->buf[0] = ',';
                size_t written = emit_entry_gemini(rs->buf + 1, rs->buf_cap - 1, role, content, tool_calls, tool_call_id);
                rs->buf_len = written + 1;
            }
            rs->buf_pos = 0;
            rs->entry_idx++;
            return 0;
        }

        /* V60: emit directly via json_escape_into + snprintf — no cJSON on output */
        /* T289: mark last system msg for per-block cache_control */
        int cc = (rs->entry_idx == rs->last_sys_idx) ? 1 : 0;
        /* First pass: measure required size */
        size_t need = emit_entry_openai(NULL, 0, role, content, tool_calls, tool_call_id, cc);
        need += 2; /* leading comma + NUL */
        if (buf_ensure(rs, need) != 0) { rs->entry_idx++; return rs_advance(rs); }

        if (rs->first_entry) {
            size_t written = emit_entry_openai(rs->buf, rs->buf_cap, role, content, tool_calls, tool_call_id, cc);
            rs->buf_len = written;
            rs->first_entry = 0;
        } else {
            rs->buf[0] = ',';
            size_t written = emit_entry_openai(rs->buf + 1, rs->buf_cap - 1, role, content, tool_calls, tool_call_id, cc);
            rs->buf_len = written + 1;
        }
        rs->buf_pos = 0;
        rs->entry_idx++;
        return 0;
    }

    case RS_PHASE_TOOLS: {
        if (rs->gemini_mode) {
            /* T290: Gemini tools format: ],"tools":[{"functionDeclarations":[...]}] */
            char *frag = NULL;
            if (rs->tools && rs->tool_count > 0)
                frag = build_tools_fragment_gemini(rs->tools, rs->tool_count);
            if (frag) {
                size_t flen = strlen(frag);
                if (buf_ensure(rs, 1 + flen) != 0) { free(frag); rs->phase = RS_PHASE_DONE; return 1; }
                rs->buf[0] = ']';
                memcpy(rs->buf + 1, frag, flen);
                rs->buf_len = 1 + flen;
                rs->buf_pos = 0;
                free(frag);
            } else {
                buf_set(rs, "]", 1);
            }
            rs->phase = RS_PHASE_CLOSE;
            return 0;
        }

        /* Close messages array, optionally append tools fragment */
        char *frag = NULL;
        if (rs->tools && rs->tool_count > 0)
            frag = build_tools_fragment(rs->tools, rs->tool_count);

        if (frag) {
            /* ] + tools fragment */
            size_t flen = strlen(frag);
            if (buf_ensure(rs, 1 + flen) != 0) { free(frag); rs->phase = RS_PHASE_DONE; return 1; }
            rs->buf[0] = ']';
            memcpy(rs->buf + 1, frag, flen);
            rs->buf_len = 1 + flen;
            rs->buf_pos = 0;
            free(frag);
        } else {
            /* Just close messages array */
            buf_set(rs, "]", 1);
        }
        rs->phase = RS_PHASE_CLOSE;
        return 0;
    }

    case RS_PHASE_CLOSE: {
        /* Optional stream + max_tokens + close root object */
        char close_buf[128];
        int n = 0;
        if (rs->gemini_mode) {
            /* T290: Gemini uses generationConfig for max_tokens + thinking */
            if (rs->cfg->provider.max_tokens > 0)
                n = snprintf(close_buf, sizeof(close_buf),
                             ",\"generationConfig\":{\"maxOutputTokens\":%d,"
                             "\"thinkingConfig\":{\"includeThoughts\":true}}}",
                             rs->cfg->provider.max_tokens);
            else
                n = snprintf(close_buf, sizeof(close_buf),
                             ",\"generationConfig\":{\"thinkingConfig\":{\"includeThoughts\":true}}}");
        } else if (rs->cfg->stream && rs->cfg->provider.max_tokens > 0)
            n = snprintf(close_buf, sizeof(close_buf),
                         ",\"stream\":true,\"max_tokens\":%d}", rs->cfg->provider.max_tokens);
        else if (rs->cfg->stream)
            n = snprintf(close_buf, sizeof(close_buf), ",\"stream\":true}");
        else if (rs->cfg->provider.max_tokens > 0)
            n = snprintf(close_buf, sizeof(close_buf),
                         ",\"max_tokens\":%d}", rs->cfg->provider.max_tokens);
        else
            n = snprintf(close_buf, sizeof(close_buf), "}");
        buf_set(rs, close_buf, (size_t)n);
        rs->phase = RS_PHASE_DONE;
        return 0;
    }

    case RS_PHASE_DONE:
        return 1;
    }
    return 1;
}

size_t rs_read_cb(char *dest, size_t size, size_t nmemb, void *userdata) {
    RequestStreamer *rs = userdata;
    size_t max = size * nmemb;
    if (max == 0) return 0;

    size_t total = 0;
    while (total < max) {
        /* Drain current buffer */
        if (!buf_empty(rs)) {
            size_t n = buf_drain(rs, dest + total, max - total);
            total += n;
            continue;
        }
        /* Buffer empty — advance to next phase/entry */
        if (rs_advance(rs) != 0)
            break; /* done */
    }
    return total;
}

void rs_cleanup(RequestStreamer *rs) {
    if (!rs) return;
    if (rs->cursor) {
        sqlite3_finalize(rs->cursor);
        rs->cursor = NULL;
    }
    free(rs->buf);
    rs->buf = NULL;
    rs->buf_len = 0;
    rs->buf_pos = 0;
    rs->buf_cap = 0;
}

void rs_reset(RequestStreamer *rs) {
    if (!rs) return;
    if (rs->cursor) {
        sqlite3_finalize(rs->cursor);
        rs->cursor = NULL;
    }
    rs->phase = RS_PHASE_PREAMBLE;
    rs->entry_idx = rs->plan->cut;
    rs->first_entry = 1;
    rs->buf_len = 0;
    rs->buf_pos = 0;
}

size_t rs_content_length(RequestStreamer *rs) {
    (void)rs;
    return 0; /* unknown — use chunked transfer */
}
