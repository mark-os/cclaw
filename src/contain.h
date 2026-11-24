#ifndef CCLAW_CONTAIN_H
#define CCLAW_CONTAIN_H

#include <stddef.h>
#include "sandbox.h"

/* Containment backend — the boundary imposed from outside on a tool child
 * (specs/portability.md, layer 3). One implementation is linked per build;
 * everything above it asks these questions instead of knowing the mechanism. */

/* Short name of the backend this build links, e.g. "linux-ns". */
const char *contain_backend(void);

/* What this host can enforce. MOUNTNS: an unprivileged user namespace with a
 * mount, IPC, PID and network namespace in it — the sandbox contain_enter
 * builds. NETNS: a user and network namespace alone (contain_enter_net).
 * SHIM: the backend can serve the egress proxy to an unconfined child on the
 * host's loopback (NE_ENV). LANDLOCK: a Landlock ruleset can be enforced on
 * an unprivileged process (kernel 5.13+). SECCOMP: a seccomp filter can be
 * installed (kernel 3.5+). Bit values are what CCLAW_CONTAIN_CAPS_MASK takes. */
#define CONTAIN_CAP_MOUNTNS  0x1u
#define CONTAIN_CAP_NETNS    0x2u
#define CONTAIN_CAP_SHIM     0x4u
#define CONTAIN_CAP_LANDLOCK 0x8u
#define CONTAIN_CAP_SECCOMP  0x10u

/* The capability bitmask, probed once per process by fork-and-try (the
 * caller's own process is never changed), cached in memory, never persisted.
 * `why` (when non-NULL) receives why MOUNTNS is missing, "" when present.
 * CCLAW_CONTAIN_CAPS_MASK, a test and diagnostic knob read only here in the
 * parent, is ANDed in: it can only take capabilities away, which can only
 * turn a call into a refusal or a code-enforced plan. */
unsigned contain_caps(char *why, size_t cap);

/* The kernel's Landlock ABI version as the probe found it (0 = none), for
 * --doctor. The child asks the kernel again; this is never sent to it. */
int contain_landlock_abi(void);

/* After a failed probe: one line saying how this host can be made to allow
 * containment, or "" when the backend knows of nothing to try. Advice only —
 * the probe is the detector. */
void contain_hint(char *out, size_t cap);

/* FE_MOUNTNS, in the forked tool child: after a 0 return the process sees
 * only the workspace, the granted paths and the read-only system
 * directories, and has no network route but the proxy socket. -1 with errno
 * set if the boundary cannot be established; the caller must not run the
 * tool. workspace is already resolved by the caller. */
int contain_enter(const char *workspace, const char *db_path,
                  const SandboxConfig *cfg);

/* NE_NETNS without the mount namespace: a user namespace (identity map) and
 * an empty network namespace with lo up; the files stay the host's. The
 * proxy socket is reached at its real path. 0, or -1 with errno set. */
int contain_enter_net(void);

/* FE_LANDLOCK: restrict this process and everything it execs to `rules`
 * (canonical paths; .ro is CONTAIN_LL_RW, _RO or _DEV), using only the
 * access rights this kernel's ABI knows. An absent path is skipped; anything
 * else that fails returns -1 and the caller must not run the tool. Needs
 * no_new_privs already set. */
enum { CONTAIN_LL_RW = 0, CONTAIN_LL_RO = 1, CONTAIN_LL_DEV = 2 };
int contain_landlock(const SandboxMount *rules, size_t n);

/* A seccomp filter for this process and everything it execs. Any other
 * architecture (and on x86_64 the x32 ABI) is killed; then each flag denies:
 * NOSOCKET socket() for every family (EACCES; socketpair stays), NOTRACE
 * ptrace and process_vm_readv/writev (EPERM). io_uring is ENOSYS under every flag: its operations would bypass a
 * syscall filter. 0, or -1 (the caller must not run the tool). */
#define CONTAIN_SC_NOSOCKET 0x1u
#define CONTAIN_SC_NOTRACE  0x2u
int contain_seccomp(unsigned flags);

/* Wire the child's egress to the broker's proxy: NE_NETNS (inside the
 * namespace, after contain_enter or contain_enter_net) or NE_ENV (on the host's loopback). Call
 * after the environment scrub, only when a proxy is serving. 0, or -1 when
 * the proxy could not be wired (the child then has no proxy variables). */
int contain_egress(const SandboxConfig *cfg);

/* Where the parent's LLM bridge socket appears inside the boundary. */
const char *contain_llm_sock_path(void);

#endif
