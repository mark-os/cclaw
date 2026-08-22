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
    w_u32(&b, (uint32_t)req->env_mode);
    w_u32(&b, (uint32_t)req->nproc);
    w_u32(&b, (uint32_t)req->as_mb);
    w_u32(&b, (uint32_t)req->cpu_sec);
    w_u32(&b, (uint32_t)req->sandbox);
    w_u32(&b, (uint32_t)req->net_mode);
    w_str(&b, req->workspace);
    w_str(&b, req->mount_cwd ? req->cwd_path : NULL);
    w_str(&b, req->db_path);
    w_u32(&b, (uint32_t)req->mount_cwd);
    w_str(&b, req->tmp_dir);
    w_str_array(&b, req->read_paths, req->read_count);
    w_str_array(&b, req->write_paths, req->write_count);
    w_str(&b, req->agent_dir);
    w_str_array(&b, req->host_rules, req->host_count);
    w_str_array(&b, req->deny_rules, req->deny_count);
    w_str(&b, req->command);
    w_str(&b, req->shell_path);
    w_u32(&b, (uint32_t)req->timeout);
    w_u32(&b, (uint32_t)req->secret_count);
    for (size_t i = 0; i < req->secret_count; i++) {
        w_str(&b, req->secrets[i].name);
        w_str(&b, req->secrets[i].value);
        w_str(&b, req->secrets[i].hosts);
    }
    w_u32(&b, (uint32_t)req->param_count);
    for (size_t i = 0; i < req->param_count; i++) {
        const ToolWireArg *p = &req->params[i];
        w_str(&b, p->key);
        w_u8(&b, (unsigned char)p->kind);
        if (p->kind == TOOL_ARG_LIST)
            w_str_array(&b, (const char **)p->list, p->list_n);
        else
            w_str(&b, p->value);
    }
    w_str(&b, req->egress_note);
    w_str(&b, req->spill_path);

    size_t total = w_finalize(&b);
    if (total == 0) { free(blob); return NULL; }
    *out_len = total;
    return (char *)blob;
}

void run_tool_req_init(RunToolReq *req, int tier, const char *tool_name,
                       const SandboxProfile *sb,
                       const char *workspace, const char *cwd_path,
                       const char *db_path) {
    memset(req, 0, sizeof(*req));
    req->tier = tier;
    req->tool_name = tool_name;
    req->db_path = db_path;
    req->env_mode = sb->env_mode;
    req->nproc   = sb->rlimits.nproc;
    req->as_mb   = sb->rlimits.as_mb;
    req->cpu_sec = sb->rlimits.cpu_sec;
    req->sandbox  = sb->sandbox;
    req->net_mode = sb->net_mode;
    req->workspace = workspace;
    req->cwd_path  = cwd_path;
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
    q->env_mode  = (int)r_u32(r);
    q->nproc     = (int)r_u32(r);
    q->as_mb     = (int)r_u32(r);
    q->cpu_sec   = (int)r_u32(r);
    q->sandbox   = (int)r_u32(r);
    q->net_mode  = (int)r_u32(r);
    q->workspace = r_str(r);
    q->cwd_path  = r_str(r);
    q->db_path   = r_str(r);
    q->mount_cwd    = (int)r_u32(r);
    q->tmp_dir      = r_str(r);
    q->read_paths  = r_str_array(r, &q->read_count);
    q->write_paths = r_str_array(r, &q->write_count);
    q->agent_dir = r_str(r);
    q->host_rules = r_str_array(r, &q->host_count);
    q->deny_rules = r_str_array(r, &q->deny_count);
    q->command = r_str(r);
    q->shell_path = r_str(r);
    q->timeout = (int)r_u32(r);
    /* An out-of-bounds count or a failed calloc must latch r->err, never
     * skip the section — skipping would decode every later field from the
     * wrong offset (silent misparse instead of a hard failure). */
    uint32_t sc = r_u32(r);
    if (!r->err && sc > 0) {
        if (sc > RUNTOOL_REQUEST_MAX / 4 ||
            !(q->secrets = calloc(sc, sizeof(*q->secrets)))) {
            r->err = 1;
        } else {
            for (uint32_t i = 0; i < sc; i++) {
                q->secrets[i].name  = r_str(r);
                q->secrets[i].value = r_str(r);
                q->secrets[i].hosts = r_str(r);
            }
            q->secret_count = sc;
        }
    }
    uint32_t pc = r_u32(r);
    if (!r->err && pc > 0) {
        if (pc > RUNTOOL_REQUEST_MAX / 8 ||
            !(q->params = calloc(pc, sizeof(*q->params)))) {
            r->err = 1;
        } else {
            for (uint32_t i = 0; i < pc; i++) {
                q->params[i].key = r_str(r);
                q->params[i].kind = (int)r_u8(r);
                if (q->params[i].kind == TOOL_ARG_LIST) {
                    q->params[i].list = r_str_array(r, &q->params[i].list_n);
                } else {
                    /* A present param always has a value: the wire's
                     * length-0 = NULL convention must not turn an empty
                     * string (e.g. file_write content:"") into absent. */
                    q->params[i].value = r_str(r);
                    if (!q->params[i].value && !r->err)
                        q->params[i].value = strdup("");
                }
            }
            q->param_count = pc;
        }
    }
    q->egress_note = r_str(r);
    q->spill_path = r_str(r);
    return r->err ? -1 : 0;
}

