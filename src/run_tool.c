#define _GNU_SOURCE
#include "run_tool.h"
#include "tool_file.h"
#include "proxy.h"
#include "sandbox.h"
#include "db.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FD_REQUEST RUNTOOL_FD_REQUEST

/* Tier discriminator (first body byte). */
#define TIER_FILE  0
#define TIER_SHELL 1

/* ── Flat binary wire format ────────────────────────────────────────────
 * Both ends are the same binary, so the request is not JSON: there is no
 * escaping, no key/value scan, no schema flexibility to pay for. Fields are
 * written and read in a fixed order known to both sides.
 *
 * Primitives: u32 = 4-byte little-endian. str = u32 length + that many raw
 * bytes (length 0 means NULL/absent). Secret values cross this boundary as
 * raw bytes — no escape/unescape round-trip touches the plaintext.
 *
 * Frame: 4-byte LE body-length prefix, then a tier byte, then the body. */

/* ── Writer (parent side) ── */
typedef struct { unsigned char *p; size_t cap; size_t len; int err; } Wbuf;

static void w_bytes(Wbuf *b, const void *src, size_t n) {
    if (b->err || b->len + n > b->cap) { b->err = 1; return; }
    memcpy(b->p + b->len, src, n);
    b->len += n;
}
static void w_u8(Wbuf *b, unsigned char v) { w_bytes(b, &v, 1); }
static void w_u32(Wbuf *b, uint32_t v) {
    unsigned char t[4] = { (unsigned char)v, (unsigned char)(v >> 8),
                           (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    w_bytes(b, t, 4);
}
static void w_str(Wbuf *b, const char *s) {
    size_t n = s ? strlen(s) : 0;
    w_u32(b, (uint32_t)n);
    if (n) w_bytes(b, s, n);
}
static void w_str_array(Wbuf *b, const char **arr, size_t count) {
    w_u32(b, (uint32_t)count);
    for (size_t i = 0; i < count; i++) w_str(b, arr ? arr[i] : NULL);
}

/* Backfill the 4-byte body-length prefix. Returns total blob size, 0 on
 * overflow/empty (caller frees and returns NULL). */
static size_t w_finalize(Wbuf *b) {
    if (b->err) return 0;
    size_t body = b->len - 4;
    if (body == 0 || body + 4 > RUNTOOL_REQUEST_MAX) return 0;
    uint32_t l = (uint32_t)body;
    b->p[0] = (unsigned char)l;        b->p[1] = (unsigned char)(l >> 8);
    b->p[2] = (unsigned char)(l >> 16); b->p[3] = (unsigned char)(l >> 24);
    return b->len;
}

char *run_tool_serialize_file_request(
    const char *tool_name, const char *arguments, const char *workspace,
    const char **read_paths, size_t read_count,
    const char **write_paths, size_t write_count,
    int workspace_ro, int mount_cwd, const char *cwd_path,
    int env_mode, int nproc, int as_mb, int cpu_sec,
    size_t *out_len)
{
    unsigned char *blob = malloc(RUNTOOL_REQUEST_MAX);
    if (!blob) return NULL;
    Wbuf b = { .p = blob, .cap = RUNTOOL_REQUEST_MAX, .len = 4 };

    w_u8(&b, TIER_FILE);
    w_str(&b, tool_name);
    w_str(&b, arguments ? arguments : "{}");
    w_str(&b, workspace);
    w_str(&b, mount_cwd ? cwd_path : NULL);
    w_u32(&b, (uint32_t)workspace_ro);
    w_u32(&b, (uint32_t)mount_cwd);
    w_u32(&b, (uint32_t)env_mode);
    w_u32(&b, (uint32_t)nproc);
    w_u32(&b, (uint32_t)as_mb);
    w_u32(&b, (uint32_t)cpu_sec);
    w_str_array(&b, read_paths, read_count);
    w_str_array(&b, write_paths, write_count);

    size_t total = w_finalize(&b);
    if (total == 0) { free(blob); return NULL; }
    *out_len = total;
    return (char *)blob;
}

char *run_tool_serialize_shell_request(
    const char *command, int timeout,
    const char *workspace, const char *cwd_path, const char *agent_dir,
    const char **host_rules, size_t host_count,
    const RunToolSecret *secrets, size_t secret_count,
    int sandbox, int env_mode, int net_mode, int mount_cwd,
    int workspace_ro, int nproc, int as_mb, int cpu_sec,
    const char **read_paths, size_t read_count,
    const char **write_paths, size_t write_count,
    size_t *out_len)
{
    unsigned char *blob = malloc(RUNTOOL_REQUEST_MAX);
    if (!blob) return NULL;
    Wbuf b = { .p = blob, .cap = RUNTOOL_REQUEST_MAX, .len = 4 };

    w_u8(&b, TIER_SHELL);
    w_str(&b, command);
    w_u32(&b, (uint32_t)timeout);
    w_str(&b, workspace);
    w_str(&b, cwd_path);
    w_str(&b, agent_dir);
    w_u32(&b, (uint32_t)sandbox);
    w_u32(&b, (uint32_t)env_mode);
    w_u32(&b, (uint32_t)net_mode);
    w_u32(&b, (uint32_t)mount_cwd);
    w_u32(&b, (uint32_t)workspace_ro);
    w_u32(&b, (uint32_t)nproc);
    w_u32(&b, (uint32_t)as_mb);
    w_u32(&b, (uint32_t)cpu_sec);
    w_str_array(&b, host_rules, host_count);

    /* secrets: u32 count, then count × (name, value) */
    w_u32(&b, (uint32_t)secret_count);
    for (size_t i = 0; i < secret_count; i++) {
        w_str(&b, secrets[i].name);
        w_str(&b, secrets[i].value);
    }

    w_str_array(&b, read_paths, read_count);
    w_str_array(&b, write_paths, write_count);

    size_t total = w_finalize(&b);
    if (total == 0) { free(blob); return NULL; }
    *out_len = total;
    return (char *)blob;
}

/* ── Reader (child side) ── */
typedef struct { const unsigned char *p; size_t rem; int err; } Rbuf;

static uint32_t r_u32(Rbuf *r) {
    if (r->err || r->rem < 4) { r->err = 1; return 0; }
    uint32_t v = (uint32_t)r->p[0] | ((uint32_t)r->p[1] << 8) |
                 ((uint32_t)r->p[2] << 16) | ((uint32_t)r->p[3] << 24);
    r->p += 4; r->rem -= 4;
    return v;
}
static unsigned char r_u8(Rbuf *r) {
    if (r->err || r->rem < 1) { r->err = 1; return 0; }
    unsigned char v = r->p[0];
    r->p += 1; r->rem -= 1;
    return v;
}
/* malloc'd NUL-terminated copy of the next str field; NULL if length 0 or on
 * truncation (which also latches r->err). */
static char *r_str(Rbuf *r) {
    uint32_t n = r_u32(r);
    if (r->err || n > r->rem) { r->err = 1; return NULL; }
    if (n == 0) return NULL;
    char *s = malloc((size_t)n + 1);
    if (!s) { r->err = 1; return NULL; }
    memcpy(s, r->p, n);
    s[n] = '\0';
    r->p += n; r->rem -= n;
    return s;
}
static char **r_str_array(Rbuf *r, size_t *count) {
    *count = 0;
    uint32_t n = r_u32(r);
    if (r->err || n == 0) return NULL;
    if (n > RUNTOOL_REQUEST_MAX / 4) { r->err = 1; return NULL; } /* sane bound */
    char **a = calloc(n, sizeof(char *));
    if (!a) { r->err = 1; return NULL; }
    for (uint32_t i = 0; i < n; i++) a[i] = r_str(r);
    *count = n;
    return a;
}

/* ── Child side (runs in the re-exec'd --run-tool process) ──────────── */

/* Read exactly n bytes from fd. Returns 0 on success, -1 on short/error. */
static int read_exact(int fd, void *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(fd, (char *)buf + done, n - done);
        if (r <= 0) return -1;
        done += (size_t)r;
    }
    return 0;
}

/* Write all bytes to fd. Returns 0 on success. */
static int write_all(int fd, const void *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, (const char *)buf + done, n - done);
        if (w <= 0) return -1;
        done += (size_t)w;
    }
    return 0;
}

