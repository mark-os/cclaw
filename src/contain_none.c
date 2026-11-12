/* Containment backend: none. For hosts and platforms where cclaw cannot
 * impose a boundary on a child itself (specs/portability.md). A contained
 * profile is refused exactly as on a Linux host without namespaces; only the
 * `host` profile runs tools, and it never reaches this file. */
#include "contain.h"
#include <errno.h>
#include <stdio.h>

const char *contain_backend(void) { return "none"; }

int contain_probe(char *why, size_t cap) {
    if (why && cap) snprintf(why, cap, "this build has no containment backend");
    return 0;
}

int contain_enter(const char *workspace, const char *cwd_path,
                  const char *db_path, const SandboxConfig *cfg) {
    (void)workspace; (void)cwd_path; (void)db_path; (void)cfg;
    errno = ENOSYS;
    return -1;
}

void contain_egress(const SandboxConfig *cfg) { (void)cfg; }

const char *contain_llm_sock_path(void) { return ""; }