/* ── Child-side param lookup ── */

static const struct RunToolParam *param_find(const RunToolParsed *q,
                                             const char *key, int kind) {
    for (size_t i = 0; i < q->param_count; i++)
        if (q->params[i].kind == kind && q->params[i].key &&
            strcmp(q->params[i].key, key) == 0)
            return &q->params[i];
    return NULL;
}

const char *run_tool_param_str(const RunToolParsed *q, const char *key) {
    const struct RunToolParam *p = param_find(q, key, TOOL_ARG_TEXT);
    return p ? p->value : NULL;
}

int run_tool_param_int(const RunToolParsed *q, const char *key, int def) {
    const char *v = run_tool_param_str(q, key);
    return v ? atoi(v) : def;
}

int run_tool_param_bool(const RunToolParsed *q, const char *key, int def) {
    const char *v = run_tool_param_str(q, key);
    return v ? (v[0] == '1') : def;
}

const char *run_tool_param_json(const RunToolParsed *q, const char *key) {
    const struct RunToolParam *p = param_find(q, key, TOOL_ARG_JSON);
    return p ? p->value : NULL;
}

char **run_tool_param_list(const RunToolParsed *q, const char *key, size_t *n) {
    const struct RunToolParam *p = param_find(q, key, TOOL_ARG_LIST);
    *n = p ? p->list_n : 0;
    return p ? p->list : NULL;
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
 * [1-byte status][4-byte meta_len (network order)][hosts JSON][result to EOF].
 *
 * status is the tool's EXPLICIT outcome (RUNTOOL_STATUS_OK / _ERROR) — the
 * child's own answer to "did this call fail", travelling atomically with the
 * body it describes so the parent never has to read the prose to find out.
 * It is not the process's exit code: exit status keeps its lifecycle meaning
 * (sandbox refusal, crash, signal) and says nothing about tool success.
 *
 * meta_len=0 (NULL hosts) for non-network tiers and every error path — an
 * unframed write would be misparsed by the parent's drain loop. */
static int write_framed(int fd, int status, const char *hosts_json,
                        const char *result) {
    size_t mlen = hosts_json ? strlen(hosts_json) : 0;
    unsigned char hdr[5] = { (unsigned char)(status ? RUNTOOL_STATUS_ERROR
                                                    : RUNTOOL_STATUS_OK),
                             (unsigned char)(mlen >> 24), (unsigned char)(mlen >> 16),
                             (unsigned char)(mlen >> 8),  (unsigned char)mlen };
    if (write_all(fd, hdr, 5) != 0) return -1;
    if (mlen && write_all(fd, hosts_json, mlen) != 0) return -1;
    return write_all(fd, result, strlen(result));
}

static void die(const char *msg) {
    write_framed(FD_REQUEST, RUNTOOL_STATUS_ERROR, NULL, msg);
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
    /* Path only — the child never opens the DB. Used solely to locate the key
     * and ciphertext for masking. */
    cfg->db_path      = q->db_path;
    cfg->proxy_sock   = proxy_sock;
    cfg->sandbox      = q->sandbox;
    cfg->mount_cwd    = q->mount_cwd;
    cfg->env_mode     = q->env_mode;
    cfg->net_mode     = q->net_mode;
    cfg->skip_pid_ns  = skip_pid_ns;
    cfg->tmp_dir         = q->tmp_dir;
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
typedef char *(*RunFn)(const RunToolParsed *q, int *is_error);

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
            fprintf(stderr, "error: the namespace sandbox could not be "
                    "established on this host, so the tool refused to run "
                    "(fail-closed). This is an environment problem, not "
                    "something you can fix — report it to the operator; "
                    "retrying will not help\n");
            _exit(126);
        }
    }

    if (desc->inner_exec)
        tool_shell_tier_exec(q);  /* execs /bin/sh, or _exit(127); never returns */

    /* In-process tier (web/js): compute result, write to stdout (the pipe).
     * The tool's status rides back to the BROKER (our immediate parent, still
     * inside the sandbox boundary) as this inner child's exit code — the only
     * channel that stays out of the byte stream stdout and stderr share. The
     * broker translates it into the response frame's status byte; the exit
     * code the daemon eventually reaps is the broker's, not this one's, and
     * keeps its lifecycle-only meaning. */
    int is_error = 0;
    char *result = desc->run_fn(q, &is_error);
    if (result) { write_all(STDOUT_FILENO, result, strlen(result)); free(result); }
    _exit(is_error ? 1 : 0);
}


