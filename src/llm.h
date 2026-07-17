#ifndef CCLAW_LLM_H
#define CCLAW_LLM_H

/* Shared LLM value types — the tool-schema struct handed to the request
 * builder and the sole provider finish_reason -> StopReason normalization.
 */

#include "types.h"
#include <sqlite3.h>

/* Tool schema for inclusion in LLM request */
typedef struct {
    const char *name;
    const char *description;
    const char *parameters_json;  /* raw JSON schema string */
} ToolSchema;

/* normalize provider finish_reason string → StopReason enum.
 * Sole normalization point. NULL input → STOP_REASON_STOP. */
StopReason map_stop_reason(const char *finish_reason);

#endif
