#ifndef CCLAW_SANDBOX_H
#define CCLAW_SANDBOX_H

/* Child sandbox setup, shared by shell_exec and the forked --mjs_eval mode.
 * Establishes the namespace sandbox, scrubs the environment, applies rlimits,
 * and installs the network proxy preload. All fields default to 0/NULL. */
typedef struct {
    const char *workspace;  /* agent workspace bind-mounted rw (NULL = none) */
    const char *cwd_path;   /* CWD rw bind-mount in CLI mode (NULL in daemon) */
    const char *db_path;    /* cclaw.db path: its dir holds .cclaw_key — bind-masked */
    const char *proxy_sock; /* path to .proxy.sock (NULL if proxy not started) */
    int sandbox;            /* 1 = namespace required, 0 = none (host trust level) */
    int workspace_ro;       /* 0 = rw, 1 = read-only remount */
    int mount_cwd;          /* 1 = mount CWD rw, 0 = skip */
    int net_mode;           /* 0 = proxy available, 1 = no network */
    int env_mode;           /* 0 = inherit + scrub secrets, 1 = clean allowlist */
    struct { int nproc, as_mb, cpu_sec; } rlimits; /* 0 = no limit */
} SandboxConfig;

/* Run in the forked child before exec. Returns 0 on success, -1 on a hard
 * failure (caller must _exit). Fail-closed: when cfg->sandbox is set, a
 * namespace or read-only-remount failure returns -1 rather than running
 * unconfined. The proxy preload failing is a soft warning (returns 0). */
int sandbox_child_setup(const SandboxConfig *cfg);

/* Single source of truth: trust_level string → sandbox policy fields.
 * Fills cfg->sandbox, env_mode, net_mode, mount_cwd, workspace_ro, rlimits.
 * Does NOT touch workspace/db_path/proxy_sock/cwd_path (caller sets those). */
void sandbox_policy_from_trust(const char *trust_level, SandboxConfig *cfg);

#endif