/* Spill an oversized result from the child that produced it.
 *
 * The wire cap used to truncate before the parent ever saw the output, so the
 * "full output" file the parent then wrote held the truncated copy — the
 * pointer we hand the model was a pointer to the same loss. The child already
 * has the whole thing in memory, so it writes the file here and sends back
 * only the head.
 *
 * The cut mirrors context.c exactly (bytes AND lines): a result still over
 * either limit would be spilled a second time by the parent, overwriting the
 * full file with the head. Returns a malloc'd replacement, or NULL to keep the
 * caller's result as-is. */
static char *spill_large_result(const char *spill_path, const char *result,
                                int already_written) {
    if (!spill_path || !spill_path[0] || !result) return NULL;
    size_t len = strlen(result);

    size_t cut = len;
    int lines = 0, total_lines = 0;
    for (size_t i = 0; i < len; i++) {
        if (result[i] == '\n') {
            total_lines++;
            if (cut == len) {
                lines++;
                if (lines >= RUNTOOL_RESULT_MAX_LINES) cut = i + 1;
            }
        }
        if (i + 1 >= RUNTOOL_RESULT_MAX && cut == len) cut = RUNTOOL_RESULT_MAX;
    }
    if (cut >= len) return NULL;   /* fits — nothing to do */

    /* The drain may already have streamed the full output here, in which case
     * rewriting from `result` would replace it with the truncated copy.
     * O_NOFOLLOW: a symlink planted at the path must fail the open, not
     * redirect the write. Best effort — a failed spill still truncates. */
    int wrote = already_written;
    int fd = already_written ? -1
           : open(spill_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        size_t off = 0;
        while (off < len) {
            ssize_t w = write(fd, result + off, len - off);
            if (w <= 0) break;
            off += (size_t)w;
        }
        wrote = (off == len);
        close(fd);
    }

    char suffix[PATH_MAX + 128];
    if (wrote)
        snprintf(suffix, sizeof(suffix),
                 "\n[truncated — showing first %d lines of %d. Full output: %s]",
                 lines < RUNTOOL_RESULT_MAX_LINES ? lines : RUNTOOL_RESULT_MAX_LINES,
                 total_lines > 0 ? total_lines : 1, spill_path);
    else
        snprintf(suffix, sizeof(suffix),
                 "\n[truncated — showing first %d lines of %d; full output could not be saved]",
                 lines < RUNTOOL_RESULT_MAX_LINES ? lines : RUNTOOL_RESULT_MAX_LINES,
                 total_lines > 0 ? total_lines : 1);

    size_t slen = strlen(suffix);
    char *out = malloc(cut + slen + 1);
    if (!out) return NULL;
    memcpy(out, result, cut);
    memcpy(out + cut, suffix, slen + 1);
    return out;
}


/* Streams overflow to the spill file so a long-running command's output is not
 * simply dropped at the in-memory cap. The head stays in `output` for the
 * result; once the total passes RUNTOOL_RESULT_MAX the file is opened, the
 * retained head written first, and everything after appended. Opening lazily
 * keeps the common small-output case free of any file I/O. */
typedef struct {
    const char *path;
    int fd;
    int failed;
} SpillSink;

