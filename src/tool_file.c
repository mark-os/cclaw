#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "tool_file.h"
#include "tool_parse.h"
#include "jsmn_util.h"
#include <dirent.h>
#include <fnmatch.h>
#include <limits.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define FILE_READ_MAX (256 * 1024)
#define FILE_LIST_DEFAULT_LIMIT 500
#define FIND_DEFAULT_LIMIT 1000
#define FIND_MAX_DEPTH 32

/* ── Forked sandbox runner ────────────────────────────────────────────── */

/* Run handler in a forked child under the kernel sandbox. If sandbox==0
 * (host trust or no workspace), run in-process directly. */
char *file_sandbox_run(FileReadCtx *ctx, char *(*handler)(const char *, void *),
                       const char *arguments) {
    if (!ctx->sandbox || !ctx->workspace) {
        /* Host mode / unit tests: run in-process */
        return handler(arguments, ctx);
    }

    int pipefd[2];
    if (pipe(pipefd) != 0)
        return strdup("error: pipe() failed");

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return strdup("error: fork() failed");
    }

    if (pid == 0) {
        /* Child */
        close(pipefd[0]);

        SandboxConfig cfg = {0};
        cfg.workspace    = ctx->workspace;
        cfg.cwd_path     = ctx->cwd_path;
        cfg.db_path      = ctx->db_path;
        cfg.sandbox      = 1;
        cfg.workspace_ro = ctx->workspace_ro;
        cfg.mount_cwd    = ctx->mount_cwd;
        cfg.env_mode     = ctx->env_mode;
        cfg.net_mode     = 1;  /* file ops never need network */
        cfg.proxy_sock   = NULL;
        cfg.rlimits.nproc   = ctx->rlimits.nproc;
        cfg.rlimits.as_mb   = ctx->rlimits.as_mb;
        cfg.rlimits.cpu_sec = ctx->rlimits.cpu_sec;

        /* Layer 2: build extra_mounts from read/write path grants */
        size_t n_extra = ctx->read_path_count + ctx->write_path_count;
        if (n_extra > 0) {
            cfg.extra_mounts = malloc(n_extra * sizeof(*cfg.extra_mounts));
            if (cfg.extra_mounts) {
                size_t j = 0;
                for (size_t i = 0; i < ctx->read_path_count; i++) {
                    cfg.extra_mounts[j].path = ctx->read_paths[i];
                    cfg.extra_mounts[j].ro = 1;
                    j++;
                }
                for (size_t i = 0; i < ctx->write_path_count; i++) {
                    cfg.extra_mounts[j].path = ctx->write_paths[i];
                    cfg.extra_mounts[j].ro = 0;
                    j++;
                }
                cfg.extra_mount_count = n_extra;
            }
        }

        if (sandbox_child_setup(&cfg) != 0) {
            const char *msg = "error: namespace sandbox unavailable";
            (void)write(pipefd[1], msg, strlen(msg));
            _exit(126);
        }

        char *result = handler(arguments, ctx);
        if (result) {
            size_t len = strlen(result);
            size_t written = 0;
            while (written < len) {
                ssize_t n = write(pipefd[1], result + written, len - written);
                if (n <= 0) break;
                written += (size_t)n;
            }
            free(result);
        }
        close(pipefd[1]);
        _exit(0);
    }

    /* Parent: read entire pipe into growable buffer */
    close(pipefd[1]);
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        return strdup("error: OOM");
    }
    for (;;) {
        if (len + 4096 > cap) {
            cap = cap * 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); close(pipefd[0]); waitpid(pid, NULL, 0); return strdup("error: OOM"); }
            buf = nb;
        }
        ssize_t n = read(pipefd[0], buf + len, cap - len);
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    buf[len] = '\0';

    if (len == 0) {
        free(buf);
        return strdup("error: empty response from sandbox child");
    }
    return buf;
}

/* ── Wrappers: dispatch through sandbox runner ────────────────────────── */

static char *file_read_inner(const char *arguments, void *user_data);
static char *file_write_inner(const char *arguments, void *user_data);
static char *file_list_inner(const char *arguments, void *user_data);
static char *file_find_inner(const char *arguments, void *user_data);
static char *file_edit_inner(const char *arguments, void *user_data);
static char *file_grep_inner(const char *arguments, void *user_data);

char *tool_file_read_handler(const char *arguments, void *user_data) {
    FileReadCtx *ctx = (FileReadCtx *)user_data;
    if (!ctx || !ctx->workspace) return strdup("error: no workspace configured");
    return file_sandbox_run(ctx, file_read_inner, arguments);
}

