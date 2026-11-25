/* Containment backend: none. For hosts and platforms where cclaw cannot
 * impose a boundary on a child itself (specs/portability.md). It reports no
 * capabilities, so the planner refuses every plan that needs one, exactly as
 * on a Linux host without namespaces; what needs none (containment `none`,
 * and file and js tools under `basic`) still runs. */
#include "contain.h"
#include <errno.h>
#include <stdio.h>

const char *contain_backend(void) { return "none"; }

unsigned contain_caps(char *why, size_t cap) {
    if (why && cap) snprintf(why, cap, "this build has no containment backend");
    return 0;
}

void contain_hint(char *out, size_t cap) { if (out && cap) out[0] = '\0'; }

int contain_enter(const char *workspace, const char *db_path,
                  const SandboxConfig *cfg) {
    (void)workspace; (void)db_path; (void)cfg;
    errno = ENOSYS;
    return -1;
}

int contain_landlock_abi(void) { return 0; }
int contain_enter_net(void) { errno = ENOSYS; return -1; }
int contain_landlock(const SandboxMount *r, size_t n) { (void)r; (void)n; return -1; }
int contain_seccomp(unsigned flags) { (void)flags; return -1; }
int contain_intercept(const SandboxConfig *c, unsigned sc, int port) {
    (void)c; (void)sc; (void)port;
    return -1;
}
ContainSup *contain_supervise(int sock, const ContainPlan *p, const char *const *roots,
                              size_t n) {
    (void)sock; (void)p; (void)roots; (void)n;
    return NULL;
}
void contain_supervise_end(ContainSup *s) { (void)s; }

int contain_egress(const SandboxConfig *cfg) { (void)cfg; return -1; }

const char *contain_llm_sock_path(void) { return ""; }
