#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <pwd.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sandbox.h"
#include "log.h"
#include "preload_blob.h"
#include "net_shim_blob.h"

/* Fixed in-sandbox path the broker's per-call proxy UDS is bind-mounted to. The
 * socket itself lives in the agent folder on the host (outside the workspace);
 * inside the netns the preload lib and net_shim reach it here. */
#define SANDBOX_PROXY_SOCK_PATH "/tmp/.cclaw_proxy.sock"

/* The egress interposer lives on the root tmpfs, not in /tmp. /tmp is now a
 * persistent bind of the agent's own scratch directory, so anything we drop
 * there survives the call and is the agent's to modify; the root tmpfs is
 * rebuilt per call and holds only our plumbing. (Tampering was never an egress
 * bypass — CLONE_NEWNET has no route out, so a disabled interposer is
 * self-inflicted denial of service — but our machinery does not belong in the
 * agent's scratch space.) */
#define SANDBOX_PRELOAD_PATH "/.cclaw_net.so"

/* Did a grant name the DB file itself, rather than merely a directory that
 * contains it? Exact match only — a read_path on ~/.cclaw or on $HOME must not
 * count, or the accident the mask exists to prevent comes straight back. */
static int sandbox_db_explicitly_granted(const char *db_abs,
                                         const SandboxConfig *cfg) {
    if (!cfg || !cfg->extra_mounts) return 0;
    for (size_t i = 0; i < cfg->extra_mount_count; i++) {
        const char *p = cfg->extra_mounts[i].path;
        if (!p || !p[0]) continue;
        char abs[PATH_MAX];
        if (realpath(p, abs) && strcmp(abs, db_abs) == 0) return 1;
    }
    return 0;
}

/* Bind-mask the secret key + DB ciphertext from the child's view.
 *
 * PROVISIONAL — this exists only because the default backend stores the key as
 * a file co-located with cclaw.db. Grants are approved on directories, so a
 * read_path covering the DB's directory hands over key + ciphertext together,
 * and the approver sees "read access to a directory" with no hint that it
 * means every stored secret. Masking closes that by path.
 *
 * It is a path-based defense and does not generalize: once the key can live
 * behind a key provider (kernel keyring, libsecret, an agent UDS, a TPM), the
 * child's inability to reach it follows from the sandbox mounting almost
 * nothing, and this function becomes dead code for those backends. Delete the
 * key half when the file backend is no longer the default.
 *
 * Two different rules, on purpose:
 *   key  — masked unconditionally. Reading it is not access to a resource, it
 *          is escalation to every secret in the DB, including system-scope
 *          ones and other agents'. A grant made on one agent's behalf must not
 *          confer authority over agents that never consented, so no grant can
 *          unmask it.
 *   DB   — masked unless a grant names the DB file itself. It is ordinary
 *          agent state and self-inspection is legitimate, so a deliberate,
 *          exact grant is honored; a directory grant that merely happens to
 *          contain it is not. Nothing breaks either way: db_query is the
 *          sanctioned read path and runs in the trusted parent, never through
 *          this mount tree.
 *
 * Targets not reachable in the child's mount tree never materialize under
 * newroot, so the stat fails and we skip them — already invisible by omission.
 * Must run after all binds and before pivot_root. */
