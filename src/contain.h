#ifndef CCLAW_CONTAIN_H
#define CCLAW_CONTAIN_H

#include <stddef.h>

/* Containment backend — the boundary imposed from outside on a tool child
 * (specs/portability.md, layer 3). One implementation is linked per build;
 * everything above it asks these questions instead of knowing the mechanism. */

/* Short name of the backend this build links, e.g. "linux-ns". */
const char *contain_backend(void);

/* Can a child be contained on this host, right now? Returns 1 if so. On 0,
 * `why` (when non-NULL) receives a one-line reason for the operator. Forks a
 * throwaway child to try it, so the caller's own process is never changed. */
int contain_probe(char *why, size_t cap);

#endif