static void spill_sink_feed(SpillSink *s, const char *head, size_t head_len,
                            const char *buf, size_t n, size_t total_before) {
    if (!s->path || !s->path[0] || s->failed) return;
    if (s->fd < 0) {
        if (total_before + n <= RUNTOOL_RESULT_MAX) return;   /* still fits inline */
        s->fd = open(s->path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
        if (s->fd < 0) { s->failed = 1; return; }
        /* everything seen so far lives in `head` */
        size_t off = 0;
        while (off < head_len) {
            ssize_t w = write(s->fd, head + off, head_len - off);
            if (w <= 0) { s->failed = 1; return; }
            off += (size_t)w;
        }
    }
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(s->fd, buf + off, n - off);
        if (w <= 0) { s->failed = 1; return; }
        off += (size_t)w;
    }
}

/* Drain child output until the deadline or the pipe closes, then reap. The
 * first NET_MAX_OUTPUT bytes are retained in `output` for the result; anything
 * beyond is streamed to `spill_path` rather than dropped, so the file the
 * result points at really is the full output. Closes fd. Returns 1 iff the
 * deadline expired (child group SIGKILLed). *total_len is everything seen. */
static int drain_child(pid_t pid, int fd, int timeout, char *output,
                       size_t *out_len, size_t *total_len,
                       const char *spill_path, int *spilled, int *status) {
    size_t len = 0, total = 0;
    SpillSink sink = { .path = spill_path, .fd = -1, .failed = 0 };
    char chunk[8192];
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
            ssize_t n = read(fd, chunk, sizeof(chunk));
            if (n <= 0) break;
            spill_sink_feed(&sink, output, len, chunk, (size_t)n, total);
            total += (size_t)n;
            size_t room = NET_MAX_OUTPUT - len;
            size_t keep = ((size_t)n < room) ? (size_t)n : room;
            memcpy(output + len, chunk, keep);
            len += keep;
        } else if (sel < 0 && errno != EINTR) break;
        int wr = waitpid(pid, status, WNOHANG);
        if (wr > 0) {
            for (;;) {
                ssize_t n = read(fd, chunk, sizeof(chunk));
                if (n <= 0) break;
                spill_sink_feed(&sink, output, len, chunk, (size_t)n, total);
                total += (size_t)n;
                size_t room = NET_MAX_OUTPUT - len;
                size_t keep = ((size_t)n < room) ? (size_t)n : room;
                memcpy(output + len, chunk, keep);
                len += keep;
            }
            close(fd);
            if (sink.fd >= 0) { close(sink.fd); *spilled = 1; }
            *out_len = len;
            *total_len = total;
            return 0;
        }
    }
    close(fd);
    if (sink.fd >= 0) { close(sink.fd); *spilled = 1; }
    *total_len = total;

    if (timed_out) {
        kill(-pid, SIGKILL);
        waitpid(pid, NULL, 0);
    } else {
        waitpid(pid, status, 0);
    }
    *out_len = len;
    return timed_out;
}

/* One cheap attribution attempt for a workload killed by a signal, made from
 * data already in hand at reap: the signal number, and the rlimits this call
 * actually ran under. SIGKILL with a limit configured is overwhelmingly the
 * kernel (or our own CPU cap) enforcing it, and "retry unchanged" is the wrong
 * next move — say so. Anything else gets the bare signal line. No dmesg, no
 * probing, no second run. Returns a static string, or NULL if not signalled. */
static const char *signal_attribution(const RunToolParsed *q, int status) {
    if (!WIFSIGNALED(status)) return NULL;
    int sig = WTERMSIG(status);
    int limited = q && (q->as_mb > 0 || q->cpu_sec > 0 || q->nproc > 0);
    if (sig == SIGKILL && limited)
        return "\n[killed by SIGKILL — likely resource limit "
               "(memory/CPU/processes); reduce usage, don't just retry]";
    if (sig == SIGKILL) return "\n[killed by SIGKILL]";
    if (sig == SIGXCPU)
        return "\n[killed by SIGXCPU — CPU limit reached; "
               "reduce usage, don't just retry]";
    return NULL;
}

/* Wrap the drained output for the fd-3 frame. Takes ownership of `output`
 * (NUL-terminated at out_len); returns the malloc'd result. *is_error carries
 * the tool's explicit outcome out to the response frame. */
