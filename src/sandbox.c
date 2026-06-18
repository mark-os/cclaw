#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sandbox.h"
#include "preload_blob.h"

/* Bind-mask the secret key + DB ciphertext from the child's view.
 *
 * The key lives at <dir of db_path>/.cclaw_key, next to cclaw.db. trusted /
 * bootstrap agents get the CWD bind-mounted rw, and in CLI mode that CWD is
 * the dir holding both files — so a child could read the key and decrypt
 * every stored secret. We bind an empty, unreadable file over each sensitive
 * path *inside the new root*. Files not reachable in the child's mount tree
 * (the standard/restricted case) never materialize under newroot, so the stat
 * fails and we skip them — they are already invisible by omission. */
static void sandbox_mask_state_files(const char *newroot, const char *db_path,
                                     const char *env_file) {
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

    /* Targets: the DB family (ciphertext) and the key file (crown jewel). */
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
    targets[nt++] = db_abs;
    targets[nt++] = dbwal;
    targets[nt++] = dbshm;
    if (klen > 0 && (size_t)klen < sizeof(keyf)) targets[nt++] = keyf;

    char envf[PATH_MAX];
    if (env_file && env_file[0] && realpath(env_file, envf))
        targets[nt++] = envf;

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

/* V82/V37/V22a: Apply namespace sandbox in the child.
 * unshare(USER|MNT|PID|NET), write uid/gid maps, pivot_root into minimal fs.
 * System dirs mounted ro, workspace rw, cwd_path rw (CLI mode).
 * Returns 0 on success, -1 on failure. */
static int sandbox_apply_namespace(const char *workspace, const char *cwd_path,
                                   const char *db_path, const char *env_file) {
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

    if (unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWNET) != 0)
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

    /* Create tmpfs as new root */
    char newroot[] = "/tmp/.cclaw_ns_XXXXXX";
    if (!mkdtemp(newroot))
        return -1;
    if (mount("none", newroot, "tmpfs", MS_NOSUID | MS_NODEV, "size=1m") != 0)
        return -1;

    /* Bind-mount system dirs read-only */
    static const char *sys_dirs[] = {
        "/bin", "/usr", "/lib", "/lib64", "/etc", "/proc", "/dev", "/sbin", NULL
    };
    for (int i = 0; sys_dirs[i]; i++) {
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

    /* Create /tmp in new root (ephemeral tmpfs, writable for scratch) */
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/tmp", newroot);
    mkdir(tmp_path, 01777);

    /* Bind-mount workspace rw */
    if (ws_resolved) {
        struct stat st;
        if (stat(ws_resolved, &st) == 0 && S_ISDIR(st.st_mode)) {
            char ws_dst[PATH_MAX + 64];
            snprintf(ws_dst, sizeof(ws_dst), "%s%s", newroot, ws_resolved);
            char *p = ws_dst + strlen(newroot) + 1;
            for (char *slash = p; *slash; slash++) {
                if (*slash == '/') {
                    *slash = '\0';
                    mkdir(ws_dst, 0755);
                    *slash = '/';
                }
            }
            mkdir(ws_dst, 0755);
            mount(ws_resolved, ws_dst, NULL, MS_BIND, NULL);
        }
    }

    /* Bind-mount CWD rw (CLI mode) */
    if (cwd_resolved) {
        struct stat st;
        if (stat(cwd_resolved, &st) == 0 && S_ISDIR(st.st_mode)) {
            char cwd_dst[PATH_MAX + 64];
            snprintf(cwd_dst, sizeof(cwd_dst), "%s%s", newroot, cwd_resolved);
            char *p = cwd_dst + strlen(newroot) + 1;
            for (char *slash = p; *slash; slash++) {
                if (*slash == '/') {
                    *slash = '\0';
                    mkdir(cwd_dst, 0755);
                    *slash = '/';
                }
            }
            mkdir(cwd_dst, 0755);
            mount(cwd_resolved, cwd_dst, NULL, MS_BIND, NULL);
        }
    }

    /* Mask the secret key + DB ciphertext if a bound path (CWD/workspace)
     * would otherwise expose them. Must run after binds, before pivot_root. */
    sandbox_mask_state_files(newroot, db_path, env_file);

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
     * PID namespace. Mount setup is already done, so the fork+exec is clean. */
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

    /* CWD into workspace so commands start there */
    if (ws_resolved && chdir(ws_resolved) != 0)
        chdir("/tmp");

    return 0;
}

/* V47: scrub the environment before exec. env_mode==1 wipes everything and
 * sets only PATH+TMPDIR; otherwise inherit minus known secret-bearing vars. */
static void sandbox_scrub_env(int env_mode) {
    extern char **environ;
    if (env_mode == 1) {
        /* Clean allowlist: wipe entire env, set only essentials */
        if (environ) environ[0] = NULL;  /* portable clearenv */
        setenv("PATH", "/bin:/usr/bin", 1);
        setenv("TMPDIR", "/tmp", 1);
        return;
    }

    /* Legacy trusted mode: inherit env minus known secret names */
    setenv("PATH", "/bin:/usr/bin", 1);
    unsetenv("HOME");
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

/* Trust-level: apply resource limits before exec */
static void sandbox_apply_rlimits(const SandboxConfig *cfg) {
    if (cfg->rlimits.nproc > 0) {
        struct rlimit rl = {(rlim_t)cfg->rlimits.nproc, (rlim_t)cfg->rlimits.nproc};
        setrlimit(RLIMIT_NPROC, &rl);
    }
    if (cfg->rlimits.as_mb > 0) {
        rlim_t bytes = (rlim_t)cfg->rlimits.as_mb * 1024 * 1024;
        struct rlimit rl = {bytes, bytes};
        setrlimit(RLIMIT_AS, &rl);
    }
    if (cfg->rlimits.cpu_sec > 0) {
        struct rlimit rl = {(rlim_t)cfg->rlimits.cpu_sec, (rlim_t)cfg->rlimits.cpu_sec};
        setrlimit(RLIMIT_CPU, &rl);
    }
}

int sandbox_child_setup(const SandboxConfig *cfg) {
    const char *ws = cfg->workspace;
    const char *cwd = cfg->cwd_path;
    const char *psock = cfg->proxy_sock;

    /* Trust-level: suppress CWD mount / proxy per policy */
    if (!cfg->mount_cwd) cwd = NULL;
    if (cfg->net_mode) psock = NULL;

    /* V82/V37: namespace sandbox — fail closed if requested but unavailable */
    if (cfg->sandbox && sandbox_apply_namespace(ws, cwd, cfg->db_path, cfg->env_file) != 0) {
        fprintf(stderr, "error: namespace sandbox unavailable (errno=%d); "
                "this trust level requires it — enable unprivileged user "
                "namespaces or set the agent's trust_level to 'host'\n", errno);
        return -1;
    }

    /* Trust-level: remount workspace read-only after pivot_root */
    if (cfg->sandbox && cfg->workspace_ro && ws) {
        char ws_real[PATH_MAX];
        if (realpath(ws, ws_real) == NULL ||
            mount(NULL, ws_real, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL) != 0) {
            fprintf(stderr, "error: read-only workspace remount failed "
                    "(errno=%d); refusing to run writable\n", errno);
            return -1;
        }
    }

    sandbox_scrub_env(cfg->env_mode);

    /* V83: set proxy socket path for LD_PRELOAD lib (after env cleanup) */
    if (psock) setenv("CCLAW_PROXY_SOCK", psock, 1);

    sandbox_apply_rlimits(cfg);

    /* Write preload lib into the namespace's private /tmp and set LD_PRELOAD —
     * the interposer is the only egress inside CLONE_NEWNET, so a failed write
     * must be loud, not silently no-network. */
    if (psock && cfg->sandbox) {
        int pfd = open("/tmp/libcclaw_net.so", O_WRONLY | O_CREAT | O_TRUNC, 0755);
        ssize_t wr = pfd >= 0 ? write(pfd, preload_net_blob, (size_t)preload_net_blob_len) : -1;
        if (pfd >= 0) close(pfd);
        if (wr == (ssize_t)preload_net_blob_len)
            setenv("LD_PRELOAD", "/tmp/libcclaw_net.so", 1);
        else
            fprintf(stderr, "[cclaw] warning: proxy preload setup failed "
                    "(errno=%d); child has no network\n", errno);
    }

    return 0;
}

void sandbox_policy_from_trust(const char *trust_level, SandboxConfig *cfg) {
    if (trust_level && strcmp(trust_level, "host") == 0) {
        cfg->sandbox = 0;
        cfg->env_mode = 0; cfg->net_mode = 0; cfg->mount_cwd = 1; cfg->workspace_ro = 0;
        cfg->rlimits.nproc = 0; cfg->rlimits.as_mb = 0; cfg->rlimits.cpu_sec = 0;
    } else if (trust_level && strcmp(trust_level, "trusted") == 0) {
        cfg->sandbox = 1;
        cfg->env_mode = 0; cfg->net_mode = 0; cfg->mount_cwd = 1; cfg->workspace_ro = 0;
        cfg->rlimits.nproc = 0; cfg->rlimits.as_mb = 0; cfg->rlimits.cpu_sec = 0;
    } else if (trust_level && strcmp(trust_level, "restricted") == 0) {
        cfg->sandbox = 1;
        cfg->env_mode = 1; cfg->net_mode = 1; cfg->mount_cwd = 0; cfg->workspace_ro = 1;
        cfg->rlimits.nproc = 8; cfg->rlimits.as_mb = 128; cfg->rlimits.cpu_sec = 10;
    } else { /* "standard", unknown, NULL */
        cfg->sandbox = 1;
        cfg->env_mode = 1; cfg->net_mode = 0; cfg->mount_cwd = 0; cfg->workspace_ro = 0;
        cfg->rlimits.nproc = 64; cfg->rlimits.as_mb = 512; cfg->rlimits.cpu_sec = 60;
    }
}
