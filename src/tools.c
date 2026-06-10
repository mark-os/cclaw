#define _POSIX_C_SOURCE 200809L
#include "tools.h"
#include <stdlib.h>
#include <string.h>

void tools_init(ToolRegistry *reg) {
    memset(reg, 0, sizeof(*reg));
}

int tools_register(ToolRegistry *reg, const char *name, const char *description,
                   const char *parameters_json, ToolHandlerFn handler, void *user_data) {
    if (!reg || !name || reg->count >= TOOLS_MAX) return -1;

    ToolEntry *e = &reg->entries[reg->count];
    e->name = strdup(name);
    e->description = description ? strdup(description) : NULL;
    e->parameters_json = parameters_json ? strdup(parameters_json) : NULL;
    e->handler = handler;
    e->user_data = user_data;
    reg->count++;
    return 0;
}

ToolEntry *tools_lookup(ToolRegistry *reg, const char *name) {
    if (!reg || !name) return NULL;
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->entries[i].name, name) == 0)
            return &reg->entries[i];
    }
    return NULL;
}

size_t tools_schemas(ToolRegistry *reg, ToolSchema *out, size_t out_cap) {
    if (!reg || !out) return 0;
    size_t n = reg->count < out_cap ? reg->count : out_cap;
    for (size_t i = 0; i < n; i++) {
        out[i].name = reg->entries[i].name;
        out[i].description = reg->entries[i].description;
        out[i].parameters_json = reg->entries[i].parameters_json;
    }
    return n;
}

void tools_free(ToolRegistry *reg) {
    if (!reg) return;
    for (size_t i = 0; i < reg->count; i++) {
        free(reg->entries[i].name);
        free(reg->entries[i].description);
        free(reg->entries[i].parameters_json);
        if (reg->entries[i].free_fn)
            reg->entries[i].free_fn(reg->entries[i].user_data);
    }
    reg->count = 0;
}

