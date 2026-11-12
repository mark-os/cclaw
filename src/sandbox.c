#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
#include "sandbox.h"
#include "contain.h"
#include "log.h"

/* Count path components (slash count) — canonical realpath() output has no
 * trailing slash, so this orders ancestors before descendants. */
static int mount_depth(const char *p) {
    int d = 0;
    for (const char *c = p; *c; c++) if (*c == '/') d++;
    return d;
}

/* qsort comparator: shallow→deep, strcmp tie-break for determinism. */
static int mount_cmp(const void *a, const void *b) {
    const SandboxMount *x = a, *y = b;
    int dx = mount_depth(x->path), dy = mount_depth(y->path);
    if (dx != dy) return dx - dy;
    return strcmp(x->path, y->path);
}

size_t sandbox_plan_mounts(const SandboxMountReq *in, size_t n, SandboxMount *out) {
    size_t cnt = 0;
    for (size_t i = 0; i < n; i++) {
        if (!in[i].path || !in[i].path[0]) continue;
        char abs[PATH_MAX];
        if (!realpath(in[i].path, abs)) continue;  /* drop unresolvable */
        size_t k;
        for (k = 0; k < cnt; k++)
            if (strcmp(out[k].path, abs) == 0) break;
        if (k < cnt) {                 /* same subtree already planned */
            if (!in[i].ro) out[k].ro = 0;  /* rw wins: write implies read */
            continue;
        }
        snprintf(out[cnt].path, sizeof(out[cnt].path), "%s", abs);
        out[cnt].ro = in[i].ro;
        cnt++;
    }
    qsort(out, cnt, sizeof(*out), mount_cmp);
    return cnt;
}

/* Resolve HOME for the tool child. `workspace` is the caller's canonicalized
 * workspace path — the same one the bind mount and the post-pivot chdir use,
 * which is what HOME has to name: with a symlink component the raw path is not
 * a directory inside the namespace at all.
 *
 * Sandboxed profiles get the agent workspace: it is bind-mounted rw at its own
 * absolute path, it is the one directory the agent owns, and it persists across
 * tool calls. That makes the usual user-level install paths work and *stay*
 * working — `pip install --user`, `cargo install`, `npm -g --prefix $HOME` all
 * land under the workspace and are on PATH next call, so an agent can bootstrap
 * its own toolchain into the one place it is allowed to write.
 *
 * The host profile has no namespace, so the honest answer is the invoking
 * user's real home, read from the passwd database rather than an inherited
 * HOME (under the --run-tool re-exec the child starts from an empty environ,
 * so there is nothing to inherit). */
static const char *sandbox_resolve_home(const SandboxConfig *cfg,
                                        const char *workspace) {
    if (cfg->sandbox) {
        if (workspace && workspace[0]) return workspace;
        return "/tmp";
    }
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir && pw->pw_dir[0]) return pw->pw_dir;
    if (workspace && workspace[0]) return workspace;
    return "/tmp";
}

/* Build PATH: the agent's own workspace-local bin dirs first (see
 * sandbox_resolve_home), then the system dirs. The old value was
 * "/bin:/usr/bin", which made every toolchain invisible — node, go, cargo and
 * rustc all install outside those two directories, so `npm install` failed with
 * "not found" before any policy question arose. */
static void sandbox_set_path(const char *home) {
    static const char *sys =
        "/usr/local/sbin:/usr/local/bin:/usr/local/go/bin:"
        "/usr/sbin:/usr/bin:/sbin:/bin";
    char buf[PATH_MAX * 3 + 128];
    int n = snprintf(buf, sizeof(buf), "%s/.local/bin:%s/.cargo/bin:%s/bin:%s",
                     home, home, home, sys);
    setenv("PATH", (n > 0 && (size_t)n < sizeof(buf)) ? buf : sys, 1);
}

/* scrub the environment before exec. env_mode==1 wipes everything and sets only
 * PATH+TMPDIR+HOME; otherwise inherit minus known secret-bearing vars. HOME is
 * set under both modes — toolchains treat its absence as a hard error. */
