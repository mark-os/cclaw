#ifndef CCLAW_SANDBOX_H
#define CCLAW_SANDBOX_H

/* The namespace sandbox: builds the child's isolation (namespaces, rlimits,
 * env scrub, mounts) and resolves a sandbox_profile string into the policy
 * fields. Shared by every --run-tool tier. Fail-closed by design.
 */

#include <limits.h>
#include <stddef.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* A requested extra bind-mount (read_path/write_path grant). path is borrowed. */
typedef struct { const char *path; int ro; } SandboxMountReq;

/* Sandbox policy + grant-path bundle, shared by the file/shell/web/js tool
 * contexts. Filled once per agent in agent_setup (policy from sandbox_profile
 * via sandbox_profile_resolve; paths from the agent's grants) and embedded in
 * each tool ctx — one definition instead of four identical copies. The policy
 * half maps 1:1 to the matching SandboxConfig fields. Path pointers
 * borrow AgentCaps and are rebound on cap refresh. */
typedef struct {
    int sandbox;            /* 1 = namespace required, 0 = none (host sandbox_profile) */
    int env_mode;           /* 0 = inherit-present-env + scrub, 1 = clean allowlist */
    int net_mode;           /* 0 = proxy available, 1 = no network */
    int mount_cwd;          /* 1 = mount CWD rw, 0 = skip */
    struct { int nproc, as_mb, cpu_sec; } rlimits; /* 0 = no limit */
    char **read_paths;  size_t read_path_count;    /* extra bind-mounts from grants */
    char **write_paths; size_t write_path_count;
} SandboxProfile;

/* A planned bind-mount: canonical path + ro flag, owned inline. */
typedef struct { char path[PATH_MAX]; int ro; } SandboxMount;

/* Child sandbox setup, shared by all --run-tool sandbox tiers (file/shell/web/js).
 * Establishes the namespace sandbox, scrubs the environment, applies rlimits,
 * and installs the network proxy preload. All fields default to 0/NULL. */
typedef struct {
    const char *workspace;  /* agent workspace bind-mounted rw (NULL = none) */
    const char *cwd_path;   /* CWD rw bind-mount in CLI mode (NULL in daemon) */
    const char *db_path;    /* cclaw.db path: its dir holds .cclaw_key. Path only —
                               never opened; used to locate both for masking. */
    const char *proxy_sock; /* path to .proxy.sock (NULL if proxy not started) */
    const char *llm_sock;   /* parent's LLM bridge UDS (NULL = no LLM());
                             * bind-mounted + exported as CCLAW_LLM_SOCK */
    const char *tmp_dir;    /* host dir bind-mounted as /tmp — persistent per-agent
                             * scratch the parent created and owns. NULL = no bind;
                             * /tmp then stays a dir on the tiny root tmpfs, which
                             * is functional but too small to build in. */
    int sandbox;            /* 1 = namespace required, 0 = none (host sandbox_profile) */
    int mount_cwd;          /* 1 = mount CWD rw, 0 = skip */
    int net_mode;           /* 0 = proxy available, 1 = no network */
    int skip_pid_ns;        /* 1 = omit CLONE_NEWPID (no inner fork); file tier */
    int env_mode;           /* 0 = inherit-present-env + scrub secrets, 1 = clean
                             * allowlist. NB: under the --run-tool re-exec model the
                             * child starts from an empty environ (execve envp={NULL}),
                             * so mode 0 yields only PATH + proxy vars, not the daemon's
                             * environment — "inherit" means whatever the child was
                             * given, which is currently nothing sensitive. */
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

/* Single source of truth: sandbox_profile string → sandbox policy fields.
 * Fills cfg->sandbox, env_mode, net_mode, mount_cwd, rlimits. Does NOT touch workspace/db_path/proxy_sock/cwd_path/tmp_dir (the
 * caller sets those — they are paths, not profile policy). */
void sandbox_policy_from_profile(const char *sandbox_profile, SandboxConfig *cfg);

/* Fill the policy half of a SandboxProfile (sandbox/env_mode/net_mode/mount_cwd/
 * rlimits) from sandbox_profile. The caller sets the
 * grant-path fields separately (they come from AgentCaps, not the profile). */
void sandbox_profile_resolve(const char *sandbox_profile, SandboxProfile *p);

/* Resolve extra-mount requests into a bind plan: canonicalize each path (drop
 * those whose realpath() fails), dedup by canonical path (rw wins over ro — a
 * write grant implies read of the same subtree), and sort shallow→deep so a
 * more-specific child mount binds after its parent and shadows it. out must
 * hold at least n entries; returns the deduped count. Pure — unit-testable. */
size_t sandbox_plan_mounts(const SandboxMountReq *in, size_t n, SandboxMount *out);

#endif