char *tool_file_write_handler(const char *arguments, void *user_data) {
    FileReadCtx *ctx = (FileReadCtx *)user_data;
    if (!ctx || !ctx->workspace) return strdup("error: no workspace configured");
    if (ctx->read_only) return strdup("error: workspace is read-only (restricted trust level)");
    return file_sandbox_run(ctx, file_write_inner, arguments);
}

char *tool_file_list_handler(const char *arguments, void *user_data) {
    FileReadCtx *ctx = (FileReadCtx *)user_data;
    if (!ctx || !ctx->workspace) return strdup("error: no workspace configured");
    return file_sandbox_run(ctx, file_list_inner, arguments);
}

char *tool_file_find_handler(const char *arguments, void *user_data) {
    FileReadCtx *ctx = (FileReadCtx *)user_data;
    if (!ctx || !ctx->workspace) return strdup("error: no workspace configured");
    return file_sandbox_run(ctx, file_find_inner, arguments);
}

char *tool_file_edit_handler(const char *arguments, void *user_data) {
    FileReadCtx *ctx = (FileReadCtx *)user_data;
    if (!ctx || !ctx->workspace) return strdup("error: no workspace configured");
    if (ctx->read_only) return strdup("error: workspace is read-only (restricted trust level)");
    return file_sandbox_run(ctx, file_edit_inner, arguments);
}

char *tool_file_grep_handler(const char *arguments, void *user_data) {
    FileReadCtx *ctx = (FileReadCtx *)user_data;
    if (!ctx || !ctx->workspace) return strdup("error: no workspace configured");
    return file_sandbox_run(ctx, file_grep_inner, arguments);
}

/* ── file_read ────────────────────────────────────────────────────────── */

static const char *FILE_READ_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"File path to read (relative or absolute)\"}"
    "},\"required\":[\"path\"]}";

static char *file_read_inner(const char *arguments, void *user_data) {
    FileReadCtx *ctx = (FileReadCtx *)user_data;
    const char *workspace = ctx->workspace;

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0)
        return strdup("error: invalid JSON arguments");

    const char *req_path = targ_str(&ta, "path");
    if (!req_path || !req_path[0]) {
        tool_parse_free(&ta);
        return strdup("error: missing or empty 'path' field");
    }

    char fullpath[PATH_MAX];
    if (req_path[0] == '/')
        snprintf(fullpath, sizeof(fullpath), "%s", req_path);
    else
        snprintf(fullpath, sizeof(fullpath), "%s/%s", workspace, req_path);
    tool_parse_free(&ta);

    FILE *f = fopen(fullpath, "rb");
    if (!f) return strdup("error: cannot open file");

    char *buf = malloc(FILE_READ_MAX + 1);
    if (!buf) { fclose(f); return strdup("error: out of memory"); }

    size_t n = fread(buf, 1, FILE_READ_MAX, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

int tool_file_read_register(ToolRegistry *reg, FileReadCtx *ctx) {
    return tools_register(reg, "file_read",
                          "Read a file (path relative or absolute)",
                          FILE_READ_PARAMS_JSON, tool_file_read_handler,
                          (void *)ctx);
}

/* ── file_write ───────────────────────────────────────────────────────── */

static const char *FILE_WRITE_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"File path to write (relative or absolute)\"},"
    "\"content\":{\"type\":\"string\",\"description\":\"Content to write\"}"
    "},\"required\":[\"path\",\"content\"]}";

static char *file_write_inner(const char *arguments, void *user_data) {
    FileReadCtx *ctx = (FileReadCtx *)user_data;
    if (ctx->read_only) return strdup("error: workspace is read-only (restricted trust level)");
    const char *workspace = ctx->workspace;

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0)
        return strdup("error: invalid JSON arguments");

    const char *req_path = targ_str(&ta, "path");
    if (!req_path || !req_path[0]) {
        tool_parse_free(&ta);
        return strdup("error: missing or empty 'path' field");
    }

    /* Reject memory-file names */
    const char *bn = strrchr(req_path, '/');
    bn = bn ? bn + 1 : req_path;
    if (strcasecmp(bn, "MEMORY.md") == 0 || strcasecmp(bn, "SOUL.md") == 0) {
        tool_parse_free(&ta);
        return strdup("error: memories are not files — use the memory tools (memory_add/memory_edit) instead of writing MEMORY.md");
    }

    const char *content = targ_str(&ta, "content");
    if (!content) {
        tool_parse_free(&ta);
        return strdup("error: missing 'content' field");
    }

    char fullpath[PATH_MAX];
    if (req_path[0] == '/')
        snprintf(fullpath, sizeof(fullpath), "%s", req_path);
    else
        snprintf(fullpath, sizeof(fullpath), "%s/%s", workspace, req_path);

    FILE *f = fopen(fullpath, "wb");
    if (!f) {
        tool_parse_free(&ta);
        return strdup("error: cannot open file for writing");
    }

    size_t content_len = strlen(content);
    size_t written = fwrite(content, 1, content_len, f);
    fclose(f);
    tool_parse_free(&ta);

    if (written != content_len)
        return strdup("error: incomplete write");

    char *result = malloc(64);
    if (!result) return strdup("ok");
    snprintf(result, 64, "wrote %zu bytes", written);
    return result;
}