static void sandbox_scrub_env(const SandboxConfig *cfg, const char *workspace) {
    extern char **environ;
    const char *home = sandbox_resolve_home(cfg, workspace);
    if (cfg->env_mode == 1) {
        /* Clean allowlist: wipe entire env, set only essentials */
        if (environ) environ[0] = NULL;  /* portable clearenv */
        sandbox_set_path(home);
        setenv("TMPDIR", "/tmp", 1);
        setenv("HOME", home, 1);
        return;
    }

    /* host mode: inherit whatever env the child was given, minus known
     * secret-bearing names. Only `host` reaches here — every sandboxed
     * profile uses the clean allowlist above. */
    sandbox_set_path(home);
    setenv("TMPDIR", "/tmp", 1);
    setenv("HOME", home, 1);
    char *drop_keys[256];
    int nkeys = 0;
    for (int i = 0; environ[i] && nkeys < 256; i++) {
        char *eq = strchr(environ[i], '=');
        if (!eq) continue;
        size_t klen = (size_t)(eq - environ[i]);
        char name[256];
        if (klen >= sizeof(name)) continue;
        memcpy(name, environ[i], klen);
        name[klen] = '\0';
        if (strncmp(name, "CCLAW_", 6) == 0 ||
            strstr(name, "API_KEY") || strstr(name, "APIKEY") ||
            strstr(name, "TOKEN") || strstr(name, "SECRET") ||
            strstr(name, "PASSWORD") || strstr(name, "CREDENTIALS")) {
            drop_keys[nkeys] = strdup(name);
            if (drop_keys[nkeys]) nkeys++;
        }
    }
    for (int i = 0; i < nkeys; i++) {
        unsetenv(drop_keys[i]);
        free(drop_keys[i]);
    }
}

/* ASan reserves terabytes of virtual address space for shadow memory and its
 * allocator mmaps freely at runtime — an RLIMIT_AS cap makes the instrumented
 * process abort with "Failed to mmap". Sanitizer builds are dev-only, so skip
 * just the AS cap there (nproc/cpu still apply). */
#if defined(__SANITIZE_ADDRESS__)
#define CCLAW_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CCLAW_ASAN 1
#endif
#endif

/* Trust-level: apply resource limits before exec */
static void sandbox_apply_rlimits(const SandboxConfig *cfg) {
    if (cfg->rlimits.nproc > 0) {
        struct rlimit rl = {(rlim_t)cfg->rlimits.nproc, (rlim_t)cfg->rlimits.nproc};
        setrlimit(RLIMIT_NPROC, &rl);
    }
#ifndef CCLAW_ASAN
    if (cfg->rlimits.as_mb > 0) {
        rlim_t bytes = (rlim_t)cfg->rlimits.as_mb * 1024 * 1024;
        struct rlimit rl = {bytes, bytes};
        setrlimit(RLIMIT_AS, &rl);
    }
#endif
    if (cfg->rlimits.cpu_sec > 0) {
        struct rlimit rl = {(rlim_t)cfg->rlimits.cpu_sec, (rlim_t)cfg->rlimits.cpu_sec};
        setrlimit(RLIMIT_CPU, &rl);
    }
}

int sandbox_child_setup(const SandboxConfig *cfg) {
    const char *ws = cfg->workspace;
    const char *cwd = cfg->cwd_path;
    const char *psock = cfg->proxy_sock;

    /* Sandbox profile: suppress CWD mount / proxy per policy */
    if (!cfg->mount_cwd) cwd = NULL;
    if (cfg->net_mode) psock = NULL;

    /* Canonicalize the workspace once, here, while the host filesystem is still
     * reachable: the bind mount and the post-pivot chdir both use the resolved
     * path, so HOME must too — a HOME carrying a symlink component names
     * nothing inside the namespace, and every toolchain that writes under $HOME
     * fails. It cannot be resolved later: sandbox_scrub_env runs after
     * pivot_root, where the symlink itself is gone. A realpath failure leaves
     * the raw path, which is exactly what the bind does with it — no new
     * failure mode, and host (no namespace, no bind) is unaffected either way. */
    char ws_abs[PATH_MAX];
    if (ws && ws[0] && realpath(ws, ws_abs)) ws = ws_abs;

    /* namespace sandbox — fail closed if requested but unavailable */
    if (cfg->sandbox && contain_enter(ws, cwd, cfg->db_path, cfg) != 0) {
        int e = errno;  /* log call below may clobber it */
        LOG_INFO_("sandbox namespace_fail errno=%d action=abort", e);
        fprintf(stderr, "error: namespace sandbox unavailable (errno=%d); "
                "this sandbox profile requires it — enable unprivileged user "
                "namespaces or set the agent's sandbox_profile to 'host'\n", e);
        return -1;
    }

    /* host has no boundary, so contain_enter's chdir never ran. In
     * CLI mode cwd_path is the user's own CWD and inheriting it is the whole
     * point of host — `cclaw` in a repo operates on that repo. Under --daemon
     * cwd_path is NULL and the daemon's CWD is meaningless to the agent, so
     * start in the workspace instead. */
    if (!cfg->sandbox && !cwd && ws && ws[0]) {
        if (chdir(ws) != 0)
            LOG_INFO_("sandbox host_chdir_fail errno=%d", errno);
    }

    sandbox_scrub_env(cfg, ws);

    /* LLM() bridge socket: the backend's in-boundary path when contained,
     * the real agent-dir path when running on the bare host. */
    if (cfg->llm_sock && cfg->llm_sock[0])
        setenv("CCLAW_LLM_SOCK",
               cfg->sandbox ? contain_llm_sock_path() : cfg->llm_sock, 1);

    sandbox_apply_rlimits(cfg);

    if (psock && cfg->sandbox) contain_egress(cfg);

    return 0;
}

