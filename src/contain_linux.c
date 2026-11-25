#define _GNU_SOURCE
/* Containment backend: Linux namespaces. Everything that needs unshare,
 * mount, pivot_root or a network namespace lives here; the policy that decides
 * what a child may see lives in sandbox.c. */
#include "contain.h"
#include "log.h"
#include "net_shim_blob.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
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
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

const char *contain_backend(void) { return "linux-ns"; }

/* The widest set the sandbox ever asks for (the shell; the file and js
 * children omit the PID namespace). Probing a narrower set would call a host usable that
 * then refuses the shell. */
#define CONTAIN_NS_FLAGS (CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWIPC | \
                          CLONE_NEWPID)

/* One line into the probe's pipe: which step failed and why. */
static void probe_fail(int fd, const char *step) {
    char msg[160];
    int n = snprintf(msg, sizeof(msg), "%s: %s", step, strerror(errno));
    if (n > 0 && write(fd, msg, (size_t)n) < 0) { /* the parent reports a silent child */ }
    _exit(1);
}

static int probe_write(const char *path, const char *text) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t n = (ssize_t)strlen(text);
    int ok = write(fd, text, (size_t)n) == n;
    close(fd);
    return ok ? 0 : -1;
}

static int sandbox_bring_up_lo(void);

/* Map the invoking user and group to themselves in a fresh user namespace
 * (setgroups denied first, as an unprivileged gid_map requires). */
static int map_identity(uid_t uid, gid_t gid) {
    char map[64];
    snprintf(map, sizeof(map), "%u %u 1\n", (unsigned)uid, (unsigned)uid);
    if (probe_write("/proc/self/uid_map", map) != 0) return -1;
    if (access("/proc/self/setgroups", F_OK) == 0 &&
        probe_write("/proc/self/setgroups", "deny\n") != 0) return -1;
    snprintf(map, sizeof(map), "%u %u 1\n", (unsigned)gid, (unsigned)gid);
    return probe_write("/proc/self/gid_map", map);
}

/* Landlock and seccomp proven in a forked child, in both directions: what is
 * allowed works and what is not is refused, through the same functions a tool
 * child uses. Returns 1 if the child saw both. */
static int probe_enforcer(unsigned cap) {
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) _exit(1);
        int sp[2], fd;
        if (cap == CONTAIN_CAP_LANDLOCK) {
            static const SandboxMount devnull = { "/dev/null", CONTAIN_LL_DEV };
            if (contain_landlock(&devnull, 1) != 0) _exit(1);
            if ((fd = open("/dev/null", O_RDWR)) < 0) _exit(1);
            _exit(open("/", O_RDONLY | O_DIRECTORY) < 0 && errno == EACCES ? 0 : 1);
        }
        if (contain_seccomp(CONTAIN_SC_NOSOCKET) != 0) _exit(1);
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) _exit(1);
        _exit(socket(AF_UNIX, SOCK_STREAM, 0) < 0 && errno == EACCES ? 0 : 1);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR) return 0;
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

#ifndef __NR_pidfd_open
#define __NR_pidfd_open  434   /* unified numbers, as Landlock's */
#endif
#ifndef __NR_pidfd_getfd
#define __NR_pidfd_getfd 438
#endif

/* NOTIF, by doing what a call does: a child installs the connect filter with
 * its flags (EINVAL before 5.19), and this process takes the child's listener
 * with pidfd_getfd and reads the child's memory — the two steps the broker
 * takes on every connect, which a container's own seccomp profile may refuse
 * (Docker's default does pidfd_getfd without CAP_SYS_PTRACE). */
static volatile uint32_t g_probe_cookie = 0x6e6f7466;
static int probe_notif(void) {
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sp) != 0) return 0;
    pid_t pid = fork();
    if (pid < 0) { close(sp[0]); close(sp[1]); return 0; }
    if (pid == 0) {
        int l = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0
                ? contain_seccomp(CONTAIN_SC_CONNECT) : -1;
        char c;
        if (write(sp[1], &l, sizeof(l)) == sizeof(l) && read(sp[1], &c, 1) == 1) _exit(0);
        _exit(1);
    }
    close(sp[1]);
    int l = -1, ok = 0;
    if (read(sp[0], &l, sizeof(l)) == sizeof(l) && l >= 0) {
        int pfd = (int)syscall(__NR_pidfd_open, pid, 0);
        int got = pfd >= 0 ? (int)syscall(__NR_pidfd_getfd, pfd, l, 0) : -1;
        uint32_t v = 0;
        struct iovec mine = { &v, sizeof(v) }, theirs = { (void *)&g_probe_cookie, sizeof(v) };
        ok = got >= 0 && process_vm_readv(pid, &mine, 1, &theirs, 1, 0) == sizeof(v) &&
             v == g_probe_cookie;
        if (got >= 0) close(got);
        if (pfd >= 0) close(pfd);
    }
    if (write(sp[0], "x", 1) != 1) ok = 0;
    close(sp[0]);
    int st;
    while (waitpid(pid, &st, 0) < 0)
        if (errno != EINTR) return 0;
    return ok;
}

static long landlock_abi_now(void);
static int g_landlock_abi;

/* Does what the sandbox does first, not only what it asks for first: creates
 * the namespaces, maps the user, then takes the first step that needs a
 * capability inside them — making the mount tree private (MOUNTNS) or bringing
 * up lo (NETNS). Creating alone is not the test: Ubuntu's AppArmor restriction
 * lets an unprivileged process create a user namespace and then denies it
 * every capability in it, so unshare() succeeds and the first mount() fails.
 * A probe that stopped at unshare() called such a host contained, and every
 * tool call then refused (seen on a real Ubuntu 24.04 kernel in CI,
 * 2026-11-23). Returns 1 if the child got through. */