int tool_file_write_register(ToolRegistry *reg, FileReadCtx *ctx) {
    return tools_register(reg, "file_write",
                          "Write content to a file (path relative or absolute)",
                          FILE_WRITE_PARAMS_JSON, tool_file_write_handler,
                          (void *)ctx);
}

/* ── file_list ────────────────────────────────────────────────────────── */

/* Skip these directory names everywhere (matches Pi's default ignores). */
static int is_ignored_dir(const char *name) {
    return strcmp(name, ".git") == 0 || strcmp(name, "node_modules") == 0;
}

static const char *FILE_LIST_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"Directory to list (relative or absolute, default '.')\"},"
    "\"limit\":{\"type\":\"number\",\"description\":\"Maximum entries to return (default 500)\"}"
    "}}";

typedef struct { char *name; int is_dir; int is_link; } LsEntry;

static int ls_entry_cmp(const void *a, const void *b) {
    return strcasecmp(((const LsEntry *)a)->name, ((const LsEntry *)b)->name);
}

static char *file_list_inner(const char *arguments, void *user_data) {
    FileReadCtx *ctx = (FileReadCtx *)user_data;
    const char *workspace = ctx->workspace;

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0)
        return strdup("error: invalid JSON arguments");
    const char *req_path = targ_str(&ta, "path");
    if (!req_path || !req_path[0]) req_path = ".";
    int limit = targ_int(&ta, "limit", FILE_LIST_DEFAULT_LIMIT);
    if (limit < 1) limit = FILE_LIST_DEFAULT_LIMIT;

    char resolved[PATH_MAX];
    if (req_path[0] == '/')
        snprintf(resolved, sizeof(resolved), "%s", req_path);
    else
        snprintf(resolved, sizeof(resolved), "%s/%s", workspace, req_path);
    tool_parse_free(&ta);

    DIR *d = opendir(resolved);
    if (!d) return strdup("error: not a directory or cannot open");

    LsEntry *entries = NULL;
    size_t n = 0, ecap = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (n == ecap) {
            ecap = ecap ? ecap * 2 : 64;
            LsEntry *ne = realloc(entries, ecap * sizeof(*entries));
            if (!ne) break;
            entries = ne;
        }
        char full[PATH_MAX + 256];
        snprintf(full, sizeof(full), "%s/%s", resolved, ent->d_name);
        struct stat lst;
        entries[n].is_link = (lstat(full, &lst) == 0 && S_ISLNK(lst.st_mode));
        struct stat st;
        entries[n].is_dir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));
        entries[n].name = strdup(ent->d_name);
        if (!entries[n].name) break;
        n++;
    }
    closedir(d);

    qsort(entries, n, sizeof(*entries), ls_entry_cmp);

    int limited = (n > (size_t)limit);
    size_t shown = limited ? (size_t)limit : n;

    size_t cap = 4096, len = 0;
    char *out = malloc(cap);
    if (out) out[0] = '\0';
    for (size_t i = 0; out && i < shown; i++) {
        const char *suffix = entries[i].is_dir ? "/" : entries[i].is_link ? "@" : "";
        size_t need = len + strlen(entries[i].name) + 2;
        if (need + 1 > cap) {
            cap = need * 2;
            char *nb = realloc(out, cap);
            if (!nb) { free(out); out = NULL; break; }
            out = nb;
        }
        len += (size_t)snprintf(out + len, cap - len, "%s%s\n", entries[i].name, suffix);
    }
    for (size_t i = 0; i < n; i++) free(entries[i].name);
    free(entries);

    if (!out) return strdup("error: OOM");
    if (len == 0) { free(out); return strdup("(empty directory)"); }
    if (out[len - 1] == '\n') out[len - 1] = '\0';
    if (limited) {
        size_t need = len + 48;
        char *nb = realloc(out, need);
        if (nb) { out = nb; snprintf(out + len, need - len, "\n\n[%d entries limit reached]", limit); }
    }
    return out;
}

