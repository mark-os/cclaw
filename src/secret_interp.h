#ifndef CCLAW_SECRET_INTERP_H
#define CCLAW_SECRET_INTERP_H

/* Secret interpolation and result post-processing: expands {{SECRET:name}}
 * before tool execution, deinterpolates values back out of results, and
 * runs the DLP scan + marker sanitizer over every tool result.
 */

#include "types.h"  /* ShellSecret */
#include <stddef.h>

/* Replace {{SECRET:name}} placeholders with actual secret values.
 * Returns new heap-allocated string (caller frees).
 * If no placeholders found, returns strdup of input. */
char *secret_interpolate(const char *text, const ShellSecret *secrets, size_t count);

/* Replace literal secret values with {{SECRET:name}} placeholders.
 * Processes longest secrets first to avoid partial matches.
 * Returns new heap-allocated string (caller frees). */
char *secret_deinterpolate(const char *text, const ShellSecret *secrets, size_t count);

/* Post-process a tool result: deinterpolate + secret scan.
 * Returns new heap-allocated string if modified, or NULL if unchanged. */
char *tool_result_postprocess(const char *result, const ShellSecret *secrets, size_t count);

/* List the distinct {{SECRET:name}} placeholder names in text (parse only —
 * no values touched, no secret store consulted; unknown names are listed too).
 * Returns a heap array of heap strings, *out_count entries (NULL if none or
 * OOM, *out_count 0). Free with secret_names_free(). */
char **secret_placeholder_names(const char *text, size_t *out_count);
void secret_names_free(char **names, size_t count);

#endif /* CCLAW_SECRET_INTERP_H */