static int probe(int flags, int mount_step, char *why, size_t cap) {
    if (why && cap) why[0] = '\0';
    int pfd[2];
    if (pipe(pfd) != 0) {
        if (why) snprintf(why, cap, "pipe: %s", strerror(errno));
        return 0;
    }
    uid_t uid = getuid();
    gid_t gid = getgid();
    pid_t pid = fork();
    if (pid < 0) {
        if (why) snprintf(why, cap, "fork: %s", strerror(errno));
        close(pfd[0]); close(pfd[1]);
        return 0;
    }
    if (pid == 0) {
        close(pfd[0]);
        char map[64];
        if (unshare(flags) != 0)
            probe_fail(pfd[1], mount_step ? "unshare(user, mount, network, ipc, pid)"
                                          : "unshare(user, network)");
        snprintf(map, sizeof(map), "%u %u 1\n", (unsigned)uid, (unsigned)uid);
        if (probe_write("/proc/self/uid_map", map) != 0)
            probe_fail(pfd[1], "writing the user namespace's uid_map");
        if (access("/proc/self/setgroups", F_OK) == 0 &&
            probe_write("/proc/self/setgroups", "deny\n") != 0)
            probe_fail(pfd[1], "writing the user namespace's setgroups");
        snprintf(map, sizeof(map), "%u %u 1\n", (unsigned)gid, (unsigned)gid);
        if (probe_write("/proc/self/gid_map", map) != 0)
            probe_fail(pfd[1], "writing the user namespace's gid_map");
        if (mount_step && mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
            probe_fail(pfd[1], "mount inside the new namespace");
        if (!mount_step && sandbox_bring_up_lo() != 0)
            probe_fail(pfd[1], "bringing up lo in the new network namespace");
        _exit(0);
    }
    close(pfd[1]);
    char msg[160];
    ssize_t n;
    while ((n = read(pfd[0], msg, sizeof(msg) - 1)) < 0 && errno == EINTR) {}
    close(pfd[0]);
    msg[n > 0 ? n : 0] = '\0';
    int st;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno == EINTR) continue;
        if (why) snprintf(why, cap, "waitpid: %s", strerror(errno));
        return 0;
    }
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return 1;
    if (why) snprintf(why, cap, "%s", msg[0] ? msg : "namespace probe child failed");
    return 0;
}

unsigned contain_caps(char *why, size_t cap) {
    static int probed;
    static unsigned caps;
    static char reason[160];
    if (!probed) {
        caps = CONTAIN_CAP_SHIM;
        if (probe(CONTAIN_NS_FLAGS, 1, reason, sizeof(reason))) caps |= CONTAIN_CAP_MOUNTNS;
        if (probe(CLONE_NEWUSER | CLONE_NEWNET, 0, NULL, 0)) caps |= CONTAIN_CAP_NETNS;
        if (probe_enforcer(CONTAIN_CAP_LANDLOCK)) {
            caps |= CONTAIN_CAP_LANDLOCK;
            g_landlock_abi = (int)landlock_abi_now();
        }
        if (probe_enforcer(CONTAIN_CAP_SECCOMP)) caps |= CONTAIN_CAP_SECCOMP;
        if ((caps & CONTAIN_CAP_SECCOMP) && probe_notif()) caps |= CONTAIN_CAP_NOTIF;
        probed = 1;
    }
    unsigned out = caps;
    const char *mask = getenv("CCLAW_CONTAIN_CAPS_MASK");
    if (mask && mask[0]) out &= (unsigned)strtoul(mask, NULL, 0);
    if (why && cap)
        snprintf(why, cap, "%s", (out & CONTAIN_CAP_MOUNTNS) ? ""
                 : (caps & CONTAIN_CAP_MOUNTNS) ? "masked by CCLAW_CONTAIN_CAPS_MASK"
                 : reason);
    return out;
}

int contain_landlock_abi(void) { contain_caps(NULL, 0); return g_landlock_abi; }

/* A sysctl's integer value, or -1 when this kernel does not have it. */
static long sysctl_long(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long v = -1;
    if (fscanf(f, "%ld", &v) != 1) v = -1;
    fclose(f);
    return v;
}

/* The switches distributions use to turn unprivileged user namespaces off,
 * most specific first. Each is named only when it is set against us. */
void contain_hint(char *out, size_t cap) {
    if (!out || !cap) return;
    out[0] = '\0';
    if (sysctl_long("/proc/sys/kernel/apparmor_restrict_unprivileged_userns") == 1)
        snprintf(out, cap, "AppArmor restricts unprivileged user namespaces here"
                 " (Ubuntu 23.10 and later). To allow them:"
                 " sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0");
    else if (sysctl_long("/proc/sys/kernel/unprivileged_userns_clone") == 0)
        snprintf(out, cap, "unprivileged user namespaces are switched off here."
                 " To allow them: sudo sysctl -w kernel.unprivileged_userns_clone=1");
    else if (sysctl_long("/proc/sys/user/max_user_namespaces") == 0)
        snprintf(out, cap, "user namespaces are limited to zero here."
                 " To allow them: sudo sysctl -w user.max_user_namespaces=15000");
    else if (access("/.dockerenv", F_OK) == 0 || access("/run/.containerenv", F_OK) == 0)
        snprintf(out, cap, "this is inside a container, and its runtime does not"
                 " let a process make namespaces of its own");
}

/* Fixed in-sandbox path the broker's per-call proxy UDS is bind-mounted to. The
 * socket itself lives in the agent folder on the host (outside the workspace);
 * inside the netns net_shim reaches it here. */
#define SANDBOX_PROXY_SOCK_PATH "/tmp/.cclaw_proxy.sock"
#define SANDBOX_LLM_SOCK_PATH   "/tmp/.cclaw_llm.sock"

