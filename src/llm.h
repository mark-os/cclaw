#ifndef CCLAW_LLM_H
#define CCLAW_LLM_H

#include "types.h"
#include <sqlite3.h>

/* Tool schema for inclusion in LLM request */
typedef struct {
    const char *name;
    const char *description;
    const char *parameters_json;  /* raw JSON schema string */
} ToolSchema;

/* V35: normalize provider finish_reason string → StopReason enum.
 * Sole normalization point. NULL input → STOP_REASON_STOP. */
StopReason map_stop_reason(const char *finish_reason);

#endif
