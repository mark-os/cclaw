#define _POSIX_C_SOURCE 200809L
#include "tool_memory.h"
#include "db.h"
#include <cJSON.h>
#include <stdlib.h>
#include <string.h>

static const char *MEMORY_CREATE_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"label\":{\"type\":\"string\",\"description\":\"Unique label for this memory block\"},"
    "\"description\":{\"type\":\"string\",\"description\":\"What this block is for (visible in context)\"},"
    "\"value\":{\"type\":\"string\",\"description\":\"Initial value (optional)\"}"
    "},\"required\":[\"label\",\"description\"]}";

static const char *MEMORY_APPEND_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"label\":{\"type\":\"string\",\"description\":\"Label of the memory block to append to\"},"
    "\"content\":{\"type\":\"string\",\"description\":\"Text to append to the block value\"}"
    "},\"required\":[\"label\",\"content\"]}";

static const char *MEMORY_REPLACE_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"label\":{\"type\":\"string\",\"description\":\"Label of the memory block\"},"
    "\"old\":{\"type\":\"string\",\"description\":\"Text to find in the block value\"},"
    "\"new\":{\"type\":\"string\",\"description\":\"Replacement text\"}"
    "},\"required\":[\"label\",\"old\",\"new\"]}";

static char *tool_memory_create_handler(const char *arguments, void *user_data) {
    ToolMemoryCtx *ctx = (ToolMemoryCtx *)user_data;
    if (!ctx || !ctx->db || !ctx->agent_name)
        return strdup("error: memory tools unavailable (no agent context)");

    cJSON *json = cJSON_Parse(arguments);
    if (!json) return strdup("error: invalid JSON arguments");

    cJSON *label = cJSON_GetObjectItemCaseSensitive(json, "label");
    cJSON *desc = cJSON_GetObjectItemCaseSensitive(json, "description");
    if (!cJSON_IsString(label) || !cJSON_IsString(desc)) {
        cJSON_Delete(json);
        return strdup("error: 'label' and 'description' required");
    }

    cJSON *val = cJSON_GetObjectItemCaseSensitive(json, "value");
    const char *value = cJSON_IsString(val) ? val->valuestring : NULL;

    int64_t id = memory_block_create(ctx->db, ctx->agent_name, label->valuestring,
                                     desc->valuestring, value, 5000);
    cJSON_Delete(json);

    if (id < 0)
        return strdup("error: failed to create block (label may already exist)");

    return strdup("ok: memory block created");
}

static char *tool_memory_append_handler(const char *arguments, void *user_data) {
    ToolMemoryCtx *ctx = (ToolMemoryCtx *)user_data;
    if (!ctx || !ctx->db || !ctx->agent_name)
        return strdup("error: memory tools unavailable (no agent context)");

    cJSON *json = cJSON_Parse(arguments);
    if (!json) return strdup("error: invalid JSON arguments");

    cJSON *label = cJSON_GetObjectItemCaseSensitive(json, "label");
    cJSON *content = cJSON_GetObjectItemCaseSensitive(json, "content");
    if (!cJSON_IsString(label) || !cJSON_IsString(content)) {
        cJSON_Delete(json);
        return strdup("error: 'label' and 'content' required");
    }

    MemoryBlock *mb = memory_block_get(ctx->db, ctx->agent_name, label->valuestring);
    if (!mb) { cJSON_Delete(json); return strdup("error: block not found"); }
    if (mb->read_only) { memory_block_free(mb); cJSON_Delete(json); return strdup("error: block is read-only"); }

    /* Build new value = existing + content */
    size_t old_len = mb->value ? strlen(mb->value) : 0;
    size_t add_len = strlen(content->valuestring);
    size_t new_len = old_len + add_len;

    if ((int)new_len > mb->char_limit) {
        memory_block_free(mb);
        cJSON_Delete(json);
        return strdup("error: append would exceed char_limit");
    }

    char *new_val = malloc(new_len + 1);
    if (old_len > 0) memcpy(new_val, mb->value, old_len);
    memcpy(new_val + old_len, content->valuestring, add_len);
    new_val[new_len] = '\0';

    int rc = memory_block_set_value(ctx->db, ctx->agent_name, label->valuestring, new_val);
    free(new_val);
    memory_block_free(mb);
    cJSON_Delete(json);

    return strdup(rc == 0 ? "ok: content appended" : "error: failed to update block");
}

static char *tool_memory_replace_handler(const char *arguments, void *user_data) {
    ToolMemoryCtx *ctx = (ToolMemoryCtx *)user_data;
    if (!ctx || !ctx->db || !ctx->agent_name)
        return strdup("error: memory tools unavailable (no agent context)");

    cJSON *json = cJSON_Parse(arguments);
    if (!json) return strdup("error: invalid JSON arguments");

    cJSON *label = cJSON_GetObjectItemCaseSensitive(json, "label");
    cJSON *old_str = cJSON_GetObjectItemCaseSensitive(json, "old");
    cJSON *new_str = cJSON_GetObjectItemCaseSensitive(json, "new");
    if (!cJSON_IsString(label) || !cJSON_IsString(old_str) || !cJSON_IsString(new_str)) {
        cJSON_Delete(json);
        return strdup("error: 'label', 'old', and 'new' required");
    }

    MemoryBlock *mb = memory_block_get(ctx->db, ctx->agent_name, label->valuestring);
    if (!mb) { cJSON_Delete(json); return strdup("error: block not found"); }
    if (mb->read_only) { memory_block_free(mb); cJSON_Delete(json); return strdup("error: block is read-only"); }

    /* Find old text in value */
    const char *pos = mb->value ? strstr(mb->value, old_str->valuestring) : NULL;
    if (!pos) {
        memory_block_free(mb);
        cJSON_Delete(json);
        return strdup("error: 'old' text not found in block value");
    }

    size_t old_len = strlen(old_str->valuestring);
    size_t new_len = strlen(new_str->valuestring);
    size_t val_len = strlen(mb->value);
    size_t result_len = val_len - old_len + new_len;

    if ((int)result_len > mb->char_limit) {
        memory_block_free(mb);
        cJSON_Delete(json);
        return strdup("error: replacement would exceed char_limit");
    }

    char *new_val = malloc(result_len + 1);
    size_t prefix_len = (size_t)(pos - mb->value);
    memcpy(new_val, mb->value, prefix_len);
    memcpy(new_val + prefix_len, new_str->valuestring, new_len);
    memcpy(new_val + prefix_len + new_len, pos + old_len, val_len - prefix_len - old_len);
    new_val[result_len] = '\0';

    int rc = memory_block_set_value(ctx->db, ctx->agent_name, label->valuestring, new_val);
    free(new_val);
    memory_block_free(mb);
    cJSON_Delete(json);

    return strdup(rc == 0 ? "ok: text replaced" : "error: failed to update block");
}

int tool_memory_register(ToolRegistry *reg, ToolMemoryCtx *ctx) {
    if (tools_register(reg, "memory_create",
                       "Create a new memory block with a label and description. Persists across sessions.",
                       MEMORY_CREATE_PARAMS, tool_memory_create_handler, ctx) != 0)
        return -1;
    if (tools_register(reg, "memory_append",
                       "Append text to an existing memory block's value.",
                       MEMORY_APPEND_PARAMS, tool_memory_append_handler, ctx) != 0)
        return -1;
    if (tools_register(reg, "memory_replace",
                       "Find and replace text within a memory block's value.",
                       MEMORY_REPLACE_PARAMS, tool_memory_replace_handler, ctx) != 0)
        return -1;
    return 0;
}