/* net_shim as a file the shell can run by name (`connect` mode, for
 * ssh's ProxyCommand). It lives on the root tmpfs, not in /tmp: /tmp is a
 * persistent bind of the agent's own scratch directory, so anything we drop
 * there survives the call and is the agent's to modify; the root tmpfs is
 * rebuilt per call and holds only our plumbing. (Tampering is never an egress
 * bypass — CLONE_NEWNET has no route out and the broker decides every
 * connection — but our machinery does not belong in the agent's scratch.) */
#define SANDBOX_CONNECT_PATH "/.cclaw_connect"

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
 *          (`grants`, `agents.containment`), so reading it is
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

/* Undo mountinfo's octal escapes (\040 space, \011 tab, \012 newline,
 * \134 backslash) in place. */
static void mountinfo_unescape(char *s) {
    char *w = s;
    for (char *r = s; *r; ) {
        if (r[0] == '\\' && r[1] >= '0' && r[1] <= '3' && r[2] >= '0' && r[2] <= '7' &&
            r[3] >= '0' && r[3] <= '7') {
            *w++ = (char)(((r[1] - '0') << 6) | ((r[2] - '0') << 3) | (r[3] - '0'));
            r += 4;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* Make every mount strictly below `dst` read-only, one by one. A recursive
 * bind of a system directory carries the host's submounts along (/dev/shm,
 * /dev/mqueue, /dev/pts under /dev; a container runtime's resolv.conf under
 * /etc), and the read-only remount of the top bind does not reach them: they
 * stayed the host's, writable — a channel between agents and to the host.
 * A recursive read-only remount is refused in an unprivileged user namespace,
 * but remounting each one individually with its locked flags preserved is
 * allowed (sandbox_remount_ro). Generic on purpose: whatever this host mounts
 * under a system directory, nothing of it stays writable.
 *
 * autofs mount points are skipped: only their daemon can create entries in
 * one, and touching an untriggered one would ask a daemon in another mount
 * namespace to mount it. Returns 0, or -1 if any remount failed. */
static int sandbox_lock_submounts(const char *dst) {
    FILE *f = fopen("/proc/self/mountinfo", "re");
    if (!f) return -1;
    size_t dlen = strlen(dst);
    int rc = 0;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        /* id parent maj:min root mountpoint options [optional...] - fstype ... */
        char mp[PATH_MAX], fstype[64];
        if (sscanf(line, "%*s %*s %*s %*s %4095s", mp) != 1) continue;
        const char *sep = strstr(line, " - ");
        if (!sep || sscanf(sep + 3, "%63s", fstype) != 1) continue;
        mountinfo_unescape(mp);
        if (strncmp(mp, dst, dlen) != 0 || mp[dlen] != '/') continue;
        if (strcmp(fstype, "autofs") == 0) continue;
        if (sandbox_remount_ro(mp) != 0) {
            LOG_INFO_("sandbox submount_ro_fail path=%s errno=%d", mp, errno);
            rc = -1;
        }
    }
    fclose(f);
    return rc;
}

/* The writable parts of /dev that programs expect, each replaced by an
 * instance of its own: POSIX shared memory (/dev/shm), message queues (the
 * mount for this child's own IPC namespace) and pseudo-terminals (a new devpts
 * instance, which /dev/ptmx then allocates from). They work inside the call
 * and are gone with it; the host's stay out of reach. Each is mounted only
 * where the host has the directory; a failure is fatal to the call, since the
 * alternative is the host's instance or nothing. */
static int sandbox_private_dev(const char *newroot) {
    static const struct { const char *path, *fstype, *opts; unsigned long flags; } dev[] = {
        { "/dev/shm",    "tmpfs",  "mode=1777", MS_NOSUID | MS_NODEV },
        { "/dev/mqueue", "mqueue", NULL,        MS_NOSUID | MS_NODEV | MS_NOEXEC },
        { "/dev/pts",    "devpts", "newinstance,ptmxmode=0666,mode=0620",
                                                MS_NOSUID | MS_NOEXEC },
    };
    for (size_t i = 0; i < sizeof(dev) / sizeof(dev[0]); i++) {
        char dst[256];
        snprintf(dst, sizeof(dst), "%s%s", newroot, dev[i].path);
        struct stat st;
        if (stat(dst, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (mount(dev[i].fstype, dst, dev[i].fstype, dev[i].flags, dev[i].opts) != 0) {
            LOG_INFO_("sandbox private_dev_fail path=%s errno=%d", dev[i].path, errno);
            return -1;
        }
    }
    return 0;
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

/* Transparent: point the namespace's resolver at the DNS stub on
 * 127.0.0.1:53. /etc is the host's, read-only; resolv.conf is replaced by a
 * bind of our own file — or, when it is a symlink (systemd-resolved's
 * /run/...), by our file created where the link points, which in the new
 * root is an empty directory on the root tmpfs. Best-effort: without it a
 * name simply does not resolve, as without the stub. */
static void stub_resolv_conf(const char *newroot) {
    static const char text[] = "nameserver 127.0.0.1\n";
    char conf[256], link[256], dst[512], own[256];
    snprintf(conf, sizeof(conf), "%s/etc/resolv.conf", newroot);
    snprintf(own, sizeof(own), "%s/.cclaw_resolv.conf", newroot);
    ssize_t ln = readlink(conf, link, sizeof(link) - 1);
    if (ln > 0) {
        link[ln] = '\0';   /* resolved inside the new root, never the host's */
        snprintf(dst, sizeof(dst), "%s%s%s", newroot, link[0] == '/' ? "" : "/etc/", link);
    } else {
        snprintf(dst, sizeof(dst), "%s", conf);
    }
    struct stat st;
    int bind = lstat(dst, &st) == 0;
    if (bind && !S_ISREG(st.st_mode)) goto fail;
    if (!bind) {   /* a dangling link: the directories are the root tmpfs's */
        if (ln <= 0 || errno != ENOENT) goto fail;
        for (char *sl = dst + strlen(newroot) + 1; (sl = strchr(sl, '/')); *sl++ = '/') {
            *sl = '\0';
            mkdir(dst, 0755);
        }
    }
    int fd = open(bind ? own : dst, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
    ssize_t w = fd >= 0 ? write(fd, text, sizeof(text) - 1) : -1;
    if (fd >= 0) close(fd);
    if (w == (ssize_t)sizeof(text) - 1 &&
        (!bind || (mount(own, dst, NULL, MS_BIND, NULL) == 0 && sandbox_remount_ro(dst) == 0)))
        return;
fail:
    LOG_INFO_("sandbox resolv_stub_fail errno=%d", errno);
}

/* Apply namespace sandbox in the child.
 * unshare(USER|MNT|PID|NET), write uid/gid maps, pivot_root into minimal fs.
 * System dirs mounted ro, workspace rw.
 * Returns 0 on success, -1 on failure. */
int contain_enter(const char *workspace, const char *db_path,
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

    /* CLONE_NEWIPC: System V IPC and POSIX message queues are keyed by IPC
     * namespace, not by path, so without it a child shares the host's. */
    int clone_flags = CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWNET | CLONE_NEWIPC;
    if (!full_cfg || !full_cfg->skip_pid_ns)
        clone_flags |= CLONE_NEWPID;
    if (unshare(clone_flags) != 0)
        return -1;

    /* Identity map: the payload keeps the invoking user's uid, not ns root.
     * Fake root confused models (apt/sudo attempts, npm root-mode) and
     * granted the payload in-ns privileges it shouldn't have (low ports,
     * mounts). Namespace setup below still works: the ns *creator* holds
     * full caps in it regardless of the map; caps drop only at execve. */
    if (map_identity(uid, gid) != 0)
        return -1;

    /* Make entire mount tree private so changes don't propagate */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
        return -1;

    /* Create tmpfs as new root.
     *
     * Placed under the scratch tree's .ns/ (which the parent created) keyed by
     * pid, so the parent can reap abandoned roots without tracking pids: a live
     * one holds this tmpfs and refuses rmdir, a dead one is empty. Falls back
     * to mkdtemp when there is no scratch dir. Either way the child removes
     * the directory itself right after pivot_root (below); the parent's sweep
     * is for a child that died before getting that far. */
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
    /* 4m, not 1m: this root holds mountpoint stubs *and* a copy of net_shim
     * (SANDBOX_CONNECT_PATH; ~1 MB when statically linked). */
    if (mount("none", newroot, "tmpfs", MS_NOSUID | MS_NODEV, "size=4m") != 0)
        return -1;

    /* Bind-mount system dirs read-only (absent ones are skipped). /proc only
     * for the shell: file and js (skip_pid_ns) have no PID namespace to back
     * it, and the host procfs would leak process info. uid_map/gid_map writes
     * happen before pivot_root using the original /proc, so they're fine. */
    int with_proc = !full_cfg || !full_cfg->skip_pid_ns;
    for (int i = -1; i < 0 || SANDBOX_SYS_DIRS[i]; i++) {
        const char *sys = i < 0 ? "/proc" : SANDBOX_SYS_DIRS[i];
        if (i < 0 && !with_proc) continue;
        struct stat st;
        if (stat(sys, &st) != 0) continue;
        char dst[256];
        snprintf(dst, sizeof(dst), "%s%s", newroot, sys);
        mkdir(dst, 0755);
        if (mount(sys, dst, NULL, MS_BIND | MS_REC, NULL) == 0) {
            /* RO remount of the top bind, then of each host submount the
             * recursive bind brought along (a recursive RO remount is refused
             * in an unprivileged userns). A failure means something would be
             * writable — fail closed. */
            if (sandbox_remount_ro(dst) != 0 || sandbox_lock_submounts(dst) != 0)
                return -1;
        }
    }
    if (sandbox_private_dev(newroot) != 0)
        return -1;
    if (full_cfg && full_cfg->plan.transparent && full_cfg->proxy_sock)
        stub_resolv_conf(newroot);

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
     * in-netns net_shim reaches the broker over a pathname UDS. Best-effort:
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

    /* Mask the secret key + DB ciphertext if a bound path (workspace or a
     * grant) would otherwise expose them. After binds, before pivot_root. */
    sandbox_mask_state_files(newroot, db_path);

    /* pivot_root into new root */
    char put_old[256];
    snprintf(put_old, sizeof(put_old), "%s/.oldroot", newroot);
    mkdir(put_old, 0755);
    if (syscall(SYS_pivot_root, newroot, put_old) != 0)
        return -1;

    if (chdir("/") != 0) return -1;   /* never run with a cwd outside the new root */
    /* The directory the root tmpfs was mounted on is, after the pivot, an
     * ordinary empty directory on the old root: remove it while that is still
     * reachable. Without this the mkdtemp fallback left one empty directory
     * in /tmp per call, forever — nobody else knows the generated name.
     * Best-effort: under .ns/ the parent's sweep covers a miss. */
    char old_stub[sizeof(newroot) + 16];
    snprintf(old_stub, sizeof(old_stub), "/.oldroot%s", newroot);
    rmdir(old_stub);
    umount2("/.oldroot", MNT_DETACH);
    rmdir("/.oldroot");

    /* CLONE_NEWPID requires a fork — the child becomes PID 1 in the new
     * PID namespace. Mount setup is already done, so the fork+exec is clean.
     * When skip_pid_ns is set (file, js), no PID NS was created so no fork. */
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
        /* If this fails the host's /proc bind would stay, showing every
         * host process: refuse instead. */
        if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0)
            _exit(126);

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
        (void)!chdir("/tmp");   /* best effort: any cwd inside the sandbox will do */

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

/* The HTTP proxy every contained program is pointed at: bind a loopback
 * listener and fork the link-isolated net_shim to serve it. A program that
 * honors HTTP_PROXY / HTTPS_PROXY tunnels through the shim → the per-call UDS
 * → the broker, the sole policy authority. In the namespace (in_ns) lo is
 * brought up first, and a program that ignores the variables has no network.
 * On the host's loopback (NE_ENV) nothing stops such a program, and the
 * listener can be reached by any local process for the length of the call:
 * that is what 'advisory' says (under NE_NOTIF every connect() is the
 * supervisor's anyway). Runs before the exec; the listener's port, or -1 on
 * any failure, leaving the proxy variables unset. */
static int sandbox_setup_proxy(const char *uds_path, int in_ns) {
    if (in_ns && sandbox_bring_up_lo() != 0) return -1;

    /* Bind 127.0.0.1:0 — the shim's HTTP CONNECT listener. Not CLOEXEC: the
     * forked shim must keep it across its exec. */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(lfd, 16) != 0) { close(lfd); return -1; }
    socklen_t alen = sizeof(addr);
    if (getsockname(lfd, (struct sockaddr *)&addr, &alen) != 0) { close(lfd); return -1; }
    int port = ntohs(addr.sin_port);

    /* Materialize the shim in an anonymous memfd — no on-disk artifact and no
     * dependency on /tmp being exec-able. Not MFD_CLOEXEC: the fork must keep it
     * across exec (it execs /proc/self/fd/<mfd>). */
    int mfd = memfd_create("net_shim", 0);
    if (mfd < 0) { close(lfd); return -1; }
    ssize_t wr = write(mfd, net_shim_blob, (size_t)net_shim_blob_len);
    if (wr != (ssize_t)net_shim_blob_len) { close(mfd); close(lfd); return -1; }

    pid_t parent = getpid();
    pid_t shim = fork();
    if (shim < 0) { close(mfd); close(lfd); return -1; }
    if (shim == 0) {
        /* No PID namespace backs this child (skip_pid_ns, or no namespace at
         * all), so there's no kernel-enforced teardown when the tool process
         * exits — without this, the shim orphans to init and lingers forever.
         * PDEATHSIG doesn't survive fork(), so it must be set here, not
         * inherited from the process that forked us (same reasoning as the
         * PID-ns init's re-arm, contain_enter above). */
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        /* The signal is sent when the parent dies, so arming it after the
         * parent is already gone arms nothing. A trivial script finishes
         * before this child is first scheduled on a busy box — every call
         * then leaked a shim. Re-check after arming closes the window. */
        if (getppid() != parent) _exit(0);

        /* Shim: drop stdout/stderr (they point at the tool result pipe — the
         * shim must not pollute tool output nor hold the pipe open), keep lfd,
         * exec the link-isolated binary with the listener fd + UDS path. */
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); dup2(devnull, STDOUT_FILENO);
                            dup2(devnull, STDERR_FILENO); if (devnull > 2) close(devnull); }
        char fdbuf[16];
        snprintf(fdbuf, sizeof(fdbuf), "%d", lfd);
        /* fexecve (execveat AT_EMPTY_PATH under the hood on Linux ≥3.19) execs
         * the memfd WITHOUT touching /proc — the js child (skip_pid_ns)
         * has no /proc, so an execl("/proc/self/fd/N") would ENOENT and the shim
         * would die, closing the listener (curl → connection refused). */
        char *const argv[] = { "net_shim", fdbuf, (char *)uds_path, NULL };
        extern char **environ;
        fexecve(mfd, argv, environ);
        _exit(127);
    }

    close(mfd);

    /* The shim owns the listener now; advertise it to the command. The
     * command never sees lfd (closed here, and it is not CLOEXEC-safe to leak). */
    close(lfd);
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d", port);
    setenv("HTTP_PROXY", url, 1);  setenv("http_proxy", url, 1);
    setenv("HTTPS_PROXY", url, 1); setenv("https_proxy", url, 1);
    setenv("ALL_PROXY", url, 1);   setenv("all_proxy", url, 1);
    setenv("NO_PROXY", "localhost,127.0.0.1,::1", 1);
    setenv("no_proxy", "localhost,127.0.0.1,::1", 1);
    return port;
}

/* net_shim as a file the shell can run by name (`connect` mode, for ssh's
 * ProxyCommand). In the mount namespace: the root tmpfs, rebuilt per call.
 * Otherwise: the agent's scratch dir, written beside and renamed into place,
 * so a concurrent call still running the old copy is undisturbed and a
 * symlink planted at the name is replaced, never followed. Best-effort:
 * without the file, ssh simply has no proxy route. */
static void write_connect_shim(const SandboxConfig *cfg, int mounted) {
    char path[PATH_MAX], tmp[PATH_MAX + 16];
    if (mounted) snprintf(path, sizeof(path), "%s", SANDBOX_CONNECT_PATH);
    else if (!cfg->tmp_dir || !cfg->tmp_dir[0] ||
             snprintf(path, sizeof(path), "%s/.cclaw_connect", cfg->tmp_dir)
                 >= (int)sizeof(path)) return;
    snprintf(tmp, sizeof(tmp), mounted ? "%s" : "%s.%d", path, (int)getpid());
    /* O_NOFOLLOW: refuse to follow a symlink at this path rather than
     * writing 0755 content wherever it points. */
    int cfd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0755);
    ssize_t wr = cfd >= 0 ? write(cfd, net_shim_blob, (size_t)net_shim_blob_len) : -1;
    if (cfd >= 0) close(cfd);
    if (wr != (ssize_t)net_shim_blob_len || (!mounted && rename(tmp, path) != 0)) {
        if (!mounted) unlink(tmp);
        return;
    }
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "%s connect", path);
    setenv("CCLAW_CONNECT", cmd, 1);
    snprintf(cmd, sizeof(cmd), "ssh -o ProxyCommand='%s connect %%h %%p'", path);
    setenv("GIT_SSH_COMMAND", cmd, 1);
}