int tool_file_list_register(ToolRegistry *reg, FileReadCtx *ctx) {
    return tools_register(reg, "file_list",
                          "List directory contents. Returns entries sorted "
                          "alphabetically, with a '/' suffix for directories. Includes dotfiles. "
                          "Use this to see what files exist.",
                          FILE_LIST_PARAMS_JSON, tool_file_list_handler, (void *)ctx);
}

/* ── file_find (glob) ─────────────────────────────────────────────────── */

static const char *FIND_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"pattern\":{\"type\":\"string\",\"description\":\"Glob pattern, e.g. '*.c', '**/*.json', or 'src/**/*.spec.ts'. A pattern with no '/' matches a file's name at any depth; '**' matches across directories.\"},"
    "\"path\":{\"type\":\"string\",\"description\":\"Directory to search in (relative or absolute, default '.')\"},"
    "\"limit\":{\"type\":\"number\",\"description\":\"Maximum results (default 1000)\"}"
    "},\"required\":[\"pattern\"]}";

static int glob_match(const char *pat, const char *str) {
    while (*pat) {
        if (pat[0] == '*' && pat[1] == '*') {
            pat += 2;
            while (*pat == '/') pat++;
            if (*pat == '\0') return 1;
            for (const char *s = str;; s++) {
                if (glob_match(pat, s)) return 1;
                if (*s == '\0') return 0;
            }
        } else if (*pat == '*') {
            pat++;
            for (const char *s = str;; s++) {
                if (glob_match(pat, s)) return 1;
                if (*s == '\0' || *s == '/') return 0;
            }
        } else if (*pat == '?') {
            if (*str == '\0' || *str == '/') return 0;
            pat++; str++;
        } else {
            if (*pat != *str) return 0;
            pat++; str++;
        }
    }
    return *str == '\0';
}

typedef struct {
    char *buf;
    size_t cap, len;
    int count;
    int limit;
    const char *pattern;
    int path_mode;
} FindAcc;

static void find_walk(const char *absdir, const char *reldir, int depth, FindAcc *a) {
    if (depth > FIND_MAX_DEPTH || a->count >= a->limit) return;
    DIR *d = opendir(absdir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (a->count >= a->limit) break;

        char absfull[PATH_MAX + 256];
        snprintf(absfull, sizeof(absfull), "%s/%s", absdir, ent->d_name);
        char relfull[PATH_MAX + 256];
        snprintf(relfull, sizeof(relfull), reldir[0] ? "%s/%s" : "%s%s",
                 reldir, ent->d_name);

        struct stat st;
        if (stat(absfull, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (is_ignored_dir(ent->d_name)) continue;
            find_walk(absfull, relfull, depth + 1, a);
            continue;
        }

        int hit = a->path_mode ? glob_match(a->pattern, relfull)
                               : (fnmatch(a->pattern, ent->d_name, 0) == 0);
        if (!hit) continue;

        size_t need = a->len + strlen(relfull) + 2;
        if (need + 1 > a->cap) {
            size_t nc = need * 2;
            char *nb = realloc(a->buf, nc);
            if (!nb) { closedir(d); return; }
            a->buf = nb; a->cap = nc;
        }
        a->len += (size_t)snprintf(a->buf + a->len, a->cap - a->len, "%s\n", relfull);
        a->count++;
    }
    closedir(d);
}

static char *file_find_inner(const char *arguments, void *user_data) {
    FileReadCtx *ctx = (FileReadCtx *)user_data;
    const char *workspace = ctx->workspace;

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0)
        return strdup("error: invalid JSON arguments");
    const char *pattern = targ_str(&ta, "pattern");
    if (!pattern || !pattern[0]) {
        tool_parse_free(&ta);
        return strdup("error: missing or empty 'pattern' field");
    }
    const char *req_path = targ_str(&ta, "path");
    if (!req_path || !req_path[0]) req_path = ".";
    int limit = targ_int(&ta, "limit", FIND_DEFAULT_LIMIT);
    if (limit < 1) limit = FIND_DEFAULT_LIMIT;

    char eff[300];
    int path_mode = (strchr(pattern, '/') != NULL);
    if (path_mode && strncmp(pattern, "**/", 3) != 0 && pattern[0] != '/' &&
        strcmp(pattern, "**") != 0)
        snprintf(eff, sizeof(eff), "**/%s", pattern);
    else
        snprintf(eff, sizeof(eff), "%s", pattern);

    char resolved[PATH_MAX];
    if (req_path[0] == '/')
        snprintf(resolved, sizeof(resolved), "%s", req_path);
    else
        snprintf(resolved, sizeof(resolved), "%s/%s", workspace, req_path);
    tool_parse_free(&ta);

    FindAcc a = {.buf = malloc(4096), .cap = 4096, .len = 0, .count = 0,
                 .limit = limit, .pattern = eff, .path_mode = path_mode};
    if (!a.buf) return strdup("error: OOM");
    a.buf[0] = '\0';

    find_walk(resolved, "", 0, &a);

    if (a.len == 0) { free(a.buf); return strdup("No files found matching pattern"); }
    if (a.buf[a.len - 1] == '\n') a.buf[--a.len] = '\0';
    if (a.count >= limit) {
        size_t need = a.len + 48;
        char *nb = realloc(a.buf, need);
        if (nb) { a.buf = nb; snprintf(a.buf + a.len, need - a.len, "\n\n[%d results limit reached]", limit); }
    }
    return a.buf;
}

