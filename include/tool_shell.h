#ifndef CCLAW_TOOL_SHELL_H
#define CCLAW_TOOL_SHELL_H

#include "tools.h"

#define TOOL_SHELL_DEFAULT_TIMEOUT 30

/* Config passed as user_data to shell handler */
typedef struct {
    int timeout;
    const char *workspace;  /* agent workspace (for reference; landlock enforces access) */
} ShellConfig;

/* Register shell_exec tool into registry.
 * workspace: agent workspace path (informational — landlock restricts fs).
 * Child inherits agent's landlock rules (V22): filesystem + network. */
int tool_shell_register(ToolRegistry *reg, int default_timeout, const char *workspace);

/* Handler: parse JSON args {"command":"...", "timeout":N}, run command,
 * capture stdout+stderr, enforce timeout via process group SIGKILL.
 * Child inherits landlock sandbox from agent process.
 * Returns heap-allocated result. */
char *tool_shell_handler(const char *arguments, void *user_data);

#endif
