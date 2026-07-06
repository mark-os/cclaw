#ifndef CCLAW_RUN_TOOL_H
#define CCLAW_RUN_TOOL_H

#include "sandbox.h"

#include <stddef.h>
#include <stdint.h>

/* Max request blob size. Capped to guarantee the blocking write() on the
 * poll-loop thread completes instantly into the kernel socket buffer (which is
 * typically >=64KB). If a request exceeds this, the parent refuses to dispatch
 * — fail closed, loud error. A blob this large is a design smell (inline script
 * → should be a file). */
#define RUNTOOL_REQUEST_MAX 32768

/* The socketpair fd the request blob arrives on (and the result departs on)
 * in the re-exec'd child. */
#define RUNTOOL_FD_REQUEST 3

/* Wire format: a flat little-endian binary frame, NOT JSON — both ends are
 * the same binary, so there is no escaping and no key/value scan. A 4-byte LE
 * body-length prefix is followed by a tier byte and the fields in ONE fixed
 * superset order known to both sides (write-all-always: fields a tier doesn't
 * use are NULL/0/empty). The tier byte drives *dispatch* (which TierDescriptor),
 * not the wire layout. Primitives: u32 = 4-byte LE; str = u32 length + that many
 * raw bytes (length 0 = NULL/absent). See run_tool.c for the canonical order. */

/* Sandbox tier wire discriminator (first body byte). Mirrors SandboxTier. */
#define RUNTOOL_TIER_FILE  0
#define RUNTOOL_TIER_SHELL 1
#define RUNTOOL_TIER_WEB   2
#define RUNTOOL_TIER_JS    3

/* A name/value secret for env injection (pre-filtered/resolved in parent). */
typedef struct {
    const char *name;
    const char *value;
} RunToolSecret;

/* Superset request — one struct for every sandbox tier. Fields a tier does not
 * use are left zero/NULL. Secrets: pre-resolved by the daemon parent (the
 * broker/child never hold the master key); `command` already has {{SECRET}}
 * interpolated. Callers carrying secrets MUST explicit_bzero the blob after
 * write(fd3). */
typedef struct {
    int tier;                     /* RUNTOOL_TIER_* */
    const char *tool_name;
    const char *arguments;
    int env_mode, nproc, as_mb, cpu_sec;
    int sandbox, net_mode;
    /* file/all */
    const char *workspace, *cwd_path;
    int workspace_ro, mount_cwd;
    const char **read_paths;  size_t read_count;
    const char **write_paths; size_t write_count;
    /* network tiers (shell/web/js) */
    const char *agent_dir;
    const char **host_rules;  size_t host_count;
    const char **deny_rules;  size_t deny_count;
    /* shell only */
    const char *command;
    const char *shell_path;  /* interpreter to run `command` with; NULL = /bin/sh */
    int timeout;
    const RunToolSecret *secrets; size_t secret_count;
} RunToolReq;

/* Child-side parsed request (owned strings). Mirrors RunToolReq's wire order.
 * Strings/arrays are heap-owned by the --run-tool process for its (short)
 * lifetime; never freed before _exit. This is the one type the per-tool tier
 * leaves (tool_*_tier_run / tool_shell_tier_exec) consume. */
typedef struct {
    int   tier;
    char *tool_name;
    char *arguments;
    int   env_mode, nproc, as_mb, cpu_sec;
    int   sandbox, net_mode;
    char *workspace, *cwd_path;
    int   workspace_ro, mount_cwd;
    char **read_paths;  size_t read_count;
    char **write_paths; size_t write_count;
    char *agent_dir;
    char **host_rules;  size_t host_count;
    char **deny_rules;  size_t deny_count;
    char *command;
    char *shell_path;
    int   timeout;
    struct { char *name; char *value; } *secrets;
    size_t secret_count;
} RunToolParsed;

/* Zero req; set tier/tool_name/arguments plus all SandboxProfile-derived
 * fields and workspace/cwd_path. Callers set tier-specific fields after. */
void run_tool_req_init(RunToolReq *req, int tier, const char *tool_name,
                       const char *arguments, const SandboxProfile *sb,
                       const char *workspace, const char *cwd_path);

/* Serialize a request into a length-prefixed blob. Returns malloc'd buffer;
 * *out_len = total size. NULL on OOM or if the blob exceeds
 * RUNTOOL_REQUEST_MAX. */
char *run_tool_serialize_request(const RunToolReq *req, size_t *out_len);

/* Entry point for the re-exec'd `cclaw --run-tool` child.
 * Reads request from fd 3, decodes the tier into a TierDescriptor, sets up the
 * sandbox, runs the tier's run_fn, writes result to fd 3, _exit. Never returns
 * on success. */
int run_tool_main(void);

#endif