/* Give the child its route out: the broker's proxy socket — bound at a fixed
 * in-boundary path by contain_enter (the mount namespace), or at its real
 * path in the agent folder (NE_ENV, and a network namespace with no mount
 * namespace). Runs after the env scrub. */
int contain_egress(const SandboxConfig *cfg) {
    int in_ns = cfg->plan.ne == NE_NETNS, mounted = cfg->plan.fe == FE_MOUNTNS;
    const char *uds = mounted ? SANDBOX_PROXY_SOCK_PATH : cfg->proxy_sock;
    setenv("CCLAW_PROXY_SOCK", uds, 1);

    /* The loopback HTTP proxy: the one mechanism for every child — our own
     * libcurl (js) and whatever the shell runs. */
    int port = sandbox_setup_proxy(uds, in_ns);
    if (port < 0) return -1;

    /* Shell only: programs that take a proxy *command* rather than proxy
     * variables. ssh is the one that matters, and git reads GIT_SSH_COMMAND,
     * so git-over-ssh works unprompted; a bare `ssh` needs the option spelled
     * out (the shell tool's description says how). */
    if (!cfg->skip_pid_ns) write_connect_shim(cfg, mounted);
    return port;
}

const char *contain_llm_sock_path(void) { return SANDBOX_LLM_SOCK_PATH; }

