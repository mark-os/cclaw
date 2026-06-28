#include "llm.h"
#include <string.h>

/* V35: sole normalization point for provider finish_reason → StopReason */
StopReason map_stop_reason(const char *finish_reason) {
    if (!finish_reason)
        return STOP_REASON_STOP;

    if (strcmp(finish_reason, "stop") == 0 ||
        strcmp(finish_reason, "end") == 0 ||
        strcmp(finish_reason, "end_turn") == 0)
        return STOP_REASON_STOP;

    if (strcmp(finish_reason, "length") == 0 ||
        strcmp(finish_reason, "max_tokens") == 0)
        return STOP_REASON_LENGTH;

    if (strcmp(finish_reason, "tool_calls") == 0 ||
        strcmp(finish_reason, "function_call") == 0 ||
        strcmp(finish_reason, "tool_use") == 0)
        return STOP_REASON_TOOL_USE;

    if (strcmp(finish_reason, "content_filter") == 0 ||
        strcmp(finish_reason, "network_error") == 0)
        return STOP_REASON_ERROR;

    return STOP_REASON_ERROR;
}
