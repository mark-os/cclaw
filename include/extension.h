#ifndef CCLAW_EXTENSION_H
#define CCLAW_EXTENSION_H

#include "tool_js.h"
#include "tools.h"
#include "config.h"
#include <stddef.h>

/* T254: Discover extensions in workspace/extensions/.
 * Returns sorted array of absolute file paths. Caller frees via extension_list_free().
 * Scans .js files and subdir index.js files, sorted alphabetically. */
char **extension_discover(const char *workspace, size_t *count);
void extension_list_free(char **paths, size_t count);

/* T255: Load discovered extensions into shared QuickJS context.
 * Evals each as (function(cclaw){ <source> })(cclaw_api).
 * On throw → log warning, skip. Returns number successfully loaded. */
int extension_load(char **paths, size_t count, JsSessionRuntime *rt,
                   ToolRegistry *reg, const Config *cfg);

#endif
