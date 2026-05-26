#define _POSIX_C_SOURCE 200809L
#include "request_stream.h"
#include "json_escape.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* V60/T168: Minimal tool_calls parser — extract id, name, args substrings from stored JSON
 * without full DOM build. Walks the array looking for key offsets.
 * Emits OpenAI wire format directly into buffer.
 * Returns bytes written (may exceed cap if buffer too small). */
static size_t emit_tool_calls_openai(char *dest, size_t cap, const char *tc_json) {
    /* For correctness and simplicity in this first pass, we use cJSON to parse
     * the stored tool_calls array but emit directly via snprintf+json_escape_into
     * instead of cJSON_Print. This avoids cJSON on the OUTPUT side (no PrintUnformatted
     * for the full message). T168 will replace this with a zero-alloc state machine. */
    if (!tc_json) return 0;

    /* Use cJSON just to walk the stored array structure */
    cJSON *arr = cJSON_Parse(tc_json);
    if (!arr || !cJSON_IsArray(arr)) { cJSON_Delete(arr); return 0; }

    size_t w = 0;
    #define EMIT(s, l) do { \
        for (size_t _i = 0; _i < (l); _i++) { \
            if (w < cap) dest[w] = (s)[_i]; \
            w++; \
        } \
    } while(0)
    #define EMITS(s) EMIT(s, strlen(s))

    EMITS(",\"tool_calls\":[");

    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; i++) {
        if (i > 0) { EMIT(",", 1); }
        cJSON *item = cJSON_GetArrayItem(arr, i);
        cJSON *id_j = cJSON_GetObjectItem(item, "id");
        cJSON *name_j = cJSON_GetObjectItem(item, "name");
        cJSON *args_j = cJSON_GetObjectItem(item, "args");

        const char *id = (id_j && id_j->valuestring) ? id_j->valuestring : "";
        const char *name = (name_j && name_j->valuestring) ? name_j->valuestring : "";

        /* Get args as raw JSON string */
        char *args_raw = NULL;
        if (args_j) {
            args_raw = cJSON_PrintUnformatted(args_j);
        }
        const char *args_str = args_raw ? args_raw : "{}";

        /* Emit: {"id":"...","type":"function","function":{"name":"...","arguments":"..."}} */
        EMITS("{\"id\":\"");
        size_t elen = json_escape_into(dest + (w < cap ? w : 0), w < cap ? cap - w : 0, id);
        w += elen;
        EMITS("\",\"type\":\"function\",\"function\":{\"name\":\"");
        elen = json_escape_into(dest + (w < cap ? w : 0), w < cap ? cap - w : 0, name);
        w += elen;
        EMITS("\",\"arguments\":\"");
        /* OpenAI: arguments is a JSON string (escaped JSON object) */
        elen = json_escape_into(dest + (w < cap ? w : 0), w < cap ? cap - w : 0, args_str);
        w += elen;
        EMITS("\"}}");

        free(args_raw);
    }

    EMITS("]");
    if (w < cap) dest[w] = '\0';

    #undef EMIT
    #undef EMITS
    cJSON_Delete(arr);
    return w;
}

/* V60/T166: Emit a single entry as OpenAI wire JSON directly into buffer.
 * No cJSON on output path. Returns bytes written (may exceed cap). */
static size_t emit_entry_openai(char *dest, size_t cap, int role,
                                const char *content, const char *tool_calls,
                                const char *tool_call_id) {
    size_t w = 0;
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
        size_t elen = json_escape_into(dest + (w < cap ? w : 0), w < cap ? cap - w : 0,
                                       tool_call_id ? tool_call_id : "");
        w += elen;
        EMITS("\",\"content\":\"");
        elen = json_escape_into(dest + (w < cap ? w : 0), w < cap ? cap - w : 0,
                                content ? content : "");
        w += elen;
        EMITS("\"}");
    } else if (role == 2) {
        /* assistant → {"role":"assistant","content":"..." or null, + optional tool_calls} */
        EMITS("{\"role\":\"assistant\"");
        if (content) {
            EMITS(",\"content\":\"");
            size_t elen = json_escape_into(dest + (w < cap ? w : 0), w < cap ? cap - w : 0, content);
            w += elen;
            EMITS("\"");
        } else {
            EMITS(",\"content\":null");
        }
        if (tool_calls) {
            size_t tc_len = emit_tool_calls_openai(dest + (w < cap ? w : 0),
                                                   w < cap ? cap - w : 0, tool_calls);
            w += tc_len;
        }
        EMITS("}");
    } else {
        /* user (1) / system (0) → {"role":"...","content":"..."} */
        const char *role_str = (role == 0) ? "system" : "user";
        EMITS("{\"role\":\"");
        EMITS(role_str);
        EMITS("\",\"content\":\"");
        size_t elen = json_escape_into(dest + (w < cap ? w : 0), w < cap ? cap - w : 0,
                                       content ? content : "");
        w += elen;
        EMITS("\"}");
    }

    if (w < cap) dest[w] = '\0';
    #undef EMIT
    #undef EMITS
    return w;
}