int tool_file_find_register(ToolRegistry *reg, FileReadCtx *ctx) {
    return tools_register(reg, "file_find",
                          "Search for files by glob pattern. Returns matching "
                          "file paths relative to the search directory. A pattern without '/' (e.g. "
                          "'*.c') matches a file's name at any depth; use '**' to cross directories "
                          "(e.g. 'src/**/*.spec.ts'). Skips .git and node_modules.",
                          FIND_PARAMS_JSON, tool_file_find_handler, (void *)ctx);
}

/* ── file_edit (search/replace) ───────────────────────────────────────── */

#define FILE_EDIT_MAX_EDITS 32
#define FILE_EDIT_MAX_TOKENS 512
#define FILE_EDIT_MAX_FILE (1024 * 1024)

static const char *FILE_EDIT_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"File to edit (relative or absolute)\"},"
    "\"edits\":{\"type\":\"array\",\"description\":\"List of replacements applied to the original file\","
    "\"items\":{\"type\":\"object\",\"properties\":{"
    "\"oldText\":{\"type\":\"string\",\"description\":\"Exact text to find; must occur exactly once\"},"
    "\"newText\":{\"type\":\"string\",\"description\":\"Replacement text\"}},"
    "\"required\":[\"oldText\",\"newText\"]}}"
    "},\"required\":[\"path\",\"edits\"]}";

typedef struct {
    char *old_text; size_t old_len;
    char *new_text; size_t new_len;
    size_t off;
} EditOp;

static int jtok_key_eq(const jsmntok_t *t, const char *json, const char *key) {
    size_t klen = strlen(key);
    return t->type == JSMN_STRING && (size_t)(t->end - t->start) == klen &&
           memcmp(json + t->start, key, klen) == 0;
}

static int jtok_find(const jsmntok_t *t, int ntok, const char *json, const char *key) {
    int j = 1;
    for (int k = 0; k < t[0].size; k++) {
        int vi = j + 1;
        if (jtok_key_eq(&t[j], json, key)) return vi;
        j = jsmn_skip(t, vi, ntok);
    }
    return -1;
}

static int mem_count(const char *hay, size_t hlen, const char *ned, size_t nlen) {
    if (nlen == 0 || nlen > hlen) return 0;
    int c = 0;
    for (size_t i = 0; i + nlen <= hlen;) {
        if (memcmp(hay + i, ned, nlen) == 0) { c++; i += nlen; } else i++;
    }
    return c;
}

static long mem_offset(const char *hay, size_t hlen, const char *ned, size_t nlen) {
    if (nlen == 0 || nlen > hlen) return -1;
    for (size_t i = 0; i + nlen <= hlen; i++)
        if (memcmp(hay + i, ned, nlen) == 0) return (long)i;
    return -1;
}

static int edit_off_cmp(const void *a, const void *b) {
    size_t oa = ((const EditOp *)a)->off, ob = ((const EditOp *)b)->off;
    return (oa > ob) - (oa < ob);
}

static void edits_free(EditOp *e, int n) {
    for (int i = 0; i < n; i++) { free(e[i].old_text); free(e[i].new_text); }
}

