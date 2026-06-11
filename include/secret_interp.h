#ifndef CCLAW_SECRET_INTERP_H
#define CCLAW_SECRET_INTERP_H

#include "tool_shell.h"  /* ShellSecret */
#include <stddef.h>

/* Replace {{SECRET:name}} placeholders with actual secret values.
 * Returns new heap-allocated string (caller frees).
 * If no placeholders found, returns strdup of input. */
char *secret_interpolate(const char *text, const ShellSecret *secrets, size_t count);

/* Replace literal secret values with {{SECRET:name}} placeholders.
 * Processes longest secrets first to avoid partial matches.
 * Returns new heap-allocated string (caller frees). */
char *secret_deinterpolate(const char *text, const ShellSecret *secrets, size_t count);

#endif /* CCLAW_SECRET_INTERP_H */
