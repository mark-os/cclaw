#ifndef CCLAW_TOOL_FILE_H
#define CCLAW_TOOL_FILE_H

#include "tools.h"

/* T118/T228: Context for file_read — workspace + optional extra paths */
typedef struct {
    const char *workspace;
    const char *extra_read_path;  /* e.g. /tmp/cclaw-<session_id> */
    const char *cclaw_path;       /* T228: CWD read-only path (CCLAW_PATH env) */
} FileReadCtx;

/* Register file_read tool into registry. Returns 0 on success.
 * ctx must remain valid for tool lifetime. */
int tool_file_read_register(ToolRegistry *reg, FileReadCtx *ctx);

/* Handler: parse JSON args {"path":"..."}, read file within workspace.
 * user_data is FileReadCtx*. Returns heap-allocated result. */
char *tool_file_read_handler(const char *arguments, void *user_data);

/* Register file_write tool into registry. Returns 0 on success. */
int tool_file_write_register(ToolRegistry *reg, const char *workspace);

/* Handler: parse JSON args {"path":"...","content":"..."}, write file within workspace.
 * user_data is the workspace path (char*). Returns heap-allocated result. */
char *tool_file_write_handler(const char *arguments, void *user_data);

/* Register file_list (ls) tool. Shares FileReadCtx with file_read for path
 * guarding. ctx must remain valid for tool lifetime. */
int tool_file_list_register(ToolRegistry *reg, FileReadCtx *ctx);
char *tool_file_list_handler(const char *arguments, void *user_data);

/* Register file_find (glob) tool. Shares FileReadCtx with file_read. */
int tool_file_find_register(ToolRegistry *reg, FileReadCtx *ctx);
char *tool_file_find_handler(const char *arguments, void *user_data);

/* Register file_edit (search/replace) tool. Edits files within the workspace. */
int tool_file_edit_register(ToolRegistry *reg, FileReadCtx *ctx);
char *tool_file_edit_handler(const char *arguments, void *user_data);

/* Register file_grep (content search) tool. Shares FileReadCtx with file_read. */
int tool_file_grep_register(ToolRegistry *reg, FileReadCtx *ctx);
char *tool_file_grep_handler(const char *arguments, void *user_data);

#endif