static void sandbox_mask_state_files(const char *newroot, const char *db_path,
                                     const SandboxConfig *full_cfg) {
    if (!db_path || !db_path[0]) return;

    char db_abs[PATH_MAX];
    if (!realpath(db_path, db_abs)) return;  /* db must exist to be masked */

    /* Empty, mode-0 masking source on the new root's tmpfs */
    char empty[PATH_MAX];
    int n = snprintf(empty, sizeof(empty), "%s/.cclaw_masked", newroot);
    if (n < 0 || (size_t)n >= sizeof(empty)) return;
    int efd = open(empty, O_CREAT | O_WRONLY, 0000);
    if (efd < 0) return;
    close(efd);

    /* Targets: the key file (crown jewel) always; the DB family (ciphertext)
     * unless a grant named the DB file exactly. */
    char keyf[PATH_MAX];
    char *slash = strrchr(db_abs, '/');
    int klen = slash
        ? snprintf(keyf, sizeof(keyf), "%.*s/.cclaw_key", (int)(slash - db_abs), db_abs)
        : snprintf(keyf, sizeof(keyf), ".cclaw_key");
    const char *targets[6];
    char dbwal[PATH_MAX + 8], dbshm[PATH_MAX + 8];
    snprintf(dbwal, sizeof(dbwal), "%s-wal", db_abs);
    snprintf(dbshm, sizeof(dbshm), "%s-shm", db_abs);
    size_t nt = 0;
    if (klen > 0 && (size_t)klen < sizeof(keyf)) targets[nt++] = keyf;
    if (!sandbox_db_explicitly_granted(db_abs, full_cfg)) {
        targets[nt++] = db_abs;
        targets[nt++] = dbwal;
        targets[nt++] = dbshm;
    }

    for (size_t i = 0; i < nt; i++) {
        char dst[PATH_MAX + 64];
        int dn = snprintf(dst, sizeof(dst), "%s%s", newroot, targets[i]);
        if (dn < 0 || (size_t)dn >= sizeof(dst)) continue;
        struct stat st;
        if (stat(dst, &st) != 0) continue;  /* not in a bound path → already hidden */
        if (mount(empty, dst, NULL, MS_BIND, NULL) == 0)
            mount(NULL, dst, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY | MS_NOSUID, NULL);
    }
}

/* Read-only remount of a bind mount, preserving the flags the kernel locked
 * onto it when it was propagated from the host. In an unprivileged user
 * namespace a remount that would clear a locked flag (nosuid/nodev/noexec/
 * atime policy) returns EPERM, so we query the live flags via statvfs and OR
 * them back in — only then is adding MS_RDONLY permitted. Returns 0/-1. */
static int sandbox_remount_ro(const char *path) {
    unsigned long flags = MS_REMOUNT | MS_BIND | MS_RDONLY | MS_NOSUID;
    struct statvfs vfs;
    if (statvfs(path, &vfs) == 0) {
        if (vfs.f_flag & ST_NODEV)      flags |= MS_NODEV;
        if (vfs.f_flag & ST_NOEXEC)     flags |= MS_NOEXEC;
        if (vfs.f_flag & ST_NOATIME)    flags |= MS_NOATIME;
        if (vfs.f_flag & ST_NODIRATIME) flags |= MS_NODIRATIME;
        if (vfs.f_flag & ST_RELATIME)   flags |= MS_RELATIME;
    }
    return mount(NULL, path, NULL, flags, NULL);
}

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

/* Bind a single path into newroot at its absolute path.
 * Creates intermediate dirs, bind-mounts, optionally remounts ro.
 *
 * Handles files as well as directories: a bind mount does not care about the
 * source type, but the target must match it, so a file source needs an empty
 * regular file as its mount point rather than a directory. Without this a
 * read_path/write_path grant naming a file mounted nothing at all (ENOTDIR)
 * while every other layer — the approval, the DB row, the wire, path_under()
 * — accepted it, so the grant silently did nothing. File granularity is also
 * what lets an agent follow the "request the narrowest grant" instruction it
 * is given; before this it had no choice but to ask for a whole directory.
 *
 * Anything that is neither (device, socket, fifo) is refused: mounting those
 * into a sandbox is not a grant we want to honor implicitly. */
