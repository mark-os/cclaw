#ifndef CCLAW_TOOL_FILE_H
#define CCLAW_TOOL_FILE_H

#include "tools.h"

/* Register file_read tool into registry. Returns 0 on success.
 * user_data must point to a null-terminated workspace path string. */
int tool_file_read_register(ToolRegistry *reg, const char *workspace);

/* Handler: parse JSON args {"path":"..."}, read file within workspace.
 * user_data is the workspace path (char*). Returns heap-allocated result. */
char *tool_file_read_handler(const char *arguments, void *user_data);

/* Register file_write tool into registry. Returns 0 on success. */
int tool_file_write_register(ToolRegistry *reg, const char *workspace);

/* Handler: parse JSON args {"path":"...","content":"..."}, write file within workspace.
 * user_data is the workspace path (char*). Returns heap-allocated result. */
char *tool_file_write_handler(const char *arguments, void *user_data);

#endif
