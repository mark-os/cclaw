#ifndef CCLAW_TOOL_SHELL_H
#define CCLAW_TOOL_SHELL_H

#include "tools.h"

#define TOOL_SHELL_DEFAULT_TIMEOUT 30

/* Register shell_exec tool into registry. default_timeout from config (0 = use 30s). */
int tool_shell_register(ToolRegistry *reg, int default_timeout);

/* Handler: parse JSON args {"command":"...", "timeout":N}, run command,
 * capture stdout+stderr, enforce timeout via process group SIGKILL.
 * Returns heap-allocated result. */
char *tool_shell_handler(const char *arguments, void *user_data);

#endif