int contain_enter_net(void) {
    uid_t uid = getuid();
    gid_t gid = getgid();
    if (unshare(CLONE_NEWUSER | CLONE_NEWNET) != 0 || map_identity(uid, gid) != 0)
        return -1;
    return sandbox_bring_up_lo();
}

/* ── Landlock ───────────────────────────────────────────────────────────
 * Raw syscalls and local definitions, so the static cross builds need no
 * kernel headers newer than they have. The numbers are the same on every
 * architecture (allocated after the syscall tables were unified). */
#ifndef __NR_landlock_create_ruleset
#define __NR_landlock_create_ruleset 444
#define __NR_landlock_add_rule       445
#define __NR_landlock_restrict_self  446
#endif
#define LL_EXECUTE   (1ull << 0)
#define LL_WRITE     (1ull << 1)
#define LL_READ      (1ull << 2)
#define LL_READ_DIR  (1ull << 3)
#define LL_ABI1      ((1ull << 13) - 1)  /* every right ABI 1 knows */
#define LL_REFER     (1ull << 13)        /* ABI 2 */
#define LL_TRUNCATE  (1ull << 14)        /* ABI 3 */
#define LL_IOCTL_DEV (1ull << 15)        /* ABI 5 */
#define LL_FILE_RIGHTS (LL_EXECUTE | LL_WRITE | LL_READ | LL_TRUNCATE | LL_IOCTL_DEV)
#define LL_SCOPE_ALL 0x3ull  /* ABI 6: abstract unix sockets, signals */