static char *file_edit_inner(const char *arguments, void *user_data) {
    FileReadCtx *ctx = (FileReadCtx *)user_data;
    if (ctx->read_only) return strdup("error: workspace is read-only (restricted trust level)");
    if (!arguments) return strdup("error: invalid JSON arguments");

    jsmntok_t toks[FILE_EDIT_MAX_TOKENS];
    jsmn_parser p;
    jsmn_init(&p);
    int nt = jsmn_parse(&p, arguments, strlen(arguments), toks, FILE_EDIT_MAX_TOKENS);
    if (nt < 1 || toks[0].type != JSMN_OBJECT)
        return strdup("error: invalid JSON arguments (or too many edits)");

    int pvi = jtok_find(toks, nt, arguments, "path");
    int avi = jtok_find(toks, nt, arguments, "edits");
    if (pvi < 0 || toks[pvi].type != JSMN_STRING)
        return strdup("error: missing or invalid 'path'");
    if (avi < 0 || toks[avi].type != JSMN_ARRAY)
        return strdup("error: missing or invalid 'edits' array");
    if (toks[avi].size < 1) return strdup("error: 'edits' is empty");
    if (toks[avi].size > FILE_EDIT_MAX_EDITS) return strdup("error: too many edits");

    char req_path[PATH_MAX];
    size_t plen = (size_t)(toks[pvi].end - toks[pvi].start);
    if (plen >= sizeof(req_path)) return strdup("error: path too long");
    plen = json_unescape(req_path, sizeof(req_path), arguments + toks[pvi].start, plen);
    req_path[plen] = '\0';

    char fullpath[PATH_MAX * 2];
    if (req_path[0] == '/') snprintf(fullpath, sizeof(fullpath), "%s", req_path);
    else snprintf(fullpath, sizeof(fullpath), "%s/%s", ctx->workspace, req_path);

    FILE *f = fopen(fullpath, "rb");
    if (!f) return strdup("error: cannot open file");
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    if (fsz < 0 || fsz > FILE_EDIT_MAX_FILE) { fclose(f); return strdup("error: file too large"); }
    fseek(f, 0, SEEK_SET);
    char *orig = malloc((size_t)fsz + 1);
    if (!orig) { fclose(f); return strdup("error: OOM"); }
    size_t olen = fread(orig, 1, (size_t)fsz, f);
    fclose(f);
    orig[olen] = '\0';

    EditOp ops[FILE_EDIT_MAX_EDITS];
    int nedits = 0;
    int ei = avi + 1;
    char *errmsg = NULL;
    for (int e = 0; e < toks[avi].size; e++) {
        if (toks[ei].type != JSMN_OBJECT) { errmsg = "error: edit is not an object"; break; }
        int npairs = toks[ei].size, fj = ei + 1;
        const char *op = NULL, *np = NULL; size_t ol = 0, nl = 0; int have_old = 0, have_new = 0;
        for (int pp = 0; pp < npairs; pp++) {
            const jsmntok_t *kt = &toks[fj];
            const jsmntok_t *vt = &toks[fj + 1];
            if (jtok_key_eq(kt, arguments, "oldText") && vt->type == JSMN_STRING) {
                op = arguments + vt->start; ol = (size_t)(vt->end - vt->start); have_old = 1;
            } else if (jtok_key_eq(kt, arguments, "newText") && vt->type == JSMN_STRING) {
                np = arguments + vt->start; nl = (size_t)(vt->end - vt->start); have_new = 1;
            }
            fj = jsmn_skip(toks, fj + 1, nt);
        }
        ei = jsmn_skip(toks, ei, nt);
        if (!have_old || !have_new) { errmsg = "error: each edit needs oldText and newText"; break; }

        EditOp *o = &ops[nedits];
        o->old_text = malloc(ol + 1); o->new_text = malloc(nl + 1);
        if (!o->old_text || !o->new_text) { free(o->old_text); free(o->new_text); errmsg = "error: OOM"; break; }
        o->old_len = json_unescape(o->old_text, ol + 1, op, ol);
        o->new_len = json_unescape(o->new_text, nl + 1, np, nl);
        o->old_text[o->old_len] = '\0'; o->new_text[o->new_len] = '\0';

        if (o->old_len == 0) { free(o->old_text); free(o->new_text); errmsg = "error: oldText is empty"; break; }
        int matches = mem_count(orig, olen, o->old_text, o->old_len);
        if (matches == 0) { free(o->old_text); free(o->new_text); errmsg = "error: oldText not found"; break; }
        if (matches > 1) { free(o->old_text); free(o->new_text); errmsg = "error: oldText not unique (matches multiple times)"; break; }
        o->off = (size_t)mem_offset(orig, olen, o->old_text, o->old_len);
        nedits++;
    }
    if (errmsg) { edits_free(ops, nedits); free(orig); return strdup(errmsg); }

    qsort(ops, (size_t)nedits, sizeof(EditOp), edit_off_cmp);
    for (int i = 1; i < nedits; i++) {
        if (ops[i].off < ops[i - 1].off + ops[i - 1].old_len) {
            edits_free(ops, nedits); free(orig);
            return strdup("error: overlapping edits");
        }
    }

    size_t new_total = olen;
    for (int i = 0; i < nedits; i++) new_total = new_total - ops[i].old_len + ops[i].new_len;
    char *out = malloc(new_total + 1);
    if (!out) { edits_free(ops, nedits); free(orig); return strdup("error: OOM"); }
    size_t cursor = 0, w = 0;
    for (int i = 0; i < nedits; i++) {
        size_t chunk = ops[i].off - cursor;
        memcpy(out + w, orig + cursor, chunk); w += chunk;
        memcpy(out + w, ops[i].new_text, ops[i].new_len); w += ops[i].new_len;
        cursor = ops[i].off + ops[i].old_len;
    }
    memcpy(out + w, orig + cursor, olen - cursor); w += olen - cursor;
    out[w] = '\0';

    edits_free(ops, nedits);
    free(orig);

    f = fopen(fullpath, "wb");
    if (!f) { free(out); return strdup("error: cannot open file for writing"); }
    size_t written = fwrite(out, 1, w, f);
    fclose(f);
    free(out);
    if (written != w) return strdup("error: incomplete write");

    char *res = malloc(48);
    if (!res) return strdup("ok");
    snprintf(res, 48, "applied %d edit%s", nedits, nedits == 1 ? "" : "s");
    return res;
}