static void bind_path_into(const char *newroot, const char *abspath, int ro) {
    struct stat st;
    if (stat(abspath, &st) != 0) return;
    int isdir = S_ISDIR(st.st_mode);
    if (!isdir && !S_ISREG(st.st_mode)) {
        LOG_INFO_("sandbox grant_skip_not_file_or_dir path=%s mode=%o",
                  abspath, (unsigned)(st.st_mode & S_IFMT));
        return;
    }
    char dst[PATH_MAX + 64];
    int n = snprintf(dst, sizeof(dst), "%s%s", newroot, abspath);
    if (n < 0 || (size_t)n >= sizeof(dst)) return;
    /* mkdir -p the parent chain under newroot (every component up to the
     * final one, which the loop never reaches — it fires only on '/') */
    char *p = dst + strlen(newroot) + 1;
    for (char *slash = p; *slash; slash++) {
        if (*slash == '/') {
            *slash = '\0';
            mkdir(dst, 0755);
            *slash = '/';
        }
    }
    /* Final component: type must match the source or mount() gives ENOTDIR. */
    if (isdir) {
        mkdir(dst, 0755);
    } else {
        int fd = open(dst, O_CREAT | O_WRONLY, 0600);
        if (fd < 0) return;
        close(fd);
    }
    if (mount(abspath, dst, NULL, MS_BIND, NULL) != 0) {
        LOG_INFO_("sandbox grant_bind_fail path=%s errno=%d", abspath, errno);
        return;
    }
    if (ro) sandbox_remount_ro(dst);
}

/* Apply namespace sandbox in the child.
 * unshare(USER|MNT|PID|NET), write uid/gid maps, pivot_root into minimal fs.
 * System dirs mounted ro, workspace rw, cwd_path rw (CLI mode).
 * Returns 0 on success, -1 on failure. */
