#ifndef CCLAW_TOOL_SHELL_H
#define CCLAW_TOOL_SHELL_H

#include "tools.h"

#define TOOL_SHELL_DEFAULT_TIMEOUT 30

/* V37: config passed as user_data to shell handler */
typedef struct {
    int timeout;
    const char *workspace;  /* NULL = no sandbox mount */
    int shell_network;      /* if true, skip CLONE_NEWNET */
    char **allowed_hosts;   /* V49: for mjs fetch proxy (NULL = deny all) */
    size_t allowed_hosts_count;
} ShellConfig;

/* Register shell_exec tool into registry.
 * workspace: agent workspace path (NULL = no namespace sandbox mounts).
 * shell_network: per-agent flag to allow network in shell (V37).
 * allowed_hosts/count: V49 fetch proxy allowlist for mjs (NULL = deny all). */
int tool_shell_register(ToolRegistry *reg, int default_timeout,
                        const char *workspace, int shell_network,
                        char **allowed_hosts, size_t allowed_hosts_count);

/* Handler: parse JSON args {"command":"...", "timeout":N}, run command,
 * capture stdout+stderr, enforce timeout via process group SIGKILL.
 * V37: child unshares namespaces, remounts / ro, workspace rw.
 * Returns heap-allocated result. */
char *tool_shell_handler(const char *arguments, void *user_data);

#endif