struct ll_ruleset_attr { uint64_t fs, net, scoped; };
struct ll_path_beneath { uint64_t allowed; int32_t parent_fd; } __attribute__((packed));

static long landlock_abi_now(void) {
    long v = syscall(__NR_landlock_create_ruleset, NULL, 0, 1u /* _VERSION */);
    return v < 0 ? 0 : v;
}

/* Only rights this ABI knows are asked for: a newer one is refused (EINVAL)
 * by an older kernel. The network rights (ABI 4) are not used: port-only. */
int contain_landlock(const SandboxMount *rules, size_t n) {
    long abi = landlock_abi_now();
    if (abi < 1) { errno = ENOSYS; return -1; }
    uint64_t all = LL_ABI1 | (abi >= 2 ? LL_REFER : 0) | (abi >= 3 ? LL_TRUNCATE : 0) |
                   (abi >= 5 ? LL_IOCTL_DEV : 0);
    struct ll_ruleset_attr ra = { .fs = all, .scoped = abi >= 6 ? LL_SCOPE_ALL : 0 };
    size_t size = abi >= 6 ? 24 : abi >= 4 ? 16 : 8;  /* what this kernel reads */
    int rs = (int)syscall(__NR_landlock_create_ruleset, &ra, size, 0);
    if (rs < 0) return -1;
    int rc = 0;
    for (size_t i = 0; i < n && rc == 0; i++) {
        uint64_t acc = rules[i].ro == CONTAIN_LL_RW ? all
                     : rules[i].ro == CONTAIN_LL_RO ? LL_EXECUTE | LL_READ | LL_READ_DIR
                     : LL_READ | LL_WRITE | LL_TRUNCATE | LL_IOCTL_DEV;
        int fd = open(rules[i].path, O_PATH | O_CLOEXEC);
        if (fd < 0) continue;  /* absent: nothing to allow */
        struct stat st;
        if (fstat(fd, &st) != 0) rc = -1;
        else if (!S_ISDIR(st.st_mode)) acc &= LL_FILE_RIGHTS;
        struct ll_path_beneath pb = { acc & all, fd };
        if (rc == 0) rc = (int)syscall(__NR_landlock_add_rule, rs, 1 /* PATH_BENEATH */, &pb, 0);
        close(fd);
    }
    if (rc == 0) rc = (int)syscall(__NR_landlock_restrict_self, rs, 0);
    close(rs);
    return rc == 0 ? 0 : -1;
}

/* ── seccomp ────────────────────────────────────────────────────────────
 * Classic BPF and the seccomp ABI, defined here: the static builds compile
 * against musl's headers alone, which carry no <linux/...>. These values are
 * kernel ABI and never change. The build architecture's own syscall numbers;
 * anything else is killed. A target not listed reports no seccomp, and the
 * planner refuses what needs it. */