/* T123: detect if model needs explicit cache_control markers */
static int needs_cache_control(const Config *cfg) {
    CacheHints h = cfg->provider.cache_hints;
    if (h == CACHE_HINTS_OFF) return 0;
    if (h == CACHE_HINTS_ON) return 1;
    /* AUTO: detect from model name */
    const char *m = cfg->provider.model;
    if (!m) return 0;
    return (strstr(m, "anthropic") || strstr(m, "claude"));
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

    /* Wrap: ],"tools":ARR} */
    size_t alen = strlen(arr_str);
    size_t flen = 10 + alen + 1; /* ],"tools": + arr + } */
    char *frag = malloc(flen + 1);
    if (!frag) { free(arr_str); return NULL; }
    snprintf(frag, flen + 1, "],\"tools\":%s}", arr_str);
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
        /* Build: {"model":"...","messages":[ */
        /* T123: optionally add "cache_control":{"type":"ephemeral"} for Anthropic */
        /* Also prepend cutoff notice as system message if truncated */
        const char *model = rs->cfg->provider.model ? rs->cfg->provider.model : "unknown";
        int has_cutoff = rs->plan->cut > 0;
        int cache = needs_cache_control(rs->cfg);

        /* Estimate size */
        size_t need = 64 + strlen(model) + 64;
        if (has_cutoff) need += 128;
        if (buf_ensure(rs, need) != 0) return 1;

        int written;
        if (cache && has_cutoff) {
            written = snprintf(rs->buf, rs->buf_cap,
                "{\"model\":\"%s\",\"cache_control\":{\"type\":\"ephemeral\"},\"messages\":["
                "{\"role\":\"system\",\"content\":"
                "\"[Earlier messages truncated. Use search for full history.]\"},",
                model);
        } else if (cache) {
            written = snprintf(rs->buf, rs->buf_cap,
                "{\"model\":\"%s\",\"cache_control\":{\"type\":\"ephemeral\"},\"messages\":[",
                model);
        } else if (has_cutoff) {
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

        /* V60: emit directly via json_escape_into + snprintf — no cJSON on output */
        /* First pass: measure required size */
        size_t need = emit_entry_openai(NULL, 0, role, content, tool_calls, tool_call_id);
        need += 2; /* leading comma + NUL */
        if (buf_ensure(rs, need) != 0) { rs->entry_idx++; return rs_advance(rs); }

        if (rs->first_entry) {
            size_t written = emit_entry_openai(rs->buf, rs->buf_cap, role, content, tool_calls, tool_call_id);
            rs->buf_len = written;
            rs->first_entry = 0;
        } else {
            rs->buf[0] = ',';
            size_t written = emit_entry_openai(rs->buf + 1, rs->buf_cap - 1, role, content, tool_calls, tool_call_id);
            rs->buf_len = written + 1;
        }
        rs->buf_pos = 0;
        rs->entry_idx++;
        return 0;
    }

    case RS_PHASE_TOOLS: {
        /* V9: only include tools when count > 0 */
        if (rs->tools && rs->tool_count > 0) {
            char *frag = build_tools_fragment(rs->tools, rs->tool_count);
            if (frag) {
                buf_set(rs, frag, strlen(frag));
                free(frag);
                rs->phase = RS_PHASE_DONE;
                return 0;
            }
        }
        /* No tools — close: ]} or ]} with max_tokens */
        rs->phase = RS_PHASE_CLOSE;
        return rs_advance(rs);
    }

    case RS_PHASE_CLOSE: {
        /* Close messages array + optional max_tokens + close object */
        if (rs->cfg->provider.max_tokens > 0) {
            char close_buf[64];
            int n = snprintf(close_buf, sizeof(close_buf),
                             "],\"max_tokens\":%d}", rs->cfg->provider.max_tokens);
            buf_set(rs, close_buf, (size_t)n);
        } else {
            buf_set(rs, "]}", 2);
        }
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
