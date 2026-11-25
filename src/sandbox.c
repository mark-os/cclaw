#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
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

/* /opt is here because that is where real toolchains land — distro node
 * packages, vendored JDKs, hand-unpacked SDKs. */
const char *const SANDBOX_SYS_DIRS[] = {
    "/bin", "/usr", "/lib", "/lib64", "/etc", "/dev", "/sbin", "/opt", NULL
};

/* ── Levels, shipped rows, the row an agent names ─────────────────────── */

/* The shipped rows. NPROC is the fork-bomb backstop (on a 128 MB target one
 * bad generated shell line otherwise takes the daemon down with it); a CPU cap
 * punishes legitimate builds, so only offline, which runs short local jobs,
 * has one. Strictest first: contain_suggest walks them in this order. */
const ContainRow CONTAIN_SHIPPED[] = {
    { "standard", { FILES_PRIVATE,  NET_FIREWALLED,  256, 0 } },
    { "offline",  { FILES_PRIVATE,  NET_NONE,      64, 120 } },
    { "basic-firewalled", { FILES_CONFINED, NET_FIREWALLED, 256, 0 } },
    { "basic",    { FILES_CONFINED, NET_ADVISORY, 256, 0 } },
    { "none",     { FILES_OPEN,     NET_DIRECT,     0, 0 } },
};
const size_t CONTAIN_SHIPPED_N = sizeof(CONTAIN_SHIPPED) / sizeof(CONTAIN_SHIPPED[0]);

static const char *const FILES_NAMES[] = { "open", "confined", "private" };
static const char *const NET_NAMES[]   = { "direct", "advisory", "firewalled", "none" };
static const char *const FE_NAMES[]    = { "none", "path check in cclaw's code",
                                           "mount namespace", "Landlock" };
static const char *const NE_NAMES[]    = { "none", "proxy variables only",
                                           "cclaw's code dials the proxy", "network namespace",
                                           "seccomp refuses socket()",
                                           "seccomp hands connect() to the proxy" };

const char *files_level_name(FilesLevel l) { return (unsigned)l < 3 ? FILES_NAMES[l] : "?"; }
const char *net_level_name(NetLevel l) { return (unsigned)l < 4 ? NET_NAMES[l] : "?"; }
const char *files_enforcer_name(FilesEnforcer e) { return (unsigned)e < 4 ? FE_NAMES[e] : "?"; }
const char *net_enforcer_name(NetEnforcer e) { return (unsigned)e < 6 ? NE_NAMES[e] : "?"; }

static int name_index(const char *const *names, int n, const char *s) {
    for (int i = 0; s && i < n; i++) if (strcmp(names[i], s) == 0) return i;
    return -1;
}