static int sandbox_apply_namespace(const char *workspace, const char *cwd_path,
                                   const char *db_path,
                                   const SandboxConfig *full_cfg) {
    uid_t uid = getuid();
    gid_t gid = getgid();

    /* Resolve paths before pivot_root changes the root */
    char ws_abs[PATH_MAX];
    const char *ws_resolved = NULL;
    if (workspace && workspace[0]) {
        if (realpath(workspace, ws_abs))
            ws_resolved = ws_abs;
    }

    char cwd_abs[PATH_MAX];
    const char *cwd_resolved = NULL;
    if (cwd_path && cwd_path[0]) {
        if (realpath(cwd_path, cwd_abs))
            cwd_resolved = cwd_abs;
    }

    int clone_flags = CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWNET;
    if (!full_cfg || !full_cfg->skip_pid_ns)
        clone_flags |= CLONE_NEWPID;
    if (unshare(clone_flags) != 0)
        return -1;

    /* Write uid_map: map ns root (0) to our real uid */
    int fd = open("/proc/self/uid_map", O_WRONLY);
    if (fd < 0) return -1;
    char map[64];
    int len = snprintf(map, sizeof(map), "0 %u 1\n", uid);
    int ok = (write(fd, map, (size_t)len) == len);
    close(fd);
    if (!ok) return -1;

    /* Deny setgroups (required before writing gid_map on unprivileged) */
    fd = open("/proc/self/setgroups", O_WRONLY);
    if (fd >= 0) {
        ok = (write(fd, "deny\n", 5) == 5);
        close(fd);
        if (!ok) return -1;
    }

    /* Write gid_map */
    fd = open("/proc/self/gid_map", O_WRONLY);
    if (fd < 0) return -1;
    len = snprintf(map, sizeof(map), "0 %u 1\n", gid);
    ok = (write(fd, map, (size_t)len) == len);
    close(fd);
    if (!ok) return -1;

    /* Make entire mount tree private so changes don't propagate */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
        return -1;

    /* Create tmpfs as new root.
     *
     * Placed under the scratch tree's .ns/ (which the parent created) keyed by
     * pid, so the parent can reap abandoned roots without tracking pids: a live
     * one holds this tmpfs and refuses rmdir, a dead one is empty. The old
     * mkdtemp in /tmp leaked one empty directory per tool call forever, because
     * after pivot_root the child cannot reach it and the parent never knew the
     * generated name. Falls back to mkdtemp when there is no scratch dir. */
    /* Deliberately modest, not PATH_MAX: several buffers below concatenate onto
     * this, and keeping it short keeps those bounded. An over-long scratch path
     * falls back to mkdtemp rather than truncating. */
    char newroot[192];
    int have_root = 0;
    if (full_cfg && full_cfg->tmp_dir && full_cfg->tmp_dir[0]) {
        const char *slash = strrchr(full_cfg->tmp_dir, '/');
        if (slash) {
            int n = snprintf(newroot, sizeof(newroot), "%.*s/.ns/%d",
                             (int)(slash - full_cfg->tmp_dir), full_cfg->tmp_dir,
                             (int)getpid());
            if (n > 0 && (size_t)n < sizeof(newroot) &&
                (mkdir(newroot, 0700) == 0 || errno == EEXIST))
                have_root = 1;
        }
    }
    if (!have_root) {
        snprintf(newroot, sizeof(newroot), "/tmp/.cclaw_ns_XXXXXX");
        if (!mkdtemp(newroot))
            return -1;
    }
    /* 4m, not 1m: this root holds mountpoint stubs *and* the ~200 KB egress
     * interposer (SANDBOX_PRELOAD_PATH), which used to be written into /tmp. */
    if (mount("none", newroot, "tmpfs", MS_NOSUID | MS_NODEV, "size=4m") != 0)
        return -1;

    /* Bind-mount system dirs read-only */
    /* /opt is here because that is where real toolchains land — distro node
     * packages, vendored JDKs, hand-unpacked SDKs. Read-only like the rest, and
     * absent on most systems, where the stat() below skips it. */
    static const char *sys_dirs[] = {
        "/bin", "/usr", "/lib", "/lib64", "/etc", "/proc", "/dev", "/sbin",
        "/opt", NULL
    };
    for (int i = 0; sys_dirs[i]; i++) {
        /* File tier (skip_pid_ns): exclude /proc — no PID namespace backs it,
         * and the host procfs would leak process info. uid_map/gid_map writes
         * happen before pivot_root using the original /proc, so they're fine. */
        if (full_cfg && full_cfg->skip_pid_ns &&
            strcmp(sys_dirs[i], "/proc") == 0)
            continue;
        struct stat st;
        if (stat(sys_dirs[i], &st) != 0) continue;
        char dst[256];
        snprintf(dst, sizeof(dst), "%s%s", newroot, sys_dirs[i]);
        mkdir(dst, 0755);
        if (mount(sys_dirs[i], dst, NULL, MS_BIND | MS_REC, NULL) == 0) {
            /* Non-recursive RO remount of the top bind (recursive RO can't
             * relock host submounts under /dev,/proc in an unprivileged
             * userns; /proc is replaced fresh below). A failure means the dir
             * would be writable — fail closed. */
            if (sandbox_remount_ro(dst) != 0)
                return -1;
        }
    }

    /* /tmp is a bind of the agent's own scratch directory on the host, not a
     * tmpfs of our own. The root tmpfs above stays tiny because it only holds
     * mountpoint stubs, and /tmp is where toolchains write real volume (cc and
     * rustc intermediates, configure scripts, npm staging) — a directory on a
     * 1 MB root broke every build with ENOSPC.
     *
     * Binding the host's own directory means /tmp inherits whatever the host
     * does for temp storage — tmpfs or disk, and the host's tmpfiles.d cleaner
     * — instead of this code inventing a size policy it cannot get right for
     * both a 128 MB SoC and a 16 GB server. It is a *private* directory rather
     * than the host's shared /tmp: that would hand the child every other
     * process's scratch, ssh-agent and gpg-agent sockets included.
     *
     * It persists across calls on purpose: ./configure in one tool call and
     * make in the next need the same /tmp, and the tool-call boundary is
     * invisible to the agent, so wiping it presents as "my files vanished".
     *
     * Failure is non-fatal — /tmp stays a directory on the root tmpfs, which
     * is functional but too small to build in. */
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/tmp", newroot);
    mkdir(tmp_path, 01777);
    if (full_cfg && full_cfg->tmp_dir && full_cfg->tmp_dir[0]) {
        if (mount(full_cfg->tmp_dir, tmp_path, NULL, MS_BIND | MS_REC, NULL) != 0)
            LOG_INFO_("sandbox tmp_bind_fail errno=%d effect=small_tmp", errno);
    }

    /* Bind the broker's per-call proxy UDS (created in the agent folder, outside
     * the agent-visible workspace) onto a fixed path inside the sandbox. Keeps
     * the control-plane socket out of the workspace while still letting the
     * in-netns preload/shim reach the broker over a pathname UDS. Best-effort:
     * a bind failure leaves no socket inside → no network (fail-closed). */
    if (full_cfg && full_cfg->proxy_sock && full_cfg->proxy_sock[0]) {
        char sdst[PATH_MAX];
        int sn = snprintf(sdst, sizeof(sdst), "%s%s", newroot, SANDBOX_PROXY_SOCK_PATH);
        if (sn > 0 && (size_t)sn < sizeof(sdst)) {
            int tfd = open(sdst, O_CREAT | O_WRONLY, 0600);
            if (tfd >= 0) close(tfd);
            mount(full_cfg->proxy_sock, sdst, NULL, MS_BIND, NULL);
        }
    }

    /* Bind-mount workspace rw */
    if (ws_resolved)
        bind_path_into(newroot, ws_resolved, 0);

    /* Bind-mount CWD rw (CLI mode) */
    if (cwd_resolved)
        bind_path_into(newroot, cwd_resolved, 0);

    /* Layer 2: extra bind-mounts from read_path/write_path grants.
     * Canonicalize + dedup (rw wins) + sort shallow→deep so a child mount
     * shadows its parent, independent of grant insertion order. */
    if (full_cfg && full_cfg->extra_mounts && full_cfg->extra_mount_count > 0) {
        size_t n = full_cfg->extra_mount_count;
        SandboxMount *plan = calloc(n, sizeof(*plan));
        if (plan) {
            size_t pn = sandbox_plan_mounts(full_cfg->extra_mounts, n, plan);
            for (size_t i = 0; i < pn; i++)
                bind_path_into(newroot, plan[i].path, plan[i].ro);
            free(plan);
        }
    }

    /* Mask the secret key + DB ciphertext if a bound path (CWD, workspace, or
     * a grant) would otherwise expose them. After binds, before pivot_root. */
    sandbox_mask_state_files(newroot, db_path, full_cfg);

    /* pivot_root into new root */
    char put_old[256];
    snprintf(put_old, sizeof(put_old), "%s/.oldroot", newroot);
    mkdir(put_old, 0755);
    if (syscall(SYS_pivot_root, newroot, put_old) != 0)
        return -1;

    chdir("/");
    umount2("/.oldroot", MNT_DETACH);
    rmdir("/.oldroot");

    /* CLONE_NEWPID requires a fork — the child becomes PID 1 in the new
     * PID namespace. Mount setup is already done, so the fork+exec is clean.
     * When skip_pid_ns is set (file tier), no PID NS was created so no fork. */
    if (!full_cfg || !full_cfg->skip_pid_ns) {
        pid_t inner = fork();
        if (inner < 0) return -1;
        if (inner > 0) {
            /* Parent of inner fork: wait and propagate exit status */
            int st;
            waitpid(inner, &st, 0);
            _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 254);
        }

        /* PID 1 in new namespace: remount /proc for correct PID view */
        mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);

        /* Tie this PID-namespace init to its parent (the sandbox child). If the
         * parent is killed — e.g. the daemon's backstop SIGKILLs the broker and
         * PR_SET_PDEATHSIG cascades down — this init dies too, and the kernel
         * tears down the whole PID namespace with it, leaving no orphaned shell
         * subtree. PR_SET_PDEATHSIG is cleared across fork, so it must be re-set
         * here on the inner child, not inherited from the broker. */
        prctl(PR_SET_PDEATHSIG, SIGKILL);
    }

    /* CWD into workspace so commands start there */
    if (ws_resolved && chdir(ws_resolved) != 0)
        chdir("/tmp");

    return 0;
}

