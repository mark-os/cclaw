#define _POSIX_C_SOURCE 200809L
#include "request_stream.h"
#include "context.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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

/* V40: truncate tool result content for LLM context */
static char *truncate_for_stream(const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    return truncate_result(src, len);
}

/* Reshape a single entry's DB JSON into OpenAI message JSON.
 * Returns heap-allocated string (caller frees). */
static char *reshape_entry(const char *data_json) {
    if (!data_json) return NULL;

    cJSON *obj = cJSON_Parse(data_json);
    if (!obj) return NULL;

    cJSON *role_j = cJSON_GetObjectItem(obj, "role");
    const char *role = role_j ? role_j->valuestring : "user";

    cJSON *out = cJSON_CreateObject();

    if (strcmp(role, "tool_result") == 0) {
        /* Tool result → {"role":"tool","tool_call_id":"...","content":"..."} */
        cJSON_AddStringToObject(out, "role", "tool");
        cJSON *tc_id = cJSON_GetObjectItem(obj, "tool_call_id");
        cJSON_AddStringToObject(out, "tool_call_id",
                                tc_id && tc_id->valuestring ? tc_id->valuestring : "");
        cJSON *content = cJSON_GetObjectItem(obj, "content");
        const char *raw = content && content->valuestring ? content->valuestring : "";
        /* V40: truncate */
        char *trunc = truncate_for_stream(raw);
        cJSON_AddStringToObject(out, "content", trunc ? trunc : raw);
        free(trunc);
    } else if (strcmp(role, "assistant") == 0) {
        cJSON_AddStringToObject(out, "role", "assistant");
        cJSON *content = cJSON_GetObjectItem(obj, "content");

        if (cJSON_IsArray(content)) {
            /* Parse content array: text blocks + tool_calls */
            const char *text = NULL;
            int n = cJSON_GetArraySize(content);
            int tc_count = 0;
            for (int i = 0; i < n; i++) {
                cJSON *item = cJSON_GetArrayItem(content, i);
                cJSON *type = cJSON_GetObjectItem(item, "type");
                if (type && type->valuestring) {
                    if (strcmp(type->valuestring, "tool_call") == 0) tc_count++;
                    else if (strcmp(type->valuestring, "text") == 0) {
                        cJSON *t = cJSON_GetObjectItem(item, "text");
                        if (t && t->valuestring) text = t->valuestring;
                    }
                }
            }

            if (text)
                cJSON_AddStringToObject(out, "content", text);
            else
                cJSON_AddNullToObject(out, "content");

            if (tc_count > 0) {
                cJSON *tc_arr = cJSON_AddArrayToObject(out, "tool_calls");
                for (int i = 0; i < n; i++) {
                    cJSON *item = cJSON_GetArrayItem(content, i);
                    cJSON *type = cJSON_GetObjectItem(item, "type");
                    if (!type || !type->valuestring ||
                        strcmp(type->valuestring, "tool_call") != 0) continue;

                    cJSON *tc = cJSON_CreateObject();
                    cJSON *id = cJSON_GetObjectItem(item, "id");
                    cJSON_AddStringToObject(tc, "id",
                                            id && id->valuestring ? id->valuestring : "");
                    cJSON_AddStringToObject(tc, "type", "function");
                    cJSON *fn = cJSON_CreateObject();
                    cJSON *name = cJSON_GetObjectItem(item, "name");
                    cJSON_AddStringToObject(fn, "name",
                                            name && name->valuestring ? name->valuestring : "");
                    cJSON *args = cJSON_GetObjectItem(item, "arguments");
                    if (args) {
                        char *args_str = cJSON_PrintUnformatted(args);
                        if (args_str) {
                            cJSON_AddRawToObject(fn, "arguments", args_str);
                            free(args_str);
                        }
                    } else {
                        cJSON_AddStringToObject(fn, "arguments", "{}");
                    }
                    cJSON_AddItemToObject(tc, "function", fn);
                    cJSON_AddItemToArray(tc_arr, tc);
                }
            }
        } else if (cJSON_IsString(content)) {
            cJSON_AddStringToObject(out, "content", content->valuestring);
        } else {
            cJSON_AddStringToObject(out, "content", "");
        }
    } else {
        /* user / system */
        cJSON_AddStringToObject(out, "role", role);
        cJSON *content = cJSON_GetObjectItem(obj, "content");
        cJSON_AddStringToObject(out, "content",
                                content && content->valuestring ? content->valuestring : "");
    }

    char *result = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    cJSON_Delete(obj);
    return result;
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

/* Prepare cursor for entry data lookup by ID */
static int prepare_cursor(RequestStreamer *rs) {
    if (rs->cursor) return 0;
    const char *sql = "SELECT data FROM entries WHERE id=? AND session_id=?;";
    return sqlite3_prepare_v2(rs->db, sql, -1, &rs->cursor, NULL) == SQLITE_OK ? 0 : -1;
}

/* Advance state machine: fill buffer with next chunk. Returns 0 if data available, 1 if done. */
static int rs_advance(RequestStreamer *rs) {
    switch (rs->phase) {
    case RS_PHASE_PREAMBLE: {
        /* Build: {"model":"...","messages":[ */
        /* Also prepend cutoff notice as system message if truncated */
        const char *model = rs->cfg->provider.model ? rs->cfg->provider.model : "unknown";
        int has_cutoff = rs->plan->cut > 0;

        /* Estimate size */
        size_t need = 32 + strlen(model) + 16;
        if (has_cutoff) need += 128;
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

        const char *data = (const char *)sqlite3_column_text(rs->cursor, 0);
        char *msg_json = reshape_entry(data);
        if (!msg_json) {
            rs->entry_idx++;
            return rs_advance(rs);
        }

        size_t mlen = strlen(msg_json);
        size_t need = mlen + 2; /* comma + json */
        if (buf_ensure(rs, need) != 0) { free(msg_json); return 1; }

        if (rs->first_entry) {
            memcpy(rs->buf, msg_json, mlen);
            rs->buf_len = mlen;
            rs->first_entry = 0;
        } else {
            rs->buf[0] = ',';
            memcpy(rs->buf + 1, msg_json, mlen);
            rs->buf_len = mlen + 1;
        }
        rs->buf_pos = 0;
        free(msg_json);
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