#if defined(__x86_64__)
#define SC_ARCH 0xC000003Eu          /* AUDIT_ARCH_X86_64 */
#elif defined(__i386__)
#define SC_ARCH 0x40000003u          /* AUDIT_ARCH_I386 */
#elif defined(__aarch64__)
#define SC_ARCH 0xC00000B7u          /* AUDIT_ARCH_AARCH64 */
#elif defined(__arm__) && !defined(__ARMEB__)
#define SC_ARCH 0x40000028u          /* AUDIT_ARCH_ARM */
#elif defined(__riscv) && __riscv_xlen == 64
#define SC_ARCH 0xC00000F3u          /* AUDIT_ARCH_RISCV64 */
#endif
/* i386 programs reach the socket calls through socketcall(), whose arguments
 * are in memory: sockets can be refused there, not inspected. */
#if defined(__NR_socketcall) && !defined(__arm__)
#define SC_MUX __NR_socketcall
#endif
#ifndef __NR_io_uring_setup
#define __NR_io_uring_setup    425   /* unified numbers, as Landlock's */
#define __NR_io_uring_enter    426
#define __NR_io_uring_register 427
#endif

#ifdef SC_ARCH
struct sc_insn { uint16_t code; uint8_t jt, jf; uint32_t k; };   /* sock_filter */
struct sc_prog { unsigned short len; struct sc_insn *filter; };  /* sock_fprog */
struct sc_data { int nr; uint32_t arch; uint64_t ip; uint64_t args[6]; };
#define SC_LD_ABS  0x20u   /* BPF_LD | BPF_W | BPF_ABS */
#define SC_JEQ     0x15u   /* BPF_JMP | BPF_JEQ | BPF_K */
#define SC_JGE     0x35u   /* BPF_JMP | BPF_JGE | BPF_K */
#define SC_RET_K   0x06u   /* BPF_RET | BPF_K */
#define SC_KILL    0x80000000u  /* SECCOMP_RET_KILL_PROCESS; before 4.14 it kills the thread */
#define SC_ERRNO   0x00050000u
#define SC_ALLOW   0x7fff0000u
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define SC_ARG0 (offsetof(struct sc_data, args) + 4)
#else
#define SC_ARG0 offsetof(struct sc_data, args)   /* the low word */
#endif
#define SC_INSN(c, t, f, v) (struct sc_insn){ (uint16_t)(c), (t), (f), (uint32_t)(v) }
#define SC_RET(v) SC_INSN(SC_RET_K, 0, 0, (v))

static void sc_deny(struct sc_insn *f, unsigned *k, unsigned nr, unsigned e) {
    f[(*k)++] = SC_INSN(SC_JEQ, 0, 1, nr);
    f[(*k)++] = SC_RET(SC_ERRNO | e);
}

#ifdef SC_MUX
/* Syscall nr with its first argument tested by `op` (SC_JEQ) against
 * val: deny on a match, allow otherwise. The accumulator then holds the
 * argument, so these blocks come after every plain sc_deny. */
static void sc_deny_arg0(struct sc_insn *f, unsigned *k, unsigned nr,
                         unsigned op, unsigned val, unsigned e) {
    f[(*k)++] = SC_INSN(SC_JEQ, 0, 4, nr);
    f[(*k)++] = SC_INSN(SC_LD_ABS, 0, 0, SC_ARG0);
    f[(*k)++] = SC_INSN(op, 0, 1, val);
    f[(*k)++] = SC_RET(SC_ERRNO | e);
    f[(*k)++] = SC_RET(SC_ALLOW);
}
#else
#define SC_NOTIF   0x7fc00000u  /* SECCOMP_RET_USER_NOTIF */
#define SC_JSET    0x45u        /* BPF_JMP | BPF_JSET | BPF_K */
#define SC_AND     0x54u        /* BPF_ALU | BPF_AND | BPF_K */
#define SC_ARG(n)  (SC_ARG0 + 8 * (n))  /* the low word: the kernel takes ints */

/* A send that connects (TCP Fast Open) without connect(): EPERM when the
 * flags argument has MSG_FASTOPEN. Ends in a return, like the block below. */
static void sc_deny_fastopen(struct sc_insn *f, unsigned *k, unsigned nr, unsigned arg) {
    f[(*k)++] = SC_INSN(SC_JEQ, 0, 4, nr);
    f[(*k)++] = SC_INSN(SC_LD_ABS, 0, 0, SC_ARG(arg));
    f[(*k)++] = SC_INSN(SC_JSET, 0, 1, 0x20000000u /* MSG_FASTOPEN */);
    f[(*k)++] = SC_RET(SC_ERRNO | EPERM);
    f[(*k)++] = SC_RET(SC_ALLOW);
}

/* socket(): unix and netlink as asked; inet and inet6 only as TCP streams,
 * so every inet connection is one connect() the supervisor sees; the rest
 * EACCES. Registers only — no pointer is read. */
static void sc_inet_stream(struct sc_insn *f, unsigned *k) {
    const struct sc_insn b[] = {
        SC_INSN(SC_JEQ, 0, 14, __NR_socket),
        SC_INSN(SC_LD_ABS, 0, 0, SC_ARG(0)),
        SC_INSN(SC_JEQ, 11, 0, AF_UNIX),
        SC_INSN(SC_JEQ, 10, 0, AF_NETLINK),
        SC_INSN(SC_JEQ, 2, 0, AF_INET),
        SC_INSN(SC_JEQ, 1, 0, AF_INET6),
        SC_RET(SC_ERRNO | EACCES),
        SC_INSN(SC_LD_ABS, 0, 0, SC_ARG(1)),
        SC_INSN(SC_AND, 0, 0, ~(unsigned)(SOCK_NONBLOCK | SOCK_CLOEXEC)),
        SC_INSN(SC_JEQ, 0, 3, SOCK_STREAM),
        SC_INSN(SC_LD_ABS, 0, 0, SC_ARG(2)),
        SC_INSN(SC_JEQ, 2, 0, 0),
        SC_INSN(SC_JEQ, 1, 0, IPPROTO_TCP),
        SC_RET(SC_ERRNO | EACCES),
        SC_RET(SC_ALLOW),
    };
    memcpy(f + *k, b, sizeof(b));
    *k += sizeof(b) / sizeof(b[0]);
}
#endif
#endif

