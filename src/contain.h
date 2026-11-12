#ifndef CCLAW_CONTAIN_H
#define CCLAW_CONTAIN_H

#include <stddef.h>
#include "sandbox.h"

/* Containment backend — the boundary imposed from outside on a tool child
 * (specs/portability.md, layer 3). One implementation is linked per build;
 * everything above it asks these questions instead of knowing the mechanism. */

/* Short name of the backend this build links, e.g. "linux-ns". */
const char *contain_backend(void);

/* Can a child be contained on this host, right now? Returns 1 if so. On 0,
 * `why` (when non-NULL) receives a one-line reason for the operator. Forks a
 * throwaway child to try it, so the caller's own process is never changed. */
int contain_probe(char *why, size_t cap);

/* Enter the boundary, in the forked tool child: after a 0 return the process
 * sees only the workspace, the CWD mount, the granted paths and the read-only
 * system directories, and has no network route but the proxy socket. -1 with
 * errno set if the boundary cannot be established; the caller must not run
 * the tool. workspace and cwd_path are already resolved by the caller. */
int contain_enter(const char *workspace, const char *cwd_path,
                  const char *db_path, const SandboxConfig *cfg);

/* Wire the contained child's egress to the broker's proxy. Call after
 * contain_enter and the environment scrub, only when a proxy is serving. */
void contain_egress(const SandboxConfig *cfg);

/* Where the parent's LLM bridge socket appears inside the boundary. */
const char *contain_llm_sock_path(void);

#endif