static void die(const char *msg) {
    write_all(FD_REQUEST, msg, strlen(msg));
    _exit(1);
}

/* Build the read-ro / write-rw bind-mount list from path grants.
 * Returns malloc'd array (caller frees), *out_count set; NULL if none. */
static SandboxMountReq *build_extra_mounts(char **read_paths, size_t read_count,
                                           char **write_paths, size_t write_count,
                                           size_t *out_count) {
    *out_count = 0;
    size_t n = read_count + write_count;
    if (n == 0) return NULL;
    SandboxMountReq *extra = calloc(n, sizeof(*extra));
    if (!extra) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < read_count; i++)  { extra[j].path = read_paths[i];  extra[j].ro = 1; j++; }
    for (size_t i = 0; i < write_count; i++) { extra[j].path = write_paths[i]; extra[j].ro = 0; j++; }
    *out_count = n;
    return extra;
}

/* Dispatch file tool by name. ctx->sandbox==0 so handlers run in-process. */
static char *dispatch_file(const char *tool_name, const char *arguments, FileReadCtx *ctx) {
    typedef char *(*handler_fn)(const char *, void *);
    struct { const char *name; handler_fn fn; } tools[] = {
        {"file_read",  tool_file_read_handler},
        {"file_write", tool_file_write_handler},
        {"file_edit",  tool_file_edit_handler},
        {"file_list",  tool_file_list_handler},
        {"file_find",  tool_file_find_handler},
        {"file_grep",  tool_file_grep_handler},
    };
    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++)
        if (strcmp(tool_name, tools[i].name) == 0)
            return tools[i].fn(arguments, ctx);
    return strdup("error: unknown file tool");
}