int containment_load(sqlite3 *db, const char *name, Containment *c) {
    if (!name || !name[0]) name = "standard";
    snprintf(c->name, sizeof(c->name), "%s", name);
    c->known = 0;
    memset(&c->lv, 0, sizeof(c->lv));
    sqlite3_stmt *st;
    if (!db || sqlite3_prepare_v2(db,
            "SELECT files, network, nproc, cpu_sec FROM containments WHERE name=?1",
            -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        int f = name_index(FILES_NAMES, 3, (const char *)sqlite3_column_text(st, 0));
        int n = name_index(NET_NAMES, 4, (const char *)sqlite3_column_text(st, 1));
        if (f >= 0 && n >= 0) {
            c->lv = (ContainLevels){ (FilesLevel)f, (NetLevel)n,
                                     sqlite3_column_int(st, 2), sqlite3_column_int(st, 3) };
            c->known = 1;
        }
    }
    sqlite3_finalize(st);
    return c->known;
}

/* ── The planner ──────────────────────────────────────────────────────── */

static const char *vehicle_noun(ExecVehicle v) {
    return v == EXEC_SHELL ? "the shell" : v == EXEC_JS ? "js" : "file tools";
}

/* The rules, for one row: per level the strongest enforcer the host has —
 * mount namespace, then Landlock, then (file and js only) the path check;
 * network namespace, then seccomp ('none': refusing socket(); otherwise
 * handing connect() to the proxy). Returns NULL with *out filled, or what the
 * host lacks (or why the combination cannot be built). */
static const char *plan_levels(const ContainLevels *lv, ExecVehicle v,
                               unsigned caps, ContainPlan *p) {
    memset(p, 0, sizeof(*p));
    p->lv = *lv;
    int ns = (caps & CONTAIN_CAP_MOUNTNS) != 0;
    switch (lv->files) {
    case FILES_PRIVATE:
        if (!ns) return "files 'private' needs unprivileged user namespaces";
        p->fe = FE_MOUNTNS;
        break;
    case FILES_CONFINED:
        /* For file and js, FE_LANDLOCK keeps the path check on too. */
        if (ns) p->fe = FE_MOUNTNS;
        else if (caps & CONTAIN_CAP_LANDLOCK) p->fe = FE_LANDLOCK;
        else if (v != EXEC_SHELL) p->fe = FE_CODE;
        else return "files 'confined' for the shell needs unprivileged user namespaces"
                    " or Landlock (kernel 5.13+)";
        break;
    case FILES_OPEN:
        p->fe = FE_NONE;
        break;
    }
    /* The mount namespace always brings an empty network namespace: there
     * the proxy is the only way out, which keeps none, firewalled and (more
     * strongly than it promises) advisory, but cannot be direct. */
    int notif = (caps & CONTAIN_CAP_NOTIF) && (caps & CONTAIN_CAP_SHIM);
    if (p->fe == FE_MOUNTNS) {
        if (lv->net == NET_DIRECT)
            return "network 'direct' cannot be combined with the mount namespace"
                   " this files level uses";
        p->ne = NE_NETNS;
        p->transparent = notif && v == EXEC_SHELL && lv->net != NET_NONE;
        return NULL;
    }
    /* The file vehicle is cclaw's own file code: it opens no socket, so no
     * network level needs an enforcer there. js is cclaw's code too, and
     * QuickJS has no socket primitive: its one way out is http_request, whose
     * curl dials the broker's proxy socket or refuses (NE_CODE). */
    if (v == EXEC_FILE || lv->net == NET_DIRECT) { p->ne = NE_NONE; return NULL; }
    if (v == EXEC_JS) { p->ne = NE_CODE; return NULL; }
    if (lv->net == NET_ADVISORY) {
        if (!(caps & CONTAIN_CAP_SHIM))
            return "network 'advisory' for the shell needs the loopback proxy,"
                   " which this build's containment backend does not have";
        p->ne = notif ? NE_NOTIF : NE_ENV;
        return NULL;
    }
    if (caps & CONTAIN_CAP_NETNS) {
        p->ne = NE_NETNS;
        p->transparent = notif && lv->net != NET_NONE;
        return NULL;
    }
    if (lv->net == NET_NONE && (caps & CONTAIN_CAP_SECCOMP)) { p->ne = NE_SECCOMP; return NULL; }
    if (lv->net == NET_FIREWALLED && notif) { p->ne = NE_NOTIF; return NULL; }
    return lv->net == NET_NONE
        ? "network 'none' for the shell needs unprivileged user namespaces (a network"
          " namespace) or seccomp"
        : "network 'firewalled' for the shell needs unprivileged user namespaces (a"
          " network namespace) or seccomp user notification (kernel 5.19+, and"
          " pidfd_getfd, which Docker's default seccomp profile allows only with"
          " CAP_SYS_PTRACE)";
}

/* Can lv be kept for vehicle `only` (0 = every child vehicle)? */
static int keepable(const ContainLevels *lv, ExecVehicle only, unsigned caps) {
    static const ExecVehicle vs[] = { EXEC_FILE, EXEC_JS, EXEC_SHELL };
    ContainPlan p;
    for (size_t i = 0; i < 3; i++)
        if ((!only || only == vs[i]) && plan_levels(lv, vs[i], caps, &p)) return 0;
    return 1;
}

static const char *suggest(ExecVehicle only, unsigned caps) {
    for (size_t i = 0; i < CONTAIN_SHIPPED_N; i++)
        if (keepable(&CONTAIN_SHIPPED[i].lv, only, caps)) return CONTAIN_SHIPPED[i].name;
    return NULL;
}

int contain_keepable(const Containment *c, unsigned caps) {
    return c->known && keepable(&c->lv, 0, caps);
}

const char *contain_suggest(unsigned caps) { return suggest(0, caps); }

int contain_plan(const Containment *c, ExecVehicle v, unsigned caps,
                 ContainPlan *out, char *why, size_t cap) {
    if (why && cap) why[0] = '\0';
    if (!c->known) {
        memset(out, 0, sizeof(*out));
        if (why) snprintf(why, cap, "error: this agent's containment '%s' is not a row of"
                          " the containments table, so its tool calls are refused —"
                          " the operator must set the agent's containment to a row"
                          " that exists (cclaw --doctor lists them)", c->name);
        return -1;
    }
    const char *lack = plan_levels(&c->lv, v, caps, out);
    if (!lack) return 0;
    if (why) {
        const char *alt = suggest(v, caps);
        snprintf(why, cap, "error: containment '%s' cannot be kept for %s on this"
                 " host: %s. The call was refused rather than run with less"
                 " containment. This is a host or configuration matter for the"
                 " operator, not something a retry fixes%s%s%s", c->name,
                 vehicle_noun(v), lack, alt ? "; a shipped containment this host"
                 " can keep for it is '" : "", alt ? alt : "", alt ? "'" : "");
    }
    return -1;
}

/* ── FE_CODE: the path check ──────────────────────────────────────────── */

/* Armed once per child process, which lives for one tool call. The roots are
 * what the mount namespace would bind: workspace and scratch rw, path grants
 * with their ro/rw, the system directories ro. The key and the DB family are
 * never reachable, by resolved path and by inode (a hard link has no other
 * name to compare). */
static struct {
    int armed;
    SandboxMount *roots;
    size_t n;
    char state[4][PATH_MAX + 16];
    struct { dev_t dev; ino_t ino; int set; } ino[4];
} g_conf;

static int path_within(const char *base, const char *p) {
    size_t n = strlen(base);
    if (n == 1 && base[0] == '/') return 1;
    return strncmp(p, base, n) == 0 && (p[n] == '\0' || p[n] == '/');
}

/* realpath of the deepest existing ancestor plus the part that does not
 * exist yet (a file about to be created). A symlink anywhere in the existing
 * part resolves to where it really points; a '..' in the missing part is
 * refused, since it could climb back out once created. */
static int resolve_for_check(const char *path, char *out) {
    char abs[PATH_MAX], head[PATH_MAX], cwd[PATH_MAX];
    if (path[0] == '/') snprintf(abs, sizeof(abs), "%s", path);
    else if (!getcwd(cwd, sizeof(cwd)) ||
             snprintf(abs, sizeof(abs), "%s/%s", cwd, path) >= (int)sizeof(abs))
        return -1;
    snprintf(head, sizeof(head), "%s", abs);
    const char *tail = "";
    while (!realpath(head, out)) {
        if (errno != ENOENT) return -1;
        char *sl = strrchr(head, '/');
        if (!sl) return -1;
        tail = abs + (sl - head) + 1;
        if (sl == head) { if (!realpath("/", out)) return -1; break; }
        *sl = '\0';
    }
    for (const char *s = tail; *s; ) {
        size_t len = strcspn(s, "/");
        if (len == 2 && s[0] == '.' && s[1] == '.') return -1;
        s += len + (s[len] == '/');
    }
    if (tail[0]) {
        size_t o = strlen(out);
        if (snprintf(out + o, PATH_MAX - o, "%s%s", o > 1 ? "/" : "", tail)
                >= (int)(PATH_MAX - o))
            return -1;
    }
    return 0;
}

int contain_path_check(const char *path, int write) {
    if (!g_conf.armed) return 0;
    char r[PATH_MAX];
    if (!path || resolve_for_check(path, r) != 0) goto deny;
    for (int i = 0; i < 4; i++)
        if (strcmp(r, g_conf.state[i]) == 0) goto deny;
    struct stat st;
    if (stat(r, &st) == 0)
        for (int i = 0; i < 4; i++)
            if (g_conf.ino[i].set && g_conf.ino[i].dev == st.st_dev &&
                g_conf.ino[i].ino == st.st_ino) goto deny;
    /* Shallow→deep, so the deepest root holding the path decides — a read
     * grant inside a writable one is read-only there, as its mount would be. */
    int ok = 0;
    for (size_t i = 0; i < g_conf.n; i++)
        if (path_within(g_conf.roots[i].path, r))
            ok = !write || !g_conf.roots[i].ro;
    if (ok) return 0;
deny:
    errno = EACCES;
    return -1;
}

static int confine_arm(const SandboxConfig *cfg, const char *ws) {
    size_t n = cfg->extra_mount_count + 10;
    SandboxMountReq *req = calloc(n, sizeof(*req));
    g_conf.roots = calloc(n, sizeof(*g_conf.roots));
    if (!req || !g_conf.roots) { free(req); return -1; }
    size_t k = 0;
    if (ws && ws[0]) req[k++] = (SandboxMountReq){ ws, 0 };
    if (cfg->tmp_dir && cfg->tmp_dir[0]) req[k++] = (SandboxMountReq){ cfg->tmp_dir, 0 };
    for (int i = 0; SANDBOX_SYS_DIRS[i]; i++)
        req[k++] = (SandboxMountReq){ SANDBOX_SYS_DIRS[i], 1 };
    for (size_t i = 0; i < cfg->extra_mount_count; i++) req[k++] = cfg->extra_mounts[i];
    g_conf.n = sandbox_plan_mounts(req, k, g_conf.roots);
    free(req);

    /* The key sits beside the DB (secret.c); the DB family is it plus -wal/-shm. */
    const char *db = cfg->db_path;
    if (!db || !db[0]) return -1;   /* nothing known to keep out: refuse */
    char dir[PATH_MAX], real[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", db);
    char *sl = strrchr(dir, '/');
    const char *base = sl ? db + (sl - dir) + 1 : db;
    if (!sl) snprintf(dir, sizeof(dir), ".");
    else if (sl == dir) sl[1] = '\0';
    else *sl = '\0';
    if (!realpath(dir, real)) return -1;
    snprintf(g_conf.state[0], sizeof(g_conf.state[0]), "%s/.cclaw_key", real);
    snprintf(g_conf.state[1], sizeof(g_conf.state[1]), "%s/%s", real, base);
    snprintf(g_conf.state[2], sizeof(g_conf.state[2]), "%s/%s-wal", real, base);
    snprintf(g_conf.state[3], sizeof(g_conf.state[3]), "%s/%s-shm", real, base);
    for (int i = 0; i < 4; i++) {
        struct stat st;
        if (stat(g_conf.state[i], &st) == 0)
            g_conf.ino[i].dev = st.st_dev, g_conf.ino[i].ino = st.st_ino, g_conf.ino[i].set = 1;
    }
    g_conf.armed = 1;
    return 0;
}

/* FE_LANDLOCK: the same roots, as kernel rules. /dev is not one: only the
 * nodes every program expects, read-write (never /dev/shm). /proc is
 * readable: tools need /proc/self, and Landlock's ptrace scoping refuses
 * other processes' sensitive files (environ, fd, mem) there. The key and the
 * DB are in no rule, so they cannot be opened. */
static int confine_landlock(void) {
    static const char *const extra[] = { "/dev/null", "/dev/zero", "/dev/full",
        "/dev/random", "/dev/urandom", "/dev/tty", "/proc", NULL };
    SandboxMount *r = calloc(g_conf.n + 7, sizeof(*r));
    if (!r) return -1;
    size_t k = 0;
    for (size_t i = 0; i < g_conf.n; i++)
        if (strcmp(g_conf.roots[i].path, "/dev") != 0) r[k++] = g_conf.roots[i];
    for (int i = 0; extra[i]; i++, k++) {
        snprintf(r[k].path, sizeof(r[k].path), "%s", extra[i]);
        r[k].ro = extra[i][1] == 'p' ? CONTAIN_LL_RO : CONTAIN_LL_DEV;
    }
    int rc = contain_landlock(r, k);
    free(r);
    return rc;
}

/* ── Child setup ──────────────────────────────────────────────────────── */

/* HOME: with any files enforcer, the agent workspace (`ws`, canonical — the
 * path the bind and the chdir use), the one writable directory the agent owns,
 * so user-level installs persist and are on PATH next call. Under files
 * 'open', the user's home from passwd: the child's environ starts empty. */
static const char *sandbox_resolve_home(const SandboxConfig *cfg, const char *ws) {
    if (cfg->plan.fe == FE_NONE) {
        struct passwd *pw = getpwuid(getuid());
        if (pw && pw->pw_dir && pw->pw_dir[0]) return pw->pw_dir;
    }
    if (ws && ws[0]) return ws;
    return "/tmp";
}

/* PATH: the agent's own workspace-local bin dirs first, then the system dirs
 * — node, go, cargo and rustc all install outside /bin:/usr/bin. */
static void sandbox_set_path(const char *home) {
    static const char *sys =
        "/usr/local/sbin:/usr/local/bin:/usr/local/go/bin:"
        "/usr/sbin:/usr/bin:/sbin:/bin";
    char buf[PATH_MAX * 3 + 128];
    int n = snprintf(buf, sizeof(buf), "%s/.local/bin:%s/.cargo/bin:%s/bin:%s",
                     home, home, home, sys);
    setenv("PATH", (n > 0 && (size_t)n < sizeof(buf)) ? buf : sys, 1);
}

/* The child's environment, under every plan: wiped (execve gave it only the
 * log level and the js heap size), then PATH, TMPDIR and HOME. The heap size
 * is the parent's resolved js_heap_mb and is read after this, so it stays. */
static void sandbox_scrub_env(const SandboxConfig *cfg, const char *ws) {
    extern char **environ;
    char heap[16] = "";
    const char *h = getenv("CCLAW_JS_HEAP_MB");
    if (h) snprintf(heap, sizeof(heap), "%s", h);
    if (environ) environ[0] = NULL;  /* portable clearenv */
    if (heap[0]) setenv("CCLAW_JS_HEAP_MB", heap, 1);
    const char *home = sandbox_resolve_home(cfg, ws);
    sandbox_set_path(home);
    setenv("HOME", home, 1);
    const char *tmp = "/tmp";
    if (cfg->plan.fe == FE_CODE || cfg->plan.fe == FE_LANDLOCK)
        tmp = cfg->tmp_dir && cfg->tmp_dir[0] ? cfg->tmp_dir : home;
    setenv("TMPDIR", tmp, 1);
}

/* No row sets RLIMIT_AS: it caps address space, which every modern runtime
 * reserves far beyond its footprint (Go arenas, V8 cages), so a cap loose
 * enough to be safe bounds nothing. NPROC and CPU are what bound a runaway. */
static void sandbox_apply_rlimits(const ContainLevels *lv) {
    if (lv->nproc > 0) {
        struct rlimit rl = {(rlim_t)lv->nproc, (rlim_t)lv->nproc};
        setrlimit(RLIMIT_NPROC, &rl);
    }
    if (lv->cpu_sec > 0) {
        struct rlimit rl = {(rlim_t)lv->cpu_sec, (rlim_t)lv->cpu_sec};
        setrlimit(RLIMIT_CPU, &rl);
    }
}

int sandbox_child_setup(const SandboxConfig *cfg) {
    const ContainPlan *p = &cfg->plan;
    const char *ws = cfg->workspace;

    /* Canonicalize the workspace once, here, while the host filesystem is still
     * reachable: the bind mount, the chdir, HOME and the path check all use the
     * resolved path (a HOME with a symlink component names nothing inside the
     * namespace). A realpath failure leaves the raw path — no new failure mode. */
    char ws_abs[PATH_MAX];
    if (ws && ws[0] && realpath(ws, ws_abs)) ws = ws_abs;

    /* No privilege gain: a setuid or file-capability binary the payload runs
     * gets the caller's own rights. Inherited across fork and exec, never
     * cleared. Not for files 'open' with network 'direct' — containment
     * `none`, which promises the operator's own reach, sudo included. */
    if ((p->lv.files != FILES_OPEN || p->lv.net != NET_DIRECT) &&
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        fprintf(stderr, "error: could not set no_new_privs, refusing to run\n");
        return -1;
    }

    if (p->fe == FE_MOUNTNS) {
        if (contain_enter(ws, cfg->db_path, cfg) != 0) {
            int e = errno;  /* log call below may clobber it */
            LOG_INFO_("sandbox namespace_fail errno=%d action=abort", e);
            fprintf(stderr, "error: namespace sandbox unavailable (errno=%d)\n", e);
            return -1;
        }
    } else if (p->ne == NE_NETNS && contain_enter_net() != 0) {
        fprintf(stderr, "error: network namespace unavailable (errno=%d)\n", errno);
        return -1;
    } else if (!cfg->cwd_path && ws && ws[0] && chdir(ws) != 0) {
        /* Outside the mount namespace, start in the workspace — except a CLI
         * run under files 'open', where the user's CWD is the point (`cclaw`
         * in a repo operates on that repo). */
        LOG_INFO_("sandbox chdir_fail errno=%d", errno);
    }
    if ((p->fe == FE_CODE || p->fe == FE_LANDLOCK) && confine_arm(cfg, ws) != 0) {
        fprintf(stderr, "error: the path check could not be set up\n");
        return -1;
    }

    sandbox_scrub_env(cfg, ws);
    if (cfg->llm_sock && cfg->llm_sock[0])
        setenv("CCLAW_LLM_SOCK",
               p->fe == FE_MOUNTNS ? contain_llm_sock_path() : cfg->llm_sock, 1);
    sandbox_apply_rlimits(&p->lv);

    int port = 0;
    if (cfg->proxy_sock) {
        if (p->ne == NE_CODE) {
            setenv("CCLAW_PROXY_SOCK", cfg->proxy_sock, 1);
        } else if (p->ne == NE_NETNS || p->ne == NE_ENV || p->ne == NE_NOTIF) {
            /* In the namespace a failure leaves no network (soft); on the
             * host it would leave the proxy variables unset while nothing
             * else holds the program back (NE_ENV), or no way out at all
             * (NE_NOTIF), so it refuses. */
            port = contain_egress(cfg);
            if (port < 0 && p->ne != NE_NETNS) {
                fprintf(stderr, "error: the loopback proxy could not be started\n");
                return -1;
            }
        }
    }

    /* Last, once the shim (ours, needing its socket) is forked off. Under
     * Landlock there is no PID namespace: ptrace and process_vm_* are refused
     * here as well as by Landlock's own scoping — and under NE_NOTIF, where a
     * traced process outside the filter would connect for the caller. */
    if (p->fe == FE_LANDLOCK && confine_landlock() != 0) {
        fprintf(stderr, "error: the Landlock ruleset could not be applied (errno=%d)\n", errno);
        return -1;
    }
    unsigned sc = (p->fe == FE_LANDLOCK || p->ne == NE_NOTIF ? CONTAIN_SC_NOTRACE : 0) |
                  (p->ne == NE_SECCOMP ? CONTAIN_SC_NOSOCKET : 0) |
                  (p->ne == NE_NOTIF ? CONTAIN_SC_INETSTREAM : 0) |
                  (p->ne == NE_NOTIF && p->lv.net == NET_FIREWALLED ? CONTAIN_SC_NOLISTEN : 0);
    int rc = p->ne == NE_NOTIF || p->transparent
        ? contain_intercept(cfg, sc, port > 0 ? port : 0)
        : sc ? contain_seccomp(sc) : 0;
    if (rc != 0) {
        fprintf(stderr, "error: the seccomp filter could not be installed (errno=%d)\n", errno);
        return -1;
    }
    return 0;
}
