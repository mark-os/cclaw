#define _GNU_SOURCE
/* Containment backend: Linux namespaces. Everything that needs unshare,
 * mount, pivot_root or a network namespace lives here; the policy that decides
 * what a child may see lives in sandbox.c. */
#include "contain.h"
#include "log.h"
#include "preload_blob.h"
#include "net_shim_blob.h"
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
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

const char *contain_backend(void) { return "linux-ns"; }

/* The widest set the sandbox ever asks for (the shell tier; file/web/js omit
 * the PID namespace). Probing a narrower set would call a host usable that
 * then refuses the shell. */
#define CONTAIN_NS_FLAGS (CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWPID)

int contain_probe(char *why, size_t cap) {
    if (why && cap) why[0] = '\0';
    pid_t pid = fork();
    if (pid < 0) {
        if (why) snprintf(why, cap, "fork: %s", strerror(errno));
        return 0;
    }
    if (pid == 0) {
        /* errno rides home in the exit status; 0 is reserved for success. */
        if (unshare(CONTAIN_NS_FLAGS) == 0) _exit(0);
        _exit(errno > 0 && errno < 256 ? errno : 255);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno == EINTR) continue;
        if (why) snprintf(why, cap, "waitpid: %s", strerror(errno));
        return 0;
    }
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return 1;
    if (why) {
        if (WIFEXITED(st) && WEXITSTATUS(st) != 255)
            snprintf(why, cap, "unshare(user, mount, network, pid): %s",
                     strerror(WEXITSTATUS(st)));
        else
            snprintf(why, cap, "namespace probe child failed");
    }
    return 0;
}

/* Fixed in-sandbox path the broker's per-call proxy UDS is bind-mounted to. The
 * socket itself lives in the agent folder on the host (outside the workspace);
 * inside the netns the preload lib and net_shim reach it here. */
#define SANDBOX_PROXY_SOCK_PATH "/tmp/.cclaw_proxy.sock"
#define SANDBOX_LLM_SOCK_PATH   "/tmp/.cclaw_llm.sock"

/* The egress interposer lives on the root tmpfs, not in /tmp. /tmp is now a
 * persistent bind of the agent's own scratch directory, so anything we drop
 * there survives the call and is the agent's to modify; the root tmpfs is
 * rebuilt per call and holds only our plumbing. (Tampering was never an egress
 * bypass — CLONE_NEWNET has no route out, so a disabled interposer is
 * self-inflicted denial of service — but our machinery does not belong in the
 * agent's scratch space.) */
#define SANDBOX_PRELOAD_PATH "/.cclaw_net.so"

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
 * One rule for both, on purpose — no grant can unmask either:
 *   key  — reading it is not access to a resource, it is escalation to every
 *          secret in the DB, including system-scope ones and other agents'. A
 *          grant made on one agent's behalf must not confer authority over
 *          agents that never consented.
 *   DB   — it is the policy store for the mechanism doing the containment
 *          (`grants`, `agents.sandbox_profile`), so reading it is
 *          reconnaissance and writing it *is* escalation. It is also not one
 *          agent's state: it holds every agent's sessions, entries, grants and
 *          approvals. Nothing breaks — db_query is the sanctioned read path
 *          and runs in the trusted parent, never through this mount tree.
 *
 * There is no escape hatch here by design: a grant naming the DB (or a
 * directory containing it) is refused at *grant* time (grant_path_hits_db,
 * src/agent_config.c), so an approved grant is never silently inert at mount.
 *
 * Targets not reachable in the child's mount tree never materialize under
 * newroot, so the stat fails and we skip them — already invisible by omission.
 * Must run after all binds and before pivot_root. */
