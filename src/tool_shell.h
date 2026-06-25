#ifndef CCLAW_TOOL_SHELL_H
#define CCLAW_TOOL_SHELL_H

#include "tools.h"

#define TOOL_SHELL_DEFAULT_TIMEOUT 30

/* V88: Secret name/value pair for shell injection + output masking */
typedef struct {
    char *name;   /* e.g. "GITHUB_TOKEN" (without CCLAW_SECRET_ prefix) */
    char *value;  /* plaintext secret value */
} ShellSecret;

/* Config passed as user_data to shell handler */
typedef struct {
    int timeout;
    const char *workspace;  /* agent workspace (namespace sandbox restricts access) */
    const char *cwd_path;   /* V22a/T276: CWD rw bind-mount in CLI mode (NULL in daemon) */
    const char *db_path;    /* cclaw.db path: its dir holds .cclaw_key — bind-masked inside the ns */
    const char *env_file;   /* provider env file: bind-masked inside the ns */
    char **allowed_hosts;   /* borrowed egress allowlist for the per-call proxy */
    size_t allowed_host_count;
    ShellSecret *secrets;   /* V88: array of secrets to inject + mask */
    size_t secret_count;
    int sandbox;            /* derived from trust_level: 1=namespace required, 0=none (host) */
    /* Trust-level policy bundle */
    int env_mode;           /* 0=inherit+scrub (trusted), 1=clean allowlist */
    int net_mode;           /* 0=proxy available, 1=no network */
    int mount_cwd;          /* 1=mount CWD rw, 0=skip */
    int workspace_ro;       /* 0=rw, 1=read-only */
    struct { int nproc; int as_mb; int cpu_sec; } rlimits; /* 0 = no limit */
    /* Layer 2: extra paths from grants (pointers into AgentCaps) */
    char **read_paths;
    size_t read_path_count;
    char **write_paths;
    size_t write_path_count;
} ShellConfig;

/* Register shell_exec tool into registry.
 * workspace: agent workspace path (namespace sandbox restricts fs). */
int tool_shell_register(ToolRegistry *reg, int default_timeout, const char *workspace);

/* Handler: parse JSON args {"command":"...", "timeout":N}, run command,
 * capture stdout+stderr, enforce timeout via process group SIGKILL.
 * Child runs in namespace sandbox (isolated fs + network).
 * Returns heap-allocated result. */
char *tool_shell_handler(const char *arguments, void *user_data);

/* V88: Collect CCLAW_SECRET_* env vars into array, clear from env.
 * Caller owns returned array (free with shell_secrets_free). */
ShellSecret *shell_secrets_collect(size_t *count);

/* Free secrets array */
void shell_secrets_free(ShellSecret *secrets, size_t count);

#endif