int tool_file_edit_register(ToolRegistry *reg, FileReadCtx *ctx) {
    return tools_register(reg, "file_edit",
                          "Apply targeted search/replace edits to a file in the workspace, without "
                          "rewriting the whole file. Each edit's 'oldText' must occur exactly once in "
                          "the file; all edits are matched against the original content and must not "
                          "overlap. Use file_write to create a file or replace it entirely.",
                          FILE_EDIT_PARAMS_JSON, tool_file_edit_handler, (void *)ctx);
}

/* ── file_grep (content search) ───────────────────────────────────────── */

#define GREP_DEFAULT_LIMIT 1000
#define GREP_MAX_FILE_SIZE (1024 * 1024)

static const char *GREP_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"pattern\":{\"type\":\"string\",\"description\":\"POSIX extended regex matched per line\"},"
    "\"path\":{\"type\":\"string\",\"description\":\"Directory to search (relative or absolute, default '.')\"},"
    "\"glob\":{\"type\":\"string\",\"description\":\"Only search files whose basename matches this glob (e.g. '*.c')\"},"
    "\"limit\":{\"type\":\"number\",\"description\":\"Maximum match lines (default 1000)\"}"
    "},\"required\":[\"pattern\"]}";

typedef struct {
    char *buf;
    size_t cap, len;
    int count;
    int limit;
    regex_t re;
    const char *glob;
} GrepAcc;

static void grep_file(const char *abspath, const char *relpath, GrepAcc *a) {
    if (a->count >= a->limit) return;

    struct stat st;
    if (stat(abspath, &st) != 0 || !S_ISREG(st.st_mode)) return;
    if (st.st_size > GREP_MAX_FILE_SIZE || st.st_size == 0) return;

    FILE *f = fopen(abspath, "rb");
    if (!f) return;

    char probe[512];
    size_t n = fread(probe, 1, sizeof(probe), f);
    if (memchr(probe, '\0', n)) { fclose(f); return; }
    fseek(f, 0, SEEK_SET);

    char *line = NULL;
    size_t linesz = 0;
    int lineno = 0;
    while (getline(&line, &linesz, f) != -1) {
        lineno++;
        if (a->count >= a->limit) break;
        if (regexec(&a->re, line, 0, NULL, 0) == 0) {
            size_t ll = strlen(line);
            while (ll > 0 && (line[ll - 1] == '\n' || line[ll - 1] == '\r'))
                ll--;
            char hdr[PATH_MAX + 256];
            int hlen = snprintf(hdr, sizeof(hdr), "%s:%d:", relpath, lineno);
            if (hlen < 0) continue;
            if ((size_t)hlen >= sizeof(hdr)) hlen = (int)sizeof(hdr) - 1;
            size_t total = (size_t)hlen + ll;
            size_t need = a->len + total + 2;
            if (need > a->cap) {
                size_t nc = need * 2;
                char *nb = realloc(a->buf, nc);
                if (!nb) break;
                a->buf = nb; a->cap = nc;
            }
            memcpy(a->buf + a->len, hdr, (size_t)hlen);
            a->len += (size_t)hlen;
            memcpy(a->buf + a->len, line, ll);
            a->len += ll;
            a->buf[a->len++] = '\n';
            a->buf[a->len] = '\0';
            a->count++;
        }
    }
    free(line);
    fclose(f);
}