static void sandbox_mask_state_files(const char *newroot, const char *db_path) {
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

    /* Targets: the key file (crown jewel) and the DB family (ciphertext plus
     * its -wal/-shm siblings) — all unconditional. */
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
    targets[nt++] = db_abs;
    targets[nt++] = dbwal;
    targets[nt++] = dbshm;

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

/* Which read-only mount swallowed this mount point? Walks the target's
 * ancestors under newroot outward and keeps the last one statvfs reports
 * read-only — that is the root of the read-only bind (a read grant, or a
 * system dir). Fills out with the host-side path (the newroot prefix stripped)
 * and returns 1; returns 0 when no ancestor is read-only. */
static int ro_mount_above(const char *newroot, const char *dst,
                          char *out, size_t outsz) {
    char cur[PATH_MAX + 64];
    snprintf(cur, sizeof(cur), "%s", dst);
    size_t rootlen = strlen(newroot);
    int found = 0;
    for (;;) {
        char *slash = strrchr(cur, '/');
        if (!slash || (size_t)(slash - cur) <= rootlen) break;
        *slash = '\0';
        struct statvfs vfs;
        if (statvfs(cur, &vfs) != 0 || !(vfs.f_flag & ST_RDONLY)) break;
        snprintf(out, outsz, "%s", cur + rootlen);
        found = 1;
    }
    return found;
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
        if (fd < 0) {
            /* No mount point, no mount. The usual cause is a nested grant: the
             * file sits inside a directory grant already mounted read-only, so
             * creating its mount point is EROFS. Fail open — like every other
             * grant-mount failure here, a grant that does not mount narrows
             * what the child can reach and never widens it, so refusing to run
             * would trade a lost grant for a dead tool — but say so on both
             * channels: syslog for the operator, stderr for the agent (it is
             * part of the tool result wherever child output is captured).
             * Silence here is the "approved grant that does nothing" bug. */
            int e = errno;
            char ro_dir[PATH_MAX] = "";
            int nested = ro_mount_above(newroot, dst, ro_dir, sizeof(ro_dir));
            LOG_INFO_("sandbox grant_mountpoint_fail path=%s errno=%d ro_parent=%s",
                      abspath, e, nested ? ro_dir : "-");
            if (nested)
                fprintf(stderr, "[cclaw] warning: file grant '%s' is inside "
                        "read-only mount '%s' — mount skipped, that grant is "
                        "NOT in effect\n", abspath, ro_dir);
            else
                fprintf(stderr, "[cclaw] warning: file grant '%s' could not be "
                        "mounted (mount point create failed, errno=%d) — that "
                        "grant is NOT in effect\n", abspath, e);
            return;
        }
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
int contain_enter(const char *workspace, const char *cwd_path,
                  const char *db_path, const SandboxConfig *full_cfg) {
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

    /* Identity uid_map: the payload keeps the invoking user's uid, not ns
     * root. Fake root confused models (apt/sudo attempts, npm root-mode) and
     * granted the payload in-ns privileges it shouldn't have (low ports,
     * mounts). Namespace setup below still works: the ns *creator* holds
     * full caps in it regardless of the map; caps drop only at execve. */
    int fd = open("/proc/self/uid_map", O_WRONLY);
    if (fd < 0) return -1;
    char map[64];
    int len = snprintf(map, sizeof(map), "%u %u 1\n", uid, uid);
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

    /* Identity gid_map, same reasoning */
    fd = open("/proc/self/gid_map", O_WRONLY);
    if (fd < 0) return -1;
    len = snprintf(map, sizeof(map), "%u %u 1\n", gid, gid);
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

    /* Same treatment for the parent's per-call LLM bridge socket (LLM()). */
    if (full_cfg && full_cfg->llm_sock && full_cfg->llm_sock[0]) {
        char ldst[PATH_MAX];
        int ln = snprintf(ldst, sizeof(ldst), "%s%s", newroot, SANDBOX_LLM_SOCK_PATH);
        if (ln > 0 && (size_t)ln < sizeof(ldst)) {
            int tfd = open(ldst, O_CREAT | O_WRONLY, 0600);
            if (tfd >= 0) close(tfd);
            mount(full_cfg->llm_sock, ldst, NULL, MS_BIND, NULL);
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
    sandbox_mask_state_files(newroot, db_path);

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

/* Bring the loopback interface up inside the netns. `lo` exists but is DOWN in
 * a fresh CLONE_NEWNET; as creator of the user namespace we hold CAP_NET_ADMIN
 * over it (regardless of the identity uid_map — caps drop only at execve), so
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

/* Give the contained child its only route out: the broker's proxy socket,
 * bound at a fixed in-boundary path by contain_enter. Runs after the env
 * scrub. A failure here leaves the child with no network, never with an
 * unfiltered one. */
void contain_egress(const SandboxConfig *cfg) {
    setenv("CCLAW_PROXY_SOCK", SANDBOX_PROXY_SOCK_PATH, 1);

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

const char *contain_llm_sock_path(void) { return SANDBOX_LLM_SOCK_PATH; }
