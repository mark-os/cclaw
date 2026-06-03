#ifndef CCLAW_TOOL_REQUEST_CONFIG_H
#define CCLAW_TOOL_REQUEST_CONFIG_H

#include "tools.h"

/* T274/V120: request_config tool for CLI mode.
 * Agent proposes tool grant or host grant; CLI prompts user inline. */
int tool_request_config_register(ToolRegistry *reg);

#endif
