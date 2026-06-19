#ifndef CCLAW_SANDBOX_H
#define CCLAW_SANDBOX_H

#include <limits.h>
#include <stddef.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* A requested extra bind-mount (read_path/write_path grant). path is borrowed. */
typedef struct { const char *path; int ro; } SandboxMountReq;

/* A planned bind-mount: canonical path + ro flag, owned inline. */
typedef struct { char path[PATH_MAX]; int ro; } SandboxMount;

/* Child sandbox setup, shared by shell_exec and the forked --mjs_eval mode.
 * Establishes the namespace sandbox, scrubs the environment, applies rlimits,
 * and installs the network proxy preload. All fields default to 0/NULL. */
typedef struct {
    const char *workspace;  /* agent workspace bind-mounted rw (NULL = none) */
    const char *cwd_path;   /* CWD rw bind-mount in CLI mode (NULL in daemon) */
    const char *db_path;    /* cclaw.db path: its dir holds .cclaw_key — bind-masked */
    const char *env_file;   /* provider env file: bind-masked inside the ns */
    const char *proxy_sock; /* path to .proxy.sock (NULL if proxy not started) */
    int sandbox;            /* 1 = namespace required, 0 = none (host trust level) */
    int workspace_ro;       /* 0 = rw, 1 = read-only remount */
    int mount_cwd;          /* 1 = mount CWD rw, 0 = skip */
    int net_mode;           /* 0 = proxy available, 1 = no network */
    int env_mode;           /* 0 = inherit + scrub secrets, 1 = clean allowlist */
    struct { int nproc, as_mb, cpu_sec; } rlimits; /* 0 = no limit */
    /* Layer 2: extra bind-mounts from read_path/write_path grants */
    SandboxMountReq *extra_mounts;
    size_t extra_mount_count;
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

/* Resolve extra-mount requests into a bind plan: canonicalize each path (drop
 * those whose realpath() fails), dedup by canonical path (rw wins over ro — a
 * write grant implies read of the same subtree), and sort shallow→deep so a
 * more-specific child mount binds after its parent and shadows it. out must
 * hold at least n entries; returns the deduped count. Pure — unit-testable. */
size_t sandbox_plan_mounts(const SandboxMountReq *in, size_t n, SandboxMount *out);

#endif