/* ── Runtime enforcement gate ───────────────────────────────────────────
 * Verify the --run-tool process is clean before dispatching any handler.
 * Stray fds are already closed by run_tool_main (the blanket 4..maxfd close),
 * and no DB is ever opened in this process (early intercept in main bypasses
 * all DB init), so the one runtime invariant left to assert is key-absence.
 * Fails closed — a production guard, not an assert. */
static void run_tool_verify_clean(void) {
    if (db_secret_key_loaded())
        die("error: run-tool: secret key present, refusing");
}

/* ── Shell tier: broker + sandbox child ─────────────────────────────────
 * proxy_bind (single-threaded, before fork) → fork → child (sandbox + exec) →
 * proxy_serve → drain → waitpid/kill → proxy_stop. The bind-before-fork-
 * before-serve ordering is load-bearing. The broker IS this --run-tool
 * process. A broker is interposed IFF the tier needs gated egress. */

#define SHELL_MAX_OUTPUT (256 * 1024)

typedef struct { char *name; char *value; } ParsedSecret;

static void dispatch_shell(Rbuf *r, unsigned char *body, size_t body_len) {
    char *command   = r_str(r);
    int   timeout   = (int)r_u32(r);
    char *workspace = r_str(r);
    char *cwd_path  = r_str(r);
    char *agent_dir = r_str(r);
    int   sandbox      = (int)r_u32(r);
    int   env_mode     = (int)r_u32(r);
    int   net_mode     = (int)r_u32(r);
    int   mount_cwd    = (int)r_u32(r);
    int   workspace_ro = (int)r_u32(r);
    int   nproc        = (int)r_u32(r);
    int   as_mb        = (int)r_u32(r);
    int   cpu_sec      = (int)r_u32(r);

    size_t host_count = 0;
    char **host_rules = r_str_array(r, &host_count);

    /* secrets: count, then count × (name, value) */
    uint32_t sc = r_u32(r);
    ParsedSecret *secrets = NULL;
    size_t secret_count = 0;
    if (!r->err && sc > 0 && sc <= RUNTOOL_REQUEST_MAX / 4) {
        secrets = calloc(sc, sizeof(*secrets));
        if (secrets) {
            for (uint32_t i = 0; i < sc; i++) {
                secrets[i].name  = r_str(r);
                secrets[i].value = r_str(r);
            }
            secret_count = sc;
        }
    }

    size_t read_count = 0, write_count = 0;
    char **read_paths  = r_str_array(r, &read_count);
    char **write_paths = r_str_array(r, &write_count);

    if (r->err || !command) die("error: shell: malformed request");
    if (timeout <= 0) timeout = 30;

    /* Pipe for child stdout+stderr */
    int pipefd[2];
    if (pipe(pipefd) != 0) die("error: shell: pipe failed");

    /* Proxy: bind while single-threaded (before the fork below).
     * Skipped if no agent_dir or net_mode==1 (no network). */
    ProxyContext proxy;
    int proxy_active = 0;
    if (agent_dir && !net_mode && sandbox) {
        if (proxy_bind(&proxy, agent_dir, host_rules, host_count) == 0)
            proxy_active = 1;
    }
    const char *psock = proxy_active ? proxy_sock_path(&proxy) : NULL;

    pid_t pid = fork();
    if (pid < 0) {
        if (proxy_active) proxy_stop(&proxy);
        die("error: shell: fork failed");
    }

    if (pid == 0) {
        /* SANDBOX CHILD */
        setpgid(0, 0);
        /* Die if the broker dies (e.g. the daemon SIGKILLs it on the backstop
         * deadline): the broker can no longer kill our process group, so tie our
         * lifetime to it. sandbox_apply_namespace re-arms this on the inner
         * PID-namespace init so the whole subtree unwinds. */
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        SandboxConfig cfg = {0};
        cfg.workspace    = workspace;
        cfg.cwd_path     = cwd_path;
        cfg.db_path      = NULL;
        cfg.env_file     = NULL;
        cfg.proxy_sock   = psock;
        cfg.sandbox      = sandbox;
        cfg.workspace_ro = workspace_ro;
        cfg.mount_cwd    = mount_cwd;
        cfg.env_mode     = env_mode;
        cfg.net_mode     = net_mode;
        cfg.skip_pid_ns  = 0;  /* shell: PID ns + /proc */
        cfg.rlimits.nproc   = nproc;
        cfg.rlimits.as_mb   = as_mb;
        cfg.rlimits.cpu_sec = cpu_sec;

        size_t n_extra = 0;
        SandboxMountReq *extra = build_extra_mounts(read_paths, read_count,
                                                    write_paths, write_count, &n_extra);
        cfg.extra_mounts = extra;
        cfg.extra_mount_count = n_extra;

        if (sandbox_child_setup(&cfg) != 0) {
            fprintf(stderr, "error: namespace sandbox failed\n");
            _exit(126);
        }

        /* Inject secrets into env — only after env scrub (ordering matters).
         * These are the minimal set pre-filtered by the parent. */
        for (size_t i = 0; i < secret_count; i++) {
            if (secrets[i].name && secrets[i].value) {
                char envname[256];
                snprintf(envname, sizeof(envname), "CCLAW_SECRET_%s", secrets[i].name);
                setenv(envname, secrets[i].value, 1);
            }
        }
        /* Wipe secret values from child memory before exec (values are in env now) */
        for (size_t i = 0; i < secret_count; i++)
            if (secrets[i].value) explicit_bzero(secrets[i].value, strlen(secrets[i].value));

        /* The one inner exec — shell is the only tier with a foreign program.
         * command carries interpolated secrets — exec replaces the address
         * space, so they don't persist in this process after execl. */
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    /* ── BROKER (parent of sandbox child) ── */
    setpgid(pid, pid);
    close(pipefd[1]);

    /* The broker is an unsandboxed process that outlives the fork for the whole
     * command duration. It needs none of the secret material post-fork (the
     * child has its own COW copies), so scrub the broker's plaintext now to
     * shrink the window a core dump / same-uid /proc read could expose:
     * the interpolated command, the secret values, and the raw request body
     * (which still holds every field verbatim). */
    if (command) explicit_bzero(command, strlen(command));
    for (size_t i = 0; i < secret_count; i++)
        if (secrets[i].value) explicit_bzero(secrets[i].value, strlen(secrets[i].value));
    if (body && body_len) explicit_bzero(body, body_len);

    /* Start proxy accept thread AFTER the single-threaded fork */
    if (proxy_active) {
        if (proxy_serve(&proxy) != 0) { proxy_stop(&proxy); proxy_active = 0; }
    }

    /* Drain child stdout with timeout */
    char *output = malloc(SHELL_MAX_OUTPUT + 1);
    if (!output) {
        kill(-pid, SIGKILL); waitpid(pid, NULL, 0);
        if (proxy_active) proxy_stop(&proxy);
        die("error: shell: OOM");
    }
    size_t out_len = 0;
    int timed_out = 0, status = 0;
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout;

    while (1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            timed_out = 1; break;
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(pipefd[0], &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int sel = select(pipefd[0] + 1, &rfds, NULL, NULL, &tv);
        if (sel > 0) {
            ssize_t n = read(pipefd[0], output + out_len, SHELL_MAX_OUTPUT - out_len);
            if (n <= 0) break;
            out_len += (size_t)n;
            if (out_len >= SHELL_MAX_OUTPUT) break;
        } else if (sel < 0 && errno != EINTR) break;
        int wr = waitpid(pid, &status, WNOHANG);
        if (wr > 0) {
            while (out_len < SHELL_MAX_OUTPUT) {
                ssize_t n = read(pipefd[0], output + out_len, SHELL_MAX_OUTPUT - out_len);
                if (n <= 0) break;
                out_len += (size_t)n;
            }
            close(pipefd[0]);
            goto shell_format;
        }
    }
    close(pipefd[0]);

    if (timed_out) {
        kill(-pid, SIGKILL);
        waitpid(pid, NULL, 0);
    } else {
        waitpid(pid, &status, 0);
    }

shell_format:
    if (proxy_active) proxy_stop(&proxy);
    output[out_len] = '\0';

    size_t needed = out_len + 128;
    char *result = malloc(needed);
    if (timed_out)
        snprintf(result, needed, "[timeout after %ds]\n%s", timeout, output);
    else
        snprintf(result, needed, "[exit %d]\n%s",
                 WIFEXITED(status) ? WEXITSTATUS(status) : -1, output);
    free(output);

    write_all(FD_REQUEST, result, strlen(result));
    free(result);
    _exit(0);
}

int run_tool_main(void) {
    /* Verify fd 3 is open */
    if (fcntl(FD_REQUEST, F_GETFD) < 0)
        _exit(1);

    /* fd hygiene: close anything beyond 0,1,2,3. This — not a later audit —
     * is what guarantees no O_CLOEXEC leak from the daemon survives. Prefer
     * close_range() (one syscall, Linux 5.9+); the per-fd loop up to
     * _SC_OPEN_MAX is the fallback — and on a high RLIMIT_NOFILE that ceiling
     * can be ~1M wasted close() calls, so it is genuinely a fallback. */
    int closed = 0;
#if defined(__linux__) && defined(SYS_close_range)
    if (syscall(SYS_close_range, 4, ~0U, 0) == 0)
        closed = 1;
#endif
    if (!closed) {
        long maxfd = sysconf(_SC_OPEN_MAX);
        if (maxfd < 0) maxfd = 1024;
        for (int fd = 4; fd < (int)maxfd; fd++)
            close(fd);
    }

    /* Verify process is clean (no master key) */
    run_tool_verify_clean();

    /* Read 4-byte LE body-length prefix */
    uint8_t lenbuf[4];
    if (read_exact(FD_REQUEST, lenbuf, 4) != 0)
        die("error: failed to read request length");
    uint32_t body_len = (uint32_t)lenbuf[0] | ((uint32_t)lenbuf[1] << 8) |
                        ((uint32_t)lenbuf[2] << 16) | ((uint32_t)lenbuf[3] << 24);
    if (body_len == 0 || body_len > RUNTOOL_REQUEST_MAX - 4)
        die("error: request length out of bounds");

    unsigned char *body = malloc(body_len);
    if (!body) die("error: OOM");
    if (read_exact(FD_REQUEST, body, body_len) != 0)
        die("error: short read on request body");

    Rbuf r = { .p = body, .rem = body_len, .err = 0 };
    unsigned char tier = r_u8(&r);

    if (tier == TIER_SHELL) {
        dispatch_shell(&r, body, body_len);   /* calls _exit — never returns */
    }
    if (tier != TIER_FILE)
        die("error: unsupported tier");

    /* File tier — fields in serialization order */
    char *tool_name  = r_str(&r);
    char *arguments  = r_str(&r);
    char *workspace  = r_str(&r);
    char *cwd_path   = r_str(&r);
    int workspace_ro = (int)r_u32(&r);
    int mount_cwd    = (int)r_u32(&r);
    int env_mode     = (int)r_u32(&r);
    int nproc        = (int)r_u32(&r);
    int as_mb        = (int)r_u32(&r);
    int cpu_sec      = (int)r_u32(&r);
    size_t read_count = 0, write_count = 0;
    char **read_paths  = r_str_array(&r, &read_count);
    char **write_paths = r_str_array(&r, &write_count);

    if (r.err) die("error: malformed request");
    if (!tool_name) die("error: missing tool_name");
    if (!workspace) die("error: missing workspace");
    if (!arguments) arguments = strdup("{}");

    /* Build SandboxConfig — skip_pid_ns=1 for file tier (no inner fork) */
    SandboxConfig cfg = {0};
    cfg.workspace    = workspace;
    cfg.cwd_path     = cwd_path;
    cfg.db_path      = NULL;  /* no DB in this process */
    cfg.sandbox      = 1;
    cfg.workspace_ro = workspace_ro;
    cfg.mount_cwd    = mount_cwd;
    cfg.env_mode     = env_mode;
    cfg.net_mode     = 1;  /* file tier: no network */
    cfg.skip_pid_ns  = 1;  /* no CLONE_NEWPID, no inner fork */
    cfg.proxy_sock   = NULL;
    cfg.rlimits.nproc   = nproc;
    cfg.rlimits.as_mb   = as_mb;
    cfg.rlimits.cpu_sec = cpu_sec;

    size_t n_extra = 0;
    SandboxMountReq *extra = build_extra_mounts(read_paths, read_count,
                                                write_paths, write_count, &n_extra);
    cfg.extra_mounts = extra;
    cfg.extra_mount_count = n_extra;

    /* Apply sandbox ON THIS PROCESS. Fail closed. */
    if (sandbox_child_setup(&cfg) != 0)
        die("error: namespace sandbox setup failed");

    /* Sandbox is now active. Dispatch the file handler in-process — sandbox=0
     * so it runs directly (the kernel mount view IS the security boundary). */
    FileReadCtx fctx = {0};
    fctx.workspace    = workspace;
    fctx.cwd_path     = cwd_path;
    fctx.sandbox      = 0;
    fctx.workspace_ro = workspace_ro;
    fctx.mount_cwd    = mount_cwd;
    fctx.read_only    = workspace_ro;
    fctx.read_paths   = read_paths;
    fctx.read_path_count = read_count;
    fctx.write_paths  = write_paths;
    fctx.write_path_count = write_count;

    char *result = dispatch_file(tool_name, arguments, &fctx);
    if (!result) result = strdup("");

    write_all(FD_REQUEST, result, strlen(result));
    free(result);
    _exit(0);
}
