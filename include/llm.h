#ifndef CCLAW_LLM_H
#define CCLAW_LLM_H

#include "types.h"
#include "arena.h"

/* Tool schema for inclusion in LLM request */
typedef struct {
    const char *name;
    const char *description;
    const char *parameters_json;  /* raw JSON schema string */
} ToolSchema;

/* Build OpenAI-compatible chat completions request JSON.
 * V9: tools array omitted entirely when tool_count == 0.
 * Returns arena-allocated JSON string, or NULL on failure. */
char *llm_build_request(Arena *a, const Config *cfg, const Message *msgs,
                        size_t msg_count, const ToolSchema *tools,
                        size_t tool_count);

#endif