int contain_seccomp(unsigned flags) {
#ifndef SC_ARCH
    (void)flags;
    errno = ENOSYS;
    return -1;
#else
    struct sc_insn f[96];
    unsigned k = 0;
    f[k++] = SC_INSN(SC_LD_ABS, 0, 0, offsetof(struct sc_data, arch));
    f[k++] = SC_INSN(SC_JEQ, 1, 0, SC_ARCH);
    f[k++] = SC_RET(SC_KILL);
    f[k++] = SC_INSN(SC_LD_ABS, 0, 0, offsetof(struct sc_data, nr));
#ifdef __x86_64__
    f[k++] = SC_INSN(SC_JGE, 0, 1, 0x40000000u);
    f[k++] = SC_RET(SC_KILL);  /* the x32 ABI's syscall bit */
#endif
    sc_deny(f, &k, __NR_io_uring_setup, ENOSYS);
    sc_deny(f, &k, __NR_io_uring_enter, ENOSYS);
    sc_deny(f, &k, __NR_io_uring_register, ENOSYS);
#ifdef __NR_socket
    if (flags & CONTAIN_SC_NOSOCKET) sc_deny(f, &k, __NR_socket, EACCES);
#endif
    if (flags & CONTAIN_SC_NOTRACE) {
        sc_deny(f, &k, __NR_ptrace, EPERM);
        sc_deny(f, &k, __NR_process_vm_readv, EPERM);
        sc_deny(f, &k, __NR_process_vm_writev, EPERM);
    }
#ifdef __arm__
    /* EABI programs make the socket calls directly, but the old multiplexer
     * (102) still answers: refused whole wherever sockets are filtered. */
    if (flags & (CONTAIN_SC_NOSOCKET | CONTAIN_SC_CONNECT | CONTAIN_SC_INETSTREAM))
        sc_deny(f, &k, 102, ENOSYS);
#endif
#ifdef SC_MUX
    /* i386's multiplexer: its first argument is the call, 1 = socket. */
    if (flags & CONTAIN_SC_NOSOCKET) sc_deny_arg0(f, &k, SC_MUX, SC_JEQ, 1, EACCES);
    /* Socket arguments live in memory there: no connect filter can be written. */
    if (flags & (CONTAIN_SC_CONNECT | CONTAIN_SC_INETSTREAM)) { errno = ENOSYS; return -1; }
#else
    if (flags & CONTAIN_SC_CONNECT) {
        f[k++] = SC_INSN(SC_JEQ, 0, 1, __NR_connect);
        f[k++] = SC_RET(SC_NOTIF);
    }
    if (flags & CONTAIN_SC_INETSTREAM) {
        sc_deny_fastopen(f, &k, __NR_sendto, 3);
        sc_deny_fastopen(f, &k, __NR_sendmsg, 2);
        sc_deny_fastopen(f, &k, __NR_sendmmsg, 3);
        sc_inet_stream(f, &k);
    }
#endif
    f[k++] = SC_RET(SC_ALLOW);
    struct sc_prog prog = { .len = (unsigned short)k, .filter = f };
    if (!(flags & CONTAIN_SC_CONNECT))
        return prctl(PR_SET_SECCOMP, 2 /* SECCOMP_MODE_FILTER */, &prog, 0, 0) == 0 ? 0 : -1;
#ifndef __NR_seccomp
    errno = ENOSYS;
    return -1;
#else
    /* NEW_LISTENER: the return is the supervisor's fd. WAIT_KILLABLE_RECV
     * (5.19): once the supervisor has the call, only a fatal signal ends the
     * wait — without it Go's preemption signals restart connect() under it. */
    int l = (int)syscall(__NR_seccomp, 1 /* SET_MODE_FILTER */, (1u << 3) | (1u << 5), &prog);
    if (l >= 0) fcntl(l, F_SETFD, FD_CLOEXEC);
    return l;
#endif
#endif
}

/* The DNS stub's socket, inside the namespace: 127.0.0.1:53, which the
 * namespace's creator may bind before the exec drops its capabilities. */
static int dns_socket(void) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(53),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (fd >= 0 && bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0) { close(fd); fd = -1; }
    return fd;
}

int contain_intercept(const SandboxConfig *cfg, unsigned sc, int port) {
    /* The stub only where resolv.conf could be pointed at it (contain_enter). */
    int dns = cfg->plan.transparent && cfg->plan.fe == FE_MOUNTNS && port > 0
              ? dns_socket() : -1;
    int l = cfg->notif_sock > 0 ? contain_seccomp(sc | CONTAIN_SC_CONNECT) : -1;
    if (l < 0) {
        if (dns >= 0) close(dns);
        return -1;
    }
    int fds[2] = { l, dns }, n = dns >= 0 ? 2 : 1;
    int32_t p = port;
    union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(fds))]; } c;
    memset(&c, 0, sizeof(c));
    struct iovec io = { &p, sizeof(p) };
    struct msghdr m = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = c.b,
                        .msg_controllen = CMSG_SPACE(n * sizeof(int)) };
    struct cmsghdr *h = CMSG_FIRSTHDR(&m);
    h->cmsg_level = SOL_SOCKET;
    h->cmsg_type = SCM_RIGHTS;
    h->cmsg_len = CMSG_LEN(n * sizeof(int));
    memcpy(CMSG_DATA(h), fds, n * sizeof(int));
    ssize_t w = sendmsg(cfg->notif_sock, &m, MSG_NOSIGNAL);
    close(l);
    if (dns >= 0) close(dns);
    close(cfg->notif_sock);
    return w == (ssize_t)sizeof(p) ? 0 : -1;
}
