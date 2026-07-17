#ifndef CCLAW_TOOL_SHELL_H
#define CCLAW_TOOL_SHELL_H

#include "types.h"      /* ShellSecret */
#include "tools.h"
#include "sandbox.h"
#include "run_tool.h"

#define TOOL_SHELL_DEFAULT_TIMEOUT 30

/* Config passed as user_data to shell handler */
typedef struct {
    int timeout;
    const char *shell_path;  /* interpreter for -c; NULL = /bin/sh (agent_setup.c resolves) */
    const char *workspace;  /* agent workspace (namespace sandbox restricts access) */
    const char *cwd_path;   /* CWD rw bind-mount in CLI mode (NULL in daemon) */
    const char *db_path;    /* cclaw.db path: its dir holds .cclaw_key — bind-masked inside the ns */
    char **allowed_hosts;   /* borrowed egress allowlist for the per-call proxy */
    size_t allowed_host_count;
    ShellSecret *secrets;   /* array of secrets to inject + mask */
    size_t secret_count;
    SandboxProfile sb;      /* trust-derived policy + grant paths */
} ShellConfig;

/* Register shell_exec tool into registry.
 * workspace: agent workspace path (namespace sandbox restricts fs).
 * shell_path: interpreter for -c (NULL = /bin/sh).
 * Execution is via --run-tool broker (EXEC_SANDBOX); no in-process handler. */
int tool_shell_register(ToolRegistry *reg, int default_timeout, const char *workspace,
                        const char *shell_path);

/* Collect CCLAW_SECRET_* env vars into array, clear from env.
 * Caller owns returned array (free with shell_secrets_free). */
ShellSecret *shell_secrets_collect(size_t *count);

/* Free secrets array */
void shell_secrets_free(ShellSecret *secrets, size_t count);

/* Least-privilege env injection (specs/security.md): keep only secrets whose
 * CCLAW_SECRET_<name> appears in cmd with word boundaries on BOTH sides.
 * Returns malloc'd array of BORROWED name/value pointers (caller frees array
 * only); NULL with *out_count=0 if none. */
RunToolSecret *shell_filter_secrets(const char *cmd, const ShellSecret *all,
                                    size_t n, size_t *out_count);

/* --run-tool tier leaf (inner_exec): inject the pre-filtered secrets into env,
 * scrub the plaintext copies, and exec /bin/sh -c on q->command. Called from
 * the broker's sandbox child after sandbox setup; never returns (_exit(127)
 * if exec fails). */
void tool_shell_tier_exec(RunToolParsed *q) __attribute__((noreturn));

#endif
