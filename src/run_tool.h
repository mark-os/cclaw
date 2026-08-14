#ifndef CCLAW_RUN_TOOL_H
#define CCLAW_RUN_TOOL_H

/* The --run-tool sandbox broker wire protocol: the flat binary request
 * frame the parent sends and the child decodes. Both ends are the same
 * binary, so the wire is a struct, not JSON (AGENTS.md).
 */

#include "sandbox.h"
#include "tool_args.h"

#include <stddef.h>
#include <stdint.h>

/* Max request blob size. Capped to guarantee the blocking write() on the
 * poll-loop thread completes instantly into the kernel socket buffer (which is
 * typically >=64KB). If a request exceeds this, the parent refuses to dispatch
 * — fail closed, loud error. A blob this large is a design smell (inline script
 * → should be a file). */
#define RUNTOOL_REQUEST_MAX 32768

/* Largest tool result that may cross the wire and reach the DB. Anything
 * bigger is written to the request's spill_path by the child that already
 * holds it in memory, and the result names that file instead.
 *
 * The parent applies the same two limits (context.c) for tools that never
 * fork. A child-spilled result must land under BOTH or the parent would spill
 * it a second time — overwriting the full output with the truncated copy. */
#define RUNTOOL_RESULT_MAX       (64 * 1024)
#define RUNTOOL_RESULT_MAX_LINES 2000

/* The socketpair fd the request blob arrives on (and the result departs on)
 * in the re-exec'd child. */
#define RUNTOOL_FD_REQUEST 3

/* Response framing: [1-byte status][4-byte meta_len][hosts JSON][result].
 * The status byte is the tool's explicit outcome, written by the child that
 * produced the body and read by the parent's drain loop — no reader ever
 * infers failure from the result text. Distinct from the child's exit code,
 * which keeps its process-lifecycle meaning (sandbox refusal, crash, signal). */
#define RUNTOOL_STATUS_OK    0
#define RUNTOOL_STATUS_ERROR 1

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
    int env_mode, nproc, as_mb, cpu_sec;
    int sandbox, net_mode;
    /* file/all */
    const char *workspace, *cwd_path;
    /* cclaw.db path — carried so the child can mask the key + ciphertext out
     * of any bound path. The child never opens the DB. */
    const char *db_path;
    /* Host directory bind-mounted as /tmp in the child: the agent's persistent
     * scratch, created and owned by the parent (scratch_dir_ensure). */
    const char *tmp_dir;
    int mount_cwd;
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
    /* Pre-extracted tool params (tool_args_extract in the parent) — the
     * child never parses argument JSON. file/web/js tiers. */
    const ToolWireArg *params; size_t param_count;
    /* Parent-composed advice for the proxy-deny summary — set when the call's
     * egress was narrowed to its secrets' bound hosts, so a denial names the
     * missing binding instead of suggesting a grants.hosts request that
     * wouldn't help. The child appends it verbatim; it stays dumb. */
    const char *egress_note;
    /* Where to write output too large for the wire (parent-resolved, dir
     * already created; NULL disables spilling for this call). */
    const char *spill_path;
} RunToolReq;

/* Child-side parsed request (owned strings). Mirrors RunToolReq's wire order.
 * Strings/arrays are heap-owned by the --run-tool process for its (short)
 * lifetime; never freed before _exit. This is the one type the per-tool tier
 * leaves (tool_*_tier_run / tool_shell_tier_exec) consume. */
typedef struct {
    int   tier;
    char *tool_name;
    int   env_mode, nproc, as_mb, cpu_sec;
    int   sandbox, net_mode;
    char *workspace, *cwd_path;
    char *db_path;
    char *tmp_dir;
    int   mount_cwd;
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
    struct RunToolParam { char *key; int kind; char *value;
                          char **list; size_t list_n; } *params;
    size_t param_count;
    char *egress_note;
    char *spill_path;
} RunToolParsed;

/* Child-side param lookup (pre-extracted by the parent). *_str returns the
 * TEXT value (numbers/bools arrive as decimal text), NULL if absent. */
const char *run_tool_param_str(const RunToolParsed *q, const char *key);
int run_tool_param_int(const RunToolParsed *q, const char *key, int def);
int run_tool_param_bool(const RunToolParsed *q, const char *key, int def);
/* Opaque JSON sub-blob (TOOL_ARG_JSON), NULL if absent. */
const char *run_tool_param_json(const RunToolParsed *q, const char *key);
/* Flattened string list (TOOL_ARG_LIST); NULL + *n=0 if absent. */
char **run_tool_param_list(const RunToolParsed *q, const char *key, size_t *n);

/* Zero req; set tier/tool_name plus all SandboxProfile-derived fields and
 * workspace/cwd_path. Callers set tier-specific fields (params, command,
 * egress rules) after.
 *
 * db_path is a parameter rather than a field the caller may set afterwards
 * because forgetting it is silent and unsafe: sandbox_mask_state_files()
 * returns immediately on NULL, so a tier that omits it ships with the key
 * mask disabled. The file tier did exactly that. Pass the path (never the
 * handle — the child does not open the DB); NULL only where there is no DB. */
void run_tool_req_init(RunToolReq *req, int tier, const char *tool_name,
                       const SandboxProfile *sb,
                       const char *workspace, const char *cwd_path,
                       const char *db_path);

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
