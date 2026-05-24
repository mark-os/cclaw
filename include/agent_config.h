#ifndef CCLAW_AGENT_CONFIG_H
#define CCLAW_AGENT_CONFIG_H

#include <stddef.h>

/* T75: agent discovery — scan agents/ dir, list available agents by name.
 * Returns heap-allocated array of agent names (each heap-allocated).
 * Caller must free each name and the array. Sets *count. */
char **agent_discover(const char *agents_dir, size_t *count);

/* Free array returned by agent_discover. */
void agent_discover_free(char **names, size_t count);

#endif
