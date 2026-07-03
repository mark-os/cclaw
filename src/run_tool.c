#define _GNU_SOURCE
#include "run_tool.h"
#include "tool_file.h"
#include "tool_shell.h"
#include "tool_web_fetch.h"
#include "tool_js.h"
#include "proxy.h"
#include "sandbox.h"
#include "db.h"
#include "log.h"

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

/* ── Flat binary wire format ────────────────────────────────────────────
 * Both ends are the same binary, so the request is not JSON: there is no
 * escaping, no key/value scan, no schema flexibility to pay for. Fields are
 * written and read in ONE fixed superset order; the tier byte drives dispatch,
 * not layout (write-all-always: a tier's unused fields are NULL/0/empty).
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

char *run_tool_serialize_request(const RunToolReq *req, size_t *out_len) {
    unsigned char *blob = malloc(RUNTOOL_REQUEST_MAX);
    if (!blob) return NULL;
    Wbuf b = { .p = blob, .cap = RUNTOOL_REQUEST_MAX, .len = 4 };

    w_u8(&b, (unsigned char)req->tier);
    w_str(&b, req->tool_name);
    w_str(&b, req->arguments ? req->arguments : "{}");
    w_u32(&b, (uint32_t)req->env_mode);
    w_u32(&b, (uint32_t)req->nproc);
    w_u32(&b, (uint32_t)req->as_mb);
    w_u32(&b, (uint32_t)req->cpu_sec);
    w_u32(&b, (uint32_t)req->sandbox);
    w_u32(&b, (uint32_t)req->net_mode);
    w_str(&b, req->workspace);
    w_str(&b, req->mount_cwd ? req->cwd_path : NULL);
    w_u32(&b, (uint32_t)req->workspace_ro);
    w_u32(&b, (uint32_t)req->mount_cwd);
    w_str_array(&b, req->read_paths, req->read_count);
    w_str_array(&b, req->write_paths, req->write_count);
    w_str(&b, req->agent_dir);
    w_str_array(&b, req->host_rules, req->host_count);
    w_str(&b, req->command);
    w_u32(&b, (uint32_t)req->timeout);
    w_u32(&b, (uint32_t)req->secret_count);
    for (size_t i = 0; i < req->secret_count; i++) {
        w_str(&b, req->secrets[i].name);
        w_str(&b, req->secrets[i].value);
    }

    size_t total = w_finalize(&b);
    if (total == 0) { free(blob); return NULL; }
    *out_len = total;
    return (char *)blob;
}

void run_tool_req_init(RunToolReq *req, int tier, const char *tool_name,
                       const char *arguments, const SandboxProfile *sb,
                       const char *workspace, const char *cwd_path) {
    memset(req, 0, sizeof(*req));
    req->tier = tier;
    req->tool_name = tool_name;
    req->arguments = arguments;
    req->env_mode = sb->env_mode;
    req->nproc   = sb->rlimits.nproc;
    req->as_mb   = sb->rlimits.as_mb;
    req->cpu_sec = sb->rlimits.cpu_sec;
    req->sandbox  = sb->sandbox;
    req->net_mode = sb->net_mode;
    req->workspace = workspace;
    req->cwd_path  = cwd_path;
    req->workspace_ro = sb->workspace_ro;
    req->mount_cwd    = sb->mount_cwd;
    req->read_paths = (const char **)sb->read_paths;
    req->read_count = sb->read_path_count;
    req->write_paths = (const char **)sb->write_paths;
    req->write_count = sb->write_path_count;
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

/* Decode the wire body into RunToolParsed (run_tool.h) — heap-owned strings,
 * mirrors RunToolReq's wire order. */