/* No profile sets RLIMIT_AS. It caps *address space*, not resident memory, and
 * every modern runtime reserves VA far beyond its footprint — Go's allocator
 * arenas, V8's heap cages, LLVM — so a cap large enough to be safe is too large
 * to bound anything, and one small enough to bound anything refuses to start
 * `go`, `node`, or `rustc` before they do a byte of work. NPROC and CPU are the
 * knobs that actually bound a runaway; a cgroup memory limit is the right tool
 * if a real memory ceiling is ever needed. */
void sandbox_policy_from_profile(const char *sandbox_profile, SandboxConfig *cfg) {
    if (sandbox_profile && strcmp(sandbox_profile, "host") == 0) {
        /* No namespace, no proxy: the tool child runs with the invoking user's
         * own filesystem and network. cwd_path (CLI only) keeps the user's CWD;
         * under --daemon there is none, so the child starts in the workspace. */
        cfg->sandbox = 0;
        cfg->env_mode = 0; cfg->net_mode = 0; cfg->mount_cwd = 1;
        cfg->rlimits.nproc = 0; cfg->rlimits.as_mb = 0; cfg->rlimits.cpu_sec = 0;
    } else if (sandbox_profile && strcmp(sandbox_profile, "restricted") == 0) {
        /* Bounded, but still able to run real programs: the limits are a
         * runaway backstop, not a straitjacket. The workspace stays rw so
         * $HOME works — what makes this restricted is no packets, ever: no
         * proxy socket reaches the child, so the empty netns is the whole
         * story and not even a promoted declared-hosts tool egresses. */
        cfg->sandbox = 1;
        cfg->env_mode = 1; cfg->net_mode = 1; cfg->mount_cwd = 0;
        cfg->rlimits.nproc = 64; cfg->rlimits.as_mb = 0; cfg->rlimits.cpu_sec = 120;
    } else { /* "standard", unknown, NULL — the default */
        /* Resources are effectively unbounded and visibility is not widened:
         * no CWD mount, so the agent sees its own workspace and nothing of the
         * user's files or any other agent's — additional paths arrive only as
         * read_path/write_path grants. NPROC is kept purely as the fork-bomb
         * backstop (on a 128MB target one bad generated shell line otherwise
         * takes the daemon down with it); there is no per-process CPU cap,
         * because legit builds brush it and it punishes nothing real. */
        cfg->sandbox = 1;
        cfg->env_mode = 1; cfg->net_mode = 0; cfg->mount_cwd = 0;
        cfg->rlimits.nproc = 256; cfg->rlimits.as_mb = 0; cfg->rlimits.cpu_sec = 0;
    }
}

/* Delegates sandbox-profile branching to sandbox_policy_from_profile() — the
 * only duplication here is this manual field-by-field copy from SandboxConfig
 * into SandboxProfile. The two structs deliberately have different shapes
 * (SandboxProfile adds read_paths/write_paths; SandboxConfig adds mount/path
 * fields), so a memcpy shortcut would be fragile — it'd silently break on
 * field reordering instead of failing to compile. This field list must be
 * kept in sync with SandboxConfig's policy fields by hand whenever either
 * struct's policy fields change. */
void sandbox_profile_resolve(const char *sandbox_profile, SandboxProfile *p) {
    SandboxConfig c = {0};
    sandbox_policy_from_profile(sandbox_profile, &c);
    p->sandbox      = c.sandbox;
    p->env_mode     = c.env_mode;
    p->net_mode     = c.net_mode;
    p->mount_cwd    = c.mount_cwd;
    p->rlimits.nproc   = c.rlimits.nproc;
    p->rlimits.as_mb   = c.rlimits.as_mb;
    p->rlimits.cpu_sec = c.rlimits.cpu_sec;
}