/* Resolve HOME for the tool child.
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
static const char *sandbox_resolve_home(const SandboxConfig *cfg) {
    if (cfg->sandbox) {
        if (cfg->workspace && cfg->workspace[0]) return cfg->workspace;
        return "/tmp";
    }
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir && pw->pw_dir[0]) return pw->pw_dir;
    if (cfg->workspace && cfg->workspace[0]) return cfg->workspace;
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
static void sandbox_scrub_env(const SandboxConfig *cfg) {
    extern char **environ;
    const char *home = sandbox_resolve_home(cfg);
    if (cfg->env_mode == 1) {
        /* Clean allowlist: wipe entire env, set only essentials */
        if (environ) environ[0] = NULL;  /* portable clearenv */
        sandbox_set_path(home);
        setenv("TMPDIR", "/tmp", 1);
        setenv("HOME", home, 1);
        return;
    }

    /* Legacy trusted mode: inherit env minus known secret names */
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

/* Bring the loopback interface up inside the netns. `lo` exists but is DOWN in
 * a fresh CLONE_NEWNET; we hold CAP_NET_ADMIN as root of the user namespace, so
 * SIOCSIFFLAGS is permitted. Returns 0 on success, -1 otherwise. */
static int sandbox_bring_up_lo(void) {
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "lo", IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFFLAGS, &ifr) != 0) { close(s); return -1; }
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    int rc = ioctl(s, SIOCSIFFLAGS, &ifr);
    close(s);
    return rc;
}