static int parse_request(Rbuf *r, RunToolParsed *q) {
    memset(q, 0, sizeof(*q));
    q->tier = r_u8(r);
    q->tool_name = r_str(r);
    q->arguments = r_str(r);
    q->env_mode  = (int)r_u32(r);
    q->nproc     = (int)r_u32(r);
    q->as_mb     = (int)r_u32(r);
    q->cpu_sec   = (int)r_u32(r);
    q->sandbox   = (int)r_u32(r);
    q->net_mode  = (int)r_u32(r);
    q->workspace = r_str(r);
    q->cwd_path  = r_str(r);
    q->workspace_ro = (int)r_u32(r);
    q->mount_cwd    = (int)r_u32(r);
    q->read_paths  = r_str_array(r, &q->read_count);
    q->write_paths = r_str_array(r, &q->write_count);
    q->agent_dir = r_str(r);
    q->host_rules = r_str_array(r, &q->host_count);
    q->command = r_str(r);
    q->timeout = (int)r_u32(r);
    uint32_t sc = r_u32(r);
    if (!r->err && sc > 0 && sc <= RUNTOOL_REQUEST_MAX / 4) {
        q->secrets = calloc(sc, sizeof(*q->secrets));
        if (q->secrets) {
            for (uint32_t i = 0; i < sc; i++) {
                q->secrets[i].name  = r_str(r);
                q->secrets[i].value = r_str(r);
            }
            q->secret_count = sc;
        }
    }
    return r->err ? -1 : 0;
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

/* ── fd-3 response framing ──────────────────────────────────────────────
 * [4-byte meta_len (network order)][hosts JSON][result bytes to EOF].
 * meta_len=0 (NULL hosts) for non-network tiers and every error path — an
 * unframed write would be misparsed by the parent's drain loop. */
static int write_framed(int fd, const char *hosts_json, const char *result) {
    size_t mlen = hosts_json ? strlen(hosts_json) : 0;
    unsigned char hdr[4] = { (unsigned char)(mlen >> 24), (unsigned char)(mlen >> 16),
                             (unsigned char)(mlen >> 8),  (unsigned char)mlen };
    if (write_all(fd, hdr, 4) != 0) return -1;
    if (mlen && write_all(fd, hosts_json, mlen) != 0) return -1;
    return write_all(fd, result, strlen(result));
}

static void die(const char *msg) {
    write_framed(FD_REQUEST, NULL, msg);
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

/* Fill a SandboxConfig from the parsed request + descriptor. proxy_sock may be
 * NULL. extra_mounts must be freed by the caller after sandbox_child_setup. */
static void build_sandbox_cfg(const RunToolParsed *q, int skip_pid_ns,
                              const char *proxy_sock, SandboxConfig *cfg,
                              SandboxMountReq **extra, size_t *n_extra) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->workspace    = q->workspace;
    cfg->cwd_path     = q->cwd_path;
    cfg->db_path      = NULL;  /* no DB in this process */
    cfg->proxy_sock   = proxy_sock;
    cfg->sandbox      = q->sandbox;
    cfg->workspace_ro = q->workspace_ro;
    cfg->mount_cwd    = q->mount_cwd;
    cfg->env_mode     = q->env_mode;
    cfg->net_mode     = q->net_mode;
    cfg->skip_pid_ns  = skip_pid_ns;
    cfg->rlimits.nproc   = q->nproc;
    cfg->rlimits.as_mb   = q->as_mb;
    cfg->rlimits.cpu_sec = q->cpu_sec;
    *n_extra = 0;
    *extra = build_extra_mounts(q->read_paths, q->read_count,
                                q->write_paths, q->write_count, n_extra);
    cfg->extra_mounts = *extra;
    cfg->extra_mount_count = *n_extra;
}

/* ── Tier leaf run_fns ──────────────────────────────────────────────────
 * Each leaf is owned by its tool's own file (tool_file.c / tool_web_fetch.c /
 * tool_js.c) — the broker holds only these pointers and zero per-tool
 * knowledge. In-process tiers compute a result string. Shell is inner_exec
 * (run_fn == NULL) — the sandbox child calls tool_shell_tier_exec directly. */
typedef char *(*RunFn)(const RunToolParsed *q);

/* ── Tier descriptor (single source of truth) ────────────────────────────
 * Decode the tier byte ONCE into this; pass it down. "What makes web differ
 * from shell" lives here, not scattered across call sites. */
typedef struct {
    int   skip_pid_ns;   /* 1 = no CLONE_NEWPID (file/web/js); 0 = PID ns (shell) */
    int   needs_proxy;   /* 1 = bind/serve proxy + inner fork (network tiers) */
    int   inner_exec;    /* 1 = run_fn execs a foreign program (shell) */
    RunFn run_fn;        /* leaf result producer; NULL iff inner_exec */
} TierDescriptor;

static const TierDescriptor *tier_descriptor(int tier) {
    static const TierDescriptor table[] = {
        [RUNTOOL_TIER_FILE]  = { .skip_pid_ns = 1, .needs_proxy = 0, .inner_exec = 0, .run_fn = tool_file_tier_run },
        [RUNTOOL_TIER_SHELL] = { .skip_pid_ns = 0, .needs_proxy = 1, .inner_exec = 1, .run_fn = NULL },
        [RUNTOOL_TIER_WEB]   = { .skip_pid_ns = 1, .needs_proxy = 1, .inner_exec = 0, .run_fn = tool_web_tier_run },
        [RUNTOOL_TIER_JS]    = { .skip_pid_ns = 1, .needs_proxy = 1, .inner_exec = 0, .run_fn = tool_js_tier_run },
    };
    if (tier < 0 || tier >= (int)(sizeof(table) / sizeof(table[0])))
        return NULL;
    if (!table[tier].run_fn && !table[tier].inner_exec)
        return NULL;  /* unpopulated slot */
    return &table[tier];
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

/* ── Network tiers: broker + sandbox child ───────────────────────────────
 * proxy_bind (single-threaded, before fork) → fork → child (sandbox + run_fn
 * or inner exec) → proxy_serve → drain → waitpid/kill → proxy_stop. The
 * bind-before-fork-before-serve ordering is load-bearing. The broker IS this
 * --run-tool process. A proxy is interposed IFF the tier needs gated egress. */

#define NET_MAX_OUTPUT (256 * 1024)

/* The pid==0 body of serve_network_child's fork: wire stdout/stderr to the
 * pipe, apply the sandbox, then exec (shell) or run the tier leaf in-process. */
__attribute__((noreturn))
static void sandbox_child_main(const TierDescriptor *desc, RunToolParsed *q,
                               const char *psock, int pipefd[2]) {
    setpgid(0, 0);
    /* Die if the broker dies (daemon SIGKILLs on backstop deadline):
     * the broker can no longer kill our group, so tie our lifetime to it.
     * sandbox_apply_namespace re-arms this on the inner PID-ns init. */
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);

    /* Host mode (sandbox=0): no namespace, no proxy (proxy_active is
     * already gated on q->sandbox) — run_fn / exec runs on the bare host. */
    if (q->sandbox) {
        SandboxConfig cfg;
        SandboxMountReq *extra = NULL;
        size_t n_extra = 0;
        build_sandbox_cfg(q, desc->skip_pid_ns, psock, &cfg, &extra, &n_extra);

        if (sandbox_child_setup(&cfg) != 0) {
            fprintf(stderr, "error: namespace sandbox failed\n");
            _exit(126);
        }
    }

    if (desc->inner_exec)
        tool_shell_tier_exec(q);  /* execs /bin/sh, or _exit(127); never returns */

    /* In-process tier (web/js): compute result, write to stdout (the pipe). */
    char *result = desc->run_fn(q);
    if (result) { write_all(STDOUT_FILENO, result, strlen(result)); free(result); }
    _exit(0);
}

/* Drain child output into `output` until the deadline, the pipe closes, or the
 * cap is hit; then reap. Closes fd. Returns 1 iff the deadline expired (child
 * group SIGKILLed). *status is the waitpid status when not timed out. */
static int drain_child(pid_t pid, int fd, int timeout, char *output,
                       size_t *out_len, int *status) {
    size_t len = 0;
    int timed_out = 0;
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
        FD_SET(fd, &rfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int sel = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (sel > 0) {
            ssize_t n = read(fd, output + len, NET_MAX_OUTPUT - len);
            if (n <= 0) break;
            len += (size_t)n;
            if (len >= NET_MAX_OUTPUT) break;
        } else if (sel < 0 && errno != EINTR) break;
        int wr = waitpid(pid, status, WNOHANG);
        if (wr > 0) {
            while (len < NET_MAX_OUTPUT) {
                ssize_t n = read(fd, output + len, NET_MAX_OUTPUT - len);
                if (n <= 0) break;
                len += (size_t)n;
            }
            close(fd);
            *out_len = len;
            return 0;
        }
    }
    close(fd);

    if (timed_out) {
        kill(-pid, SIGKILL);
        waitpid(pid, NULL, 0);
    } else {
        waitpid(pid, status, 0);
    }
    *out_len = len;
    return timed_out;
}

/* Wrap the drained output for the fd-3 frame. Takes ownership of `output`
 * (NUL-terminated at out_len); returns the malloc'd result. */
static char *format_tier_result(const TierDescriptor *desc, char *output,
                                size_t out_len, int timed_out, int status,
                                int timeout) {
    char *result;
    if (desc->inner_exec) {
        /* Shell: prefix the captured stdout/stderr with the exit status. */
        size_t needed = out_len + 128;
        result = malloc(needed);
        if (timed_out)
            snprintf(result, needed, "[timeout after %ds]\n%s", timeout, output);
        else
            snprintf(result, needed, "[exit %d]\n%s",
                     WIFEXITED(status) ? WEXITSTATUS(status) : -1, output);
        free(output);
    } else {
        /* In-process tier: the child already produced the final result. */
        if (timed_out) {
            size_t needed = out_len + 64;
            result = malloc(needed);
            snprintf(result, needed, "error: tool timed out (%ds)\n%s", timeout, output);
            free(output);
        } else {
            result = output;  /* hand off ownership */
        }
    }
    return result;
}

static void serve_network_child(const TierDescriptor *desc, RunToolParsed *q,
                                unsigned char *body, size_t body_len) {
    if (q->timeout <= 0) q->timeout = 30;

    /* Pipe for child stdout+stderr (or the in-process result for web/js). */
    int pipefd[2];
    if (pipe(pipefd) != 0) die("error: net: pipe failed");

    /* Proxy: bind while single-threaded (before the fork below).
     * Skipped if no agent_dir, no sandbox, or net_mode==1 (no network). */
    ProxyContext proxy;
    int proxy_active = 0;
    if (q->agent_dir && !q->net_mode && q->sandbox) {
        if (proxy_bind(&proxy, q->agent_dir, q->host_rules, q->host_count) == 0)
            proxy_active = 1;
    }
    const char *psock = proxy_active ? proxy_sock_path(&proxy) : NULL;

    pid_t pid = fork();
    if (pid < 0) {
        if (proxy_active) proxy_stop(&proxy);
        die("error: net: fork failed");
    }

    if (pid == 0)
        sandbox_child_main(desc, q, psock, pipefd);  /* never returns */

    /* ── BROKER (parent of sandbox child) ── */
    setpgid(pid, pid);
    close(pipefd[1]);

    /* The broker outlives the fork for the whole command. It needs no secret
     * material post-fork (the child has COW copies), so scrub plaintext now to
     * shrink the window a core dump / same-uid /proc read could expose. */
    if (q->command) explicit_bzero(q->command, strlen(q->command));
    for (size_t i = 0; i < q->secret_count; i++)
        if (q->secrets[i].value) explicit_bzero(q->secrets[i].value, strlen(q->secrets[i].value));
    if (body && body_len) explicit_bzero(body, body_len);

    /* Start proxy accept thread AFTER the single-threaded fork */
    if (proxy_active) {
        if (proxy_serve(&proxy) != 0) { proxy_stop(&proxy); proxy_active = 0; }
    }

    /* Drain child output with timeout */
    char *output = malloc(NET_MAX_OUTPUT + 1);
    if (!output) {
        kill(-pid, SIGKILL); waitpid(pid, NULL, 0);
        if (proxy_active) proxy_stop(&proxy);
        die("error: net: OOM");
    }
    size_t out_len = 0;
    int status = 0;
    int timed_out = drain_child(pid, pipefd[0], q->timeout, output, &out_len, &status);

    /* Capture the contacted-hosts tag before proxy_stop frees the list. */
    char *hosts_json = proxy_active ? proxy_hosts_json(&proxy) : NULL;
    if (proxy_active) proxy_stop(&proxy);
    output[out_len] = '\0';

    char *result = format_tier_result(desc, output, out_len, timed_out, status, q->timeout);

    write_framed(FD_REQUEST, hosts_json, result);
    free(hosts_json);
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
     * _SC_OPEN_MAX is the fallback. */
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

    /* Syslog for this process family (proxy denials, sandbox failures) —
     * after the blanket close (openlog opens a socket fd). The parent passes
     * only CCLAW_LOG_LEVEL in the env; without setting the level the child
     * would sit on log.c's compiled-in default, not the configured one. */
    cclaw_log_init(0);
    cclaw_log_set_level(log_level_parse(getenv("CCLAW_LOG_LEVEL")));

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
    RunToolParsed q;
    if (parse_request(&r, &q) != 0)
        die("error: malformed request");
    if (!q.tool_name) die("error: missing tool_name");
    if (!q.arguments) q.arguments = strdup("{}");

    const TierDescriptor *desc = tier_descriptor(q.tier);
    if (!desc) die("error: unsupported tier");

    if (desc->needs_proxy) {
        serve_network_child(desc, &q, body, body_len);  /* _exit, never returns */
    }

    /* Non-network tier (file): sandbox THIS process, run the handler in-process. */
    if (!q.workspace) die("error: missing workspace");

    /* net_mode forced to "no network"; host mode (sandbox=0) runs the handler
     * on the bare host — the app-level workspace clamp still applies. */
    q.net_mode = 1;
    if (q.sandbox) {
        SandboxConfig cfg;
        SandboxMountReq *extra = NULL;
        size_t n_extra = 0;
        build_sandbox_cfg(&q, desc->skip_pid_ns, NULL, &cfg, &extra, &n_extra);

        if (sandbox_child_setup(&cfg) != 0)
            die("error: namespace sandbox setup failed");
    }

    char *result = desc->run_fn(&q);
    if (!result) result = strdup("");
    write_framed(FD_REQUEST, NULL, result);  /* file tier: zero-length meta */
    free(result);
    _exit(0);
}