static void grep_walk(const char *absdir, const char *reldir, int depth, GrepAcc *a) {
    if (depth > FIND_MAX_DEPTH || a->count >= a->limit) return;
    DIR *d = opendir(absdir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (a->count >= a->limit) break;

        char absfull[PATH_MAX + 256];
        snprintf(absfull, sizeof(absfull), "%s/%s", absdir, ent->d_name);
        char relfull[PATH_MAX + 256];
        snprintf(relfull, sizeof(relfull), reldir[0] ? "%s/%s" : "%s%s",
                 reldir, ent->d_name);

        struct stat st;
        if (stat(absfull, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (is_ignored_dir(ent->d_name)) continue;
            grep_walk(absfull, relfull, depth + 1, a);
            continue;
        }

        if (a->glob && fnmatch(a->glob, ent->d_name, 0) != 0) continue;

        grep_file(absfull, relfull, a);
    }
    closedir(d);
}

static char *file_grep_inner(const char *arguments, void *user_data) {
    FileReadCtx *ctx = (FileReadCtx *)user_data;
    const char *workspace = ctx->workspace;

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0)
        return strdup("error: invalid JSON arguments");
    const char *pattern = targ_str(&ta, "pattern");
    if (!pattern || !pattern[0]) {
        tool_parse_free(&ta);
        return strdup("error: missing or empty 'pattern' field");
    }
    const char *req_path = targ_str(&ta, "path");
    if (!req_path || !req_path[0]) req_path = ".";
    const char *glob_arg = targ_str(&ta, "glob");
    char glob_buf[256];
    if (glob_arg && glob_arg[0])
        snprintf(glob_buf, sizeof(glob_buf), "%s", glob_arg);
    else
        glob_buf[0] = '\0';
    int limit = targ_int(&ta, "limit", GREP_DEFAULT_LIMIT);
    if (limit < 1) limit = GREP_DEFAULT_LIMIT;

    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED | REG_NEWLINE) != 0) {
        tool_parse_free(&ta);
        return strdup("error: invalid regex");
    }

    char resolved[PATH_MAX];
    if (req_path[0] == '/')
        snprintf(resolved, sizeof(resolved), "%s", req_path);
    else
        snprintf(resolved, sizeof(resolved), "%s/%s", workspace, req_path);
    tool_parse_free(&ta);

    GrepAcc a = {.buf = malloc(4096), .cap = 4096, .len = 0, .count = 0,
                 .limit = limit, .re = re, .glob = glob_buf[0] ? glob_buf : NULL};
    if (!a.buf) { regfree(&re); return strdup("error: OOM"); }
    a.buf[0] = '\0';

    grep_walk(resolved, "", 0, &a);
    regfree(&re);

    if (a.len == 0) { free(a.buf); return strdup("No matches found"); }
    if (a.buf[a.len - 1] == '\n') a.buf[--a.len] = '\0';
    if (a.count >= limit) {
        size_t need = a.len + 48;
        char *nb = realloc(a.buf, need);
        if (nb) { a.buf = nb; snprintf(a.buf + a.len, need - a.len, "\n\n[%d results limit reached]", limit); }
    }
    return a.buf;
}

int tool_file_grep_register(ToolRegistry *reg, FileReadCtx *ctx) {
    return tools_register(reg, "file_grep",
                          "Search file contents for lines matching a POSIX extended regex. "
                          "Returns matching lines as path:lineno:line. Searches recursively "
                          "under the given directory. Skips binary files, .git, and node_modules.",
                          GREP_PARAMS_JSON, tool_file_grep_handler, (void *)ctx);
}