static char *format_tier_result(const TierDescriptor *desc, char *output,
                                size_t out_len, int timed_out, int status,
                                int timeout, const RunToolParsed *q,
                                int *is_error) {
    const char *attrib = signal_attribution(q, status);
    char *result;
    if (desc->inner_exec) {
        /* Shell: prefix the captured stdout/stderr with the exit status. A
         * nonzero exit is the command's own answer, not a tool failure — only
         * a timeout is. */
        size_t needed = out_len + 256;
        result = malloc(needed);
        if (!result) return output;   /* OOM: hand back the raw capture */
        if (timed_out) {
            snprintf(result, needed,
                     "[timeout after %ds — raise with the timeout parameter]\n%s",
                     timeout, output);
            *is_error = 1;
        } else {
            snprintf(result, needed, "[exit %d]\n%s",
                     WIFEXITED(status) ? WEXITSTATUS(status) : -1, output);
        }
        free(output);
    } else {
        /* In-process tier: the child already produced the final result, and
         * signalled its own status through its exit code (sandbox_child_main).
         * A signalled death is a failure too — nobody asked for it. */
        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) *is_error = 1;
        if (WIFSIGNALED(status)) *is_error = 1;
        if (timed_out) {
            size_t needed = out_len + 128;
            result = malloc(needed);
            if (!result) return output;   /* OOM: hand back the raw capture */
            snprintf(result, needed,
                     "error: tool timed out (%ds; raise with the timeout parameter)\n%s",
                     timeout, output);
            free(output);
            *is_error = 1;
        } else {
            result = output;  /* hand off ownership */
        }
    }
    /* Signal attribution rides on the end of whatever we produced. */
    if (attrib && result) {
        size_t rlen = strlen(result), alen = strlen(attrib);
        char *joined = realloc(result, rlen + alen + 1);
        if (joined) {
            memcpy(joined + rlen, attrib, alen + 1);
            result = joined;
        }
    }
    return result;
}

static void serve_network_child(const TierDescriptor *desc, RunToolParsed *q,
                                unsigned char *body, size_t body_len) {
    if (q->timeout <= 0) q->timeout = 60;

    /* Pipe for child stdout+stderr (or the in-process result for web/js). */
    int pipefd[2];
    if (pipe(pipefd) != 0) die("error: net: pipe failed");

    /* Proxy: bind while single-threaded (before the fork below).
     * Skipped if no agent_dir, no sandbox, or net_mode==1 (no network). */
    ProxyContext proxy;
    int proxy_active = 0;
    if (q->agent_dir && !q->net_mode && q->sandbox) {
        if (proxy_bind(&proxy, q->agent_dir, q->host_rules, q->host_count,
                       q->deny_rules, q->deny_count) == 0)
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
    size_t total_len = 0;
    int drain_spilled = 0;
    int timed_out = drain_child(pid, pipefd[0], q->timeout, output, &out_len,
                                &total_len, q->spill_path, &drain_spilled, &status);

    /* Capture the contacted-hosts tag before proxy_stop frees the list. */
    char *hosts_json = proxy_active ? proxy_hosts_json(&proxy) : NULL;
    char *denied = proxy_active ? proxy_denied_summary(&proxy, q->egress_note)
                                : NULL;
    if (proxy_active) proxy_stop(&proxy);
    output[out_len] = '\0';

    int is_error = 0;
    char *result = format_tier_result(desc, output, out_len, timed_out, status,
                                      q->timeout, q, &is_error);

    /* Append proxy-deny summary so the model knows which hosts were blocked. */
    if (denied && result) {
        size_t rlen = strlen(result);
        size_t dlen = strlen(denied);
        char *combined = realloc(result, rlen + dlen + 1);
        if (combined) {
            memcpy(combined + rlen, denied, dlen + 1);
            result = combined;
        }
    }
    free(denied);

    { char *spilled = spill_large_result(q->spill_path, result, drain_spilled);
      if (spilled) { free(result); result = spilled; } }

    write_framed(FD_REQUEST, is_error, hosts_json, result);
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
            die("error: the namespace sandbox could not be established on "
                "this host, so the tool refused to run (fail-closed). This "
                "is an environment problem, not something you can fix — "
                "report it to the operator; retrying will not help");
    }

    int is_error = 0;
    char *result = desc->run_fn(&q, &is_error);
    if (!result) result = strdup("");
    { char *spilled = spill_large_result(q.spill_path, result, 0);
      if (spilled) { free(result); result = spilled; } }
    write_framed(FD_REQUEST, is_error, NULL, result);  /* file tier: zero-length meta */
    free(result);
    _exit(0);
}