/* Static-binary egress: bring up lo, bind a loopback HTTP CONNECT listener, and
 * fork the link-isolated net_shim to serve it from /tmp. Static clients that
 * honor HTTP_PROXY then tunnel through the shim → the same per-call UDS the
 * preload uses, with the broker still the sole policy authority.
 *
 * Best-effort: any failure simply leaves the proxy env unset, so static
 * binaries stay networkless (the safe pre-feature default) while dynamic
 * binaries keep working via the preload. Runs as PID 1, before the exec. */
static void sandbox_setup_static_egress(const char *uds_path) {
    if (sandbox_bring_up_lo() != 0) return;

    /* Bind 127.0.0.1:0 — the shim's HTTP CONNECT listener. Not CLOEXEC: the
     * forked shim must keep it across its exec. */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) return;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(lfd, 16) != 0) { close(lfd); return; }
    socklen_t alen = sizeof(addr);
    if (getsockname(lfd, (struct sockaddr *)&addr, &alen) != 0) { close(lfd); return; }
    int port = ntohs(addr.sin_port);

    /* Materialize the shim in an anonymous memfd — no on-disk artifact and no
     * dependency on /tmp being exec-able. Not MFD_CLOEXEC: the fork must keep it
     * across exec (it execs /proc/self/fd/<mfd>). */
    int mfd = memfd_create("net_shim", 0);
    if (mfd < 0) { close(lfd); return; }
    ssize_t wr = write(mfd, net_shim_blob, (size_t)net_shim_blob_len);
    if (wr != (ssize_t)net_shim_blob_len) { close(mfd); close(lfd); return; }

    pid_t shim = fork();
    if (shim < 0) { close(mfd); close(lfd); return; }
    if (shim == 0) {
        /* No PID namespace backs this tier (skip_pid_ns), so there's no
         * kernel-enforced teardown when the tool process exits — without this,
         * the shim orphans to init and lingers forever. PDEATHSIG doesn't
         * survive fork(), so it must be set here, not inherited from the
         * sandboxed process that forked us (same reasoning as the PID-ns
         * init's re-arm, sandbox_apply_namespace above). */
        prctl(PR_SET_PDEATHSIG, SIGKILL);

        /* Shim: drop stdout/stderr (they point at the tool result pipe — the
         * shim must not pollute tool output nor hold the pipe open), keep lfd,
         * exec the link-isolated binary with the listener fd + UDS path. */
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); dup2(devnull, STDOUT_FILENO);
                            dup2(devnull, STDERR_FILENO); if (devnull > 2) close(devnull); }
        char fdbuf[16];
        snprintf(fdbuf, sizeof(fdbuf), "%d", lfd);
        /* fexecve (execveat AT_EMPTY_PATH under the hood on Linux ≥3.19) execs
         * the memfd WITHOUT touching /proc — the skip_pid_ns network tiers (web)
         * have no /proc, so an execl("/proc/self/fd/N") would ENOENT and the shim
         * would die, closing the listener (curl → connection refused). */
        char *const argv[] = { "net_shim", fdbuf, (char *)uds_path, NULL };
        extern char **environ;
        fexecve(mfd, argv, environ);
        _exit(127);
    }

    close(mfd);

    /* PID 1: the shim owns the listener now; advertise it to the command. The
     * command never sees lfd (closed here, and it is not CLOEXEC-safe to leak). */
    close(lfd);
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d", port);
    setenv("HTTP_PROXY", url, 1);  setenv("http_proxy", url, 1);
    setenv("HTTPS_PROXY", url, 1); setenv("https_proxy", url, 1);
    setenv("ALL_PROXY", url, 1);   setenv("all_proxy", url, 1);
    setenv("NO_PROXY", "localhost,127.0.0.1,::1", 1);
    setenv("no_proxy", "localhost,127.0.0.1,::1", 1);
}

