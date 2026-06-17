#ifndef CCLAW_TOOL_FILE_H
#define CCLAW_TOOL_FILE_H

#include "tools.h"
#include "sandbox.h"

/* Context for file tools — workspace + sandbox policy for forked execution */
typedef struct {
    const char *workspace;
    const char *cwd_path;       /* CWD rw mount (CLI mode, NULL in daemon) */
    const char *db_path;        /* cclaw.db path for masking */
    int read_only;              /* 1 = refuse writes (restricted trust) */
    int sandbox;                /* 1 = fork+namespace, 0 = in-process (host) */
    int workspace_ro;
    int mount_cwd;
    int env_mode;
    struct { int nproc; int as_mb; int cpu_sec; } rlimits;
} FileReadCtx;

/* Register file_read tool into registry. Returns 0 on success.
 * ctx must remain valid for tool lifetime. */
int tool_file_read_register(ToolRegistry *reg, FileReadCtx *ctx);

/* Handler: parse JSON args {"path":"..."}, read file within workspace.
 * user_data is FileReadCtx*. Returns heap-allocated result. */
char *tool_file_read_handler(const char *arguments, void *user_data);

/* Register file_write tool into registry. Returns 0 on success.
 * ctx must remain valid for tool lifetime. */
int tool_file_write_register(ToolRegistry *reg, FileReadCtx *ctx);

/* Handler: parse JSON args {"path":"...","content":"..."}, write file within workspace.
 * user_data is FileReadCtx*. Returns heap-allocated result. */
char *tool_file_write_handler(const char *arguments, void *user_data);

/* Register file_list (ls) tool. ctx must remain valid for tool lifetime. */
int tool_file_list_register(ToolRegistry *reg, FileReadCtx *ctx);
char *tool_file_list_handler(const char *arguments, void *user_data);

/* Register file_find (glob) tool. */
int tool_file_find_register(ToolRegistry *reg, FileReadCtx *ctx);
char *tool_file_find_handler(const char *arguments, void *user_data);

/* Register file_edit (search/replace) tool. */
int tool_file_edit_register(ToolRegistry *reg, FileReadCtx *ctx);
char *tool_file_edit_handler(const char *arguments, void *user_data);

/* Register file_grep (content search) tool. */
int tool_file_grep_register(ToolRegistry *reg, FileReadCtx *ctx);
char *tool_file_grep_handler(const char *arguments, void *user_data);

/* Run a file handler in a forked sandbox child. Used by the tool dispatch
 * layer. If ctx->sandbox==0, runs handler in-process. Returns heap result. */
char *file_sandbox_run(FileReadCtx *ctx, char *(*handler)(const char *, void *),
                       const char *arguments);

#endif
