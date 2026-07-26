#ifndef CCLAW_TOOL_FILE_H
#define CCLAW_TOOL_FILE_H

#include "tools.h"
#include "sandbox.h"
#include "run_tool.h"

/* Context for file tools — workspace + trust-derived sandbox profile. */
typedef struct {
    const char *workspace;
    const char *cwd_path;       /* CWD rw mount (CLI mode, NULL in daemon) */
    const char *db_path;        /* cclaw.db path for masking */
    SandboxProfile sb;          /* trust-derived policy + grant paths */
} FileReadCtx;

/* Register the six file tools (all EXEC_SANDBOX; registry entries carry
 * tool_sandboxed_stub — execution happens only in the --run-tool child on
 * pre-extracted wire params). ctx must remain valid for tool lifetime. */
int tool_file_read_register(ToolRegistry *reg, FileReadCtx *ctx);
int tool_file_write_register(ToolRegistry *reg, FileReadCtx *ctx);
int tool_file_list_register(ToolRegistry *reg, FileReadCtx *ctx);
int tool_file_find_register(ToolRegistry *reg, FileReadCtx *ctx);
int tool_file_edit_register(ToolRegistry *reg, FileReadCtx *ctx);
int tool_file_grep_register(ToolRegistry *reg, FileReadCtx *ctx);

/* --run-tool tier leaf: dispatch a file tool by name on the request's wire
 * params. Called by the broker after the sandbox is applied. */
char *tool_file_tier_run(const RunToolParsed *q);

#endif