int sandbox_child_setup(const SandboxConfig *cfg) {
    const char *ws = cfg->workspace;
    const char *cwd = cfg->cwd_path;
    const char *psock = cfg->proxy_sock;

    /* Sandbox profile: suppress CWD mount / proxy per policy */
    if (!cfg->mount_cwd) cwd = NULL;
    if (cfg->net_mode) psock = NULL;

    /* namespace sandbox — fail closed if requested but unavailable */
    if (cfg->sandbox && sandbox_apply_namespace(ws, cwd, cfg->db_path, cfg) != 0) {
        int e = errno;  /* log call below may clobber it */
        LOG_INFO_("sandbox namespace_fail errno=%d action=abort", e);
        fprintf(stderr, "error: namespace sandbox unavailable (errno=%d); "
                "this sandbox profile requires it — enable unprivileged user "
                "namespaces or set the agent's sandbox_profile to 'host'\n", e);
        return -1;
    }

    /* Sandbox profile: remount workspace read-only after pivot_root */
    if (cfg->sandbox && cfg->workspace_ro && ws) {
        char ws_real[PATH_MAX];
        if (realpath(ws, ws_real) == NULL ||
            mount(NULL, ws_real, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL) != 0) {
            fprintf(stderr, "error: read-only workspace remount failed "
                    "(errno=%d); refusing to run writable\n", errno);
            return -1;
        }
    }

    /* host has no namespace, so sandbox_apply_namespace's chdir never ran. In
     * CLI mode cwd_path is the user's own CWD and inheriting it is the whole
     * point of host — `cclaw` in a repo operates on that repo. Under --daemon
     * cwd_path is NULL and the daemon's CWD is meaningless to the agent, so
     * start in the workspace instead. */
    if (!cfg->sandbox && !cwd && cfg->workspace && cfg->workspace[0]) {
        if (chdir(cfg->workspace) != 0)
            LOG_INFO_("sandbox host_chdir_fail errno=%d", errno);
    }

    sandbox_scrub_env(cfg);

    /* the proxy UDS is reachable at a fixed in-sandbox path (bound from the
     * broker's agent-folder socket in sandbox_apply_namespace). */
    if (psock) setenv("CCLAW_PROXY_SOCK", SANDBOX_PROXY_SOCK_PATH, 1);

    sandbox_apply_rlimits(cfg);

    if (psock && cfg->sandbox) {
        /* LD_PRELOAD interposer: only for the shell tier (PID-ns present, an
         * untrusted subprocess tree that dials network itself). The skip_pid_ns
         * network tiers (web/js) run OUR own libcurl, which honors HTTP_PROXY →
         * net_shim (spec §4: "no preload" for web). Loading the preload there
         * would intercept curl's loopback connect to the shim and fight the
         * HTTP_PROXY path, so it is gated off. The interposer is the only egress
         * inside CLONE_NEWNET for shell, so a failed write must be loud. */
        if (!cfg->skip_pid_ns) {
            /* O_NOFOLLOW: refuse to follow a symlink at this path rather than
             * writing 0755 content wherever it points. */
            int pfd = open(SANDBOX_PRELOAD_PATH,
                           O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0755);
            ssize_t wr = pfd >= 0 ? write(pfd, preload_net_blob, (size_t)preload_net_blob_len) : -1;
            if (pfd >= 0) close(pfd);
            if (wr == (ssize_t)preload_net_blob_len)
                setenv("LD_PRELOAD", SANDBOX_PRELOAD_PATH, 1);
            else {
                int e = errno;  /* log call below may clobber it */
                LOG_INFO_("sandbox preload_fail errno=%d effect=no_network", e);
                fprintf(stderr, "[cclaw] warning: proxy preload setup failed "
                        "(errno=%d); child has no network\n", e);
            }
        }

        /* net_shim: a loopback HTTP CONNECT proxy forwarding to the broker UDS.
         * Used by web/js's own curl (via HTTP_PROXY) and by shell's static
         * binaries (which ignore LD_PRELOAD). Best-effort. */
        sandbox_setup_static_egress(SANDBOX_PROXY_SOCK_PATH);
    }

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
        cfg->env_mode = 0; cfg->net_mode = 0; cfg->mount_cwd = 1; cfg->workspace_ro = 0;
        cfg->rlimits.nproc = 0; cfg->rlimits.as_mb = 0; cfg->rlimits.cpu_sec = 0;
    } else if (sandbox_profile && strcmp(sandbox_profile, "trusted") == 0) {
        /* Resources unbounded, visibility is not: no CWD mount, so the agent
         * sees its own workspace and nothing of the user's files or any other
         * agent's — additional paths arrive only as read_path/write_path
         * grants. "Trusted" is about resources, never about reach. */
        cfg->sandbox = 1;
        cfg->env_mode = 0; cfg->net_mode = 0; cfg->mount_cwd = 0; cfg->workspace_ro = 0;
        cfg->rlimits.nproc = 0; cfg->rlimits.as_mb = 0; cfg->rlimits.cpu_sec = 0;
    } else if (sandbox_profile && strcmp(sandbox_profile, "restricted") == 0) {
        /* Bounded, but still able to run real programs: the limits are a
         * runaway backstop, not a straitjacket. The workspace stays rw so
         * $HOME works — what makes this restricted is no network at all. */
        cfg->sandbox = 1;
        cfg->env_mode = 1; cfg->net_mode = 1; cfg->mount_cwd = 0; cfg->workspace_ro = 0;
        cfg->rlimits.nproc = 64; cfg->rlimits.as_mb = 0; cfg->rlimits.cpu_sec = 120;
    } else { /* "standard", unknown, NULL */
        cfg->sandbox = 1;
        cfg->env_mode = 1; cfg->net_mode = 0; cfg->mount_cwd = 0; cfg->workspace_ro = 0;
        cfg->rlimits.nproc = 256; cfg->rlimits.as_mb = 0; cfg->rlimits.cpu_sec = 1800;
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
    p->workspace_ro = c.workspace_ro;
    p->rlimits.nproc   = c.rlimits.nproc;
    p->rlimits.as_mb   = c.rlimits.as_mb;
    p->rlimits.cpu_sec = c.rlimits.cpu_sec;
}
