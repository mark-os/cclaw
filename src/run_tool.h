#ifndef CCLAW_RUN_TOOL_H
#define CCLAW_RUN_TOOL_H

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
 * body-length prefix is followed by a tier byte (0=file, 1=shell) and the
 * fields in a fixed order known to both sides. Primitives: u32 = 4-byte LE;
 * str = u32 length + that many raw bytes (length 0 = NULL/absent). See
 * run_tool.c for the canonical field order per tier. */

/* Serialize a file-tier request into a length-prefixed blob.
 * Returns malloc'd buffer; *out_len set to total size (4 + json_len).
 * Returns NULL on OOM or if result would exceed RUNTOOL_REQUEST_MAX. */
char *run_tool_serialize_file_request(
    const char *tool_name,
    const char *arguments,
    const char *workspace,
    const char **read_paths, size_t read_count,
    const char **write_paths, size_t write_count,
    int workspace_ro, int mount_cwd, const char *cwd_path,
    int env_mode, int nproc, int as_mb, int cpu_sec,
    size_t *out_len);

/* Entry point for the re-exec'd `cclaw --run-tool` child.
 * Reads request from fd 3, sets up sandbox, dispatches file handler,
 * writes result to fd 3, _exit. Never returns on success. */
int run_tool_main(void);

/* ── Shell-tier serialization ─────────────────────────────────────────
 * Secrets: pre-resolved by the daemon parent. The broker/child never hold
 * the master key. Blob carries:
 *   - command: with {{SECRET}} already interpolated
 *   - secrets[]: minimal name/value array (only command-referenced) for
 *     the child's setenv("CCLAW_SECRET_*") after env scrub
 * Caller MUST explicit_bzero the returned blob after write(fd3). */

/* A name/value secret for shell env injection (pre-filtered in parent). */
typedef struct {
    const char *name;
    const char *value;
} RunToolSecret;

/* Serialize a shell-tier request. Returns malloc'd blob, *out_len = total.
 * Returns NULL on OOM or if result exceeds RUNTOOL_REQUEST_MAX. */
char *run_tool_serialize_shell_request(
    const char *command,
    int timeout,
    const char *workspace,
    const char *cwd_path,
    const char *agent_dir,
    const char **host_rules, size_t host_count,
    const RunToolSecret *secrets, size_t secret_count,
    int sandbox, int env_mode, int net_mode, int mount_cwd,
    int workspace_ro, int nproc, int as_mb, int cpu_sec,
    const char **read_paths, size_t read_count,
    const char **write_paths, size_t write_count,
    size_t *out_len);

#endif
