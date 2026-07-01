#define _GNU_SOURCE
#include "run_tool.h"
#include "tool_file.h"
#include "tool_web_fetch.h"
#include "tool_js.h"
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

/* ── Child-side parsed request (owned strings) ──────────────────────────
 * Mirrors RunToolReq's wire order. Strings/arrays are heap-owned by the
 * --run-tool process for its (short) lifetime; never freed before _exit. */
typedef struct {
    int   tier;
    char *tool_name;
    char *arguments;
    int   env_mode, nproc, as_mb, cpu_sec;
    int   sandbox, net_mode;
    char *workspace, *cwd_path;
    int   workspace_ro, mount_cwd;
    char **read_paths;  size_t read_count;
    char **write_paths; size_t write_count;
    char *agent_dir;
    char **host_rules;  size_t host_count;
    char *command;
    int   timeout;
    struct { char *name; char *value; } *secrets;
    size_t secret_count;
} ParsedReq;

static int parse_request(Rbuf *r, ParsedReq *q) {
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

/* Fill a SandboxConfig from the parsed request + descriptor. proxy_sock may be
 * NULL. extra_mounts must be freed by the caller after sandbox_child_setup. */
static void build_sandbox_cfg(const ParsedReq *q, int skip_pid_ns,
                              const char *proxy_sock, SandboxConfig *cfg,
                              SandboxMountReq **extra, size_t *n_extra) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->workspace    = q->workspace;
    cfg->cwd_path     = q->cwd_path;
    cfg->db_path      = NULL;  /* no DB in this process */
    cfg->env_file     = NULL;
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

/* ── Tier leaf run_fns ──────────────────────────────────────────────────
 * In-process tiers (file, and later web/js) compute a result string. Shell is
 * inner_exec (run_fn == NULL) — serve_network_child execs /bin/sh directly. */
typedef char *(*RunFn)(const ParsedReq *q);

static char *run_file_tier(const ParsedReq *q) {
    /* Sandbox is already applied on this process; sandbox=0 so the file
     * handler runs directly (the kernel mount view IS the boundary). */
    FileReadCtx fctx = {0};
    fctx.workspace    = q->workspace;
    fctx.cwd_path     = q->cwd_path;
    fctx.sb.sandbox      = 0;
    fctx.sb.workspace_ro = q->workspace_ro;
    fctx.sb.mount_cwd    = q->mount_cwd;
    fctx.sb.read_paths   = q->read_paths;
    fctx.sb.read_path_count = q->read_count;
    fctx.sb.write_paths  = q->write_paths;
    fctx.sb.write_path_count = q->write_count;
    char *r = dispatch_file(q->tool_name, q->arguments, &fctx);
    return r ? r : strdup("");
}

static char *run_web_tier(const ParsedReq *q) {
    /* Runs in the inner fork, inside the netns + proxy. Our libcurl honors the
     * HTTP_PROXY set by sandbox_child_setup → net_shim → broker → decide() on
     * every hop. user_data unused (egress is the proxy's job, not a preflight). */
    char *r = tool_web_fetch_handler(q->arguments, NULL);
    return r ? r : strdup("error: web_fetch returned null");
}

static char *run_js_tier(const ParsedReq *q) {
    /* qjs runs in-process in the inner fork (web's twin): netns + proxy + mounts
     * are already applied. tool_js_eval_handler parses {code|filename,args} and
     * evals via qjs_eval_run; http_request's curl honors HTTP_PROXY → decide().
     * fs.* paths are real bind-mounts from the blob's read/write paths. */
    char *r = tool_js_eval_handler(q->arguments, NULL);
    return r ? r : strdup("error: js_eval returned null");
}

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
        [RUNTOOL_TIER_FILE]  = { .skip_pid_ns = 1, .needs_proxy = 0, .inner_exec = 0, .run_fn = run_file_tier },
        [RUNTOOL_TIER_SHELL] = { .skip_pid_ns = 0, .needs_proxy = 1, .inner_exec = 1, .run_fn = NULL },
        [RUNTOOL_TIER_WEB]   = { .skip_pid_ns = 1, .needs_proxy = 1, .inner_exec = 0, .run_fn = run_web_tier },
        [RUNTOOL_TIER_JS]    = { .skip_pid_ns = 1, .needs_proxy = 1, .inner_exec = 0, .run_fn = run_js_tier },
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

static void serve_network_child(const TierDescriptor *desc, ParsedReq *q,
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

    if (pid == 0) {
        /* SANDBOX CHILD */
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

        if (desc->inner_exec) {
            /* Inject secrets into env — only after env scrub (ordering matters).
             * These are the minimal set pre-filtered by the parent. */
            for (size_t i = 0; i < q->secret_count; i++) {
                if (q->secrets[i].name && q->secrets[i].value) {
                    char envname[256];
                    snprintf(envname, sizeof(envname), "CCLAW_SECRET_%s", q->secrets[i].name);
                    setenv(envname, q->secrets[i].value, 1);
                }
            }
            for (size_t i = 0; i < q->secret_count; i++)
                if (q->secrets[i].value) explicit_bzero(q->secrets[i].value, strlen(q->secrets[i].value));
            /* The one inner exec — shell is the only tier with a foreign
             * program. command carries interpolated secrets; exec replaces the
             * address space so they don't persist after execl. */
            execl("/bin/sh", "sh", "-c", q->command, (char *)NULL);
            _exit(127);
        }

        /* In-process tier (web/js): compute result, write to stdout (the pipe). */
        char *result = desc->run_fn(q);
        if (result) { write_all(STDOUT_FILENO, result, strlen(result)); free(result); }
        _exit(0);
    }

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
    int timed_out = 0, status = 0;
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += q->timeout;

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
            ssize_t n = read(pipefd[0], output + out_len, NET_MAX_OUTPUT - out_len);
            if (n <= 0) break;
            out_len += (size_t)n;
            if (out_len >= NET_MAX_OUTPUT) break;
        } else if (sel < 0 && errno != EINTR) break;
        int wr = waitpid(pid, &status, WNOHANG);
        if (wr > 0) {
            while (out_len < NET_MAX_OUTPUT) {
                ssize_t n = read(pipefd[0], output + out_len, NET_MAX_OUTPUT - out_len);
                if (n <= 0) break;
                out_len += (size_t)n;
            }
            close(pipefd[0]);
            goto net_format;
        }
    }
    close(pipefd[0]);

    if (timed_out) {
        kill(-pid, SIGKILL);
        waitpid(pid, NULL, 0);
    } else {
        waitpid(pid, &status, 0);
    }

net_format:
    if (proxy_active) proxy_stop(&proxy);
    output[out_len] = '\0';

    char *result;
    if (desc->inner_exec) {
        /* Shell: prefix the captured stdout/stderr with the exit status. */
        size_t needed = out_len + 128;
        result = malloc(needed);
        if (timed_out)
            snprintf(result, needed, "[timeout after %ds]\n%s", q->timeout, output);
        else
            snprintf(result, needed, "[exit %d]\n%s",
                     WIFEXITED(status) ? WEXITSTATUS(status) : -1, output);
        free(output);
    } else {
        /* In-process tier: the child already produced the final result. */
        if (timed_out) {
            size_t needed = out_len + 64;
            result = malloc(needed);
            snprintf(result, needed, "error: tool timed out (%ds)\n%s", q->timeout, output);
            free(output);
        } else {
            result = output;  /* hand off ownership */
        }
    }

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
    ParsedReq q;
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
    write_all(FD_REQUEST, result, strlen(result));
    free(result);
    _exit(0);
}
