#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "tool_file.h"
#include "buf.h"
#include "run_tool.h"
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

/* All six file tools are EXEC_SANDBOX: they run only inside the --run-tool
 * child, on pre-extracted wire params (run_tool_param_*) — no JSON is parsed
 * in this process. The registry entries carry tool_sandboxed_stub. */

static char *file_read_run(const RunToolParsed *q, FileReadCtx *ctx);
static char *file_write_run(const RunToolParsed *q, FileReadCtx *ctx);
static char *file_list_run(const RunToolParsed *q, FileReadCtx *ctx);
static char *file_find_run(const RunToolParsed *q, FileReadCtx *ctx);
static char *file_edit_run(const RunToolParsed *q, FileReadCtx *ctx);
static char *file_grep_run(const RunToolParsed *q, FileReadCtx *ctx);

/* ── Actionable denials ───────────────────────────────────────────────── */

/* Component-boundary prefix test: path equals base or lies under it. */
static int path_under(const char *base, const char *path) {
    if (!base || !base[0]) return 0;
    size_t blen = strlen(base);
    while (blen > 1 && base[blen - 1] == '/') blen--;
    if (strncmp(path, base, blen) != 0) return 0;
    return path[blen] == '\0' || path[blen] == '/';
}

/* Inside the mount sandbox an ungranted path is indistinguishable from a
 * missing file. When an open fails on a path outside every mounted area,
 * tell the model which grant to request instead of a bare "cannot open".
 * suggest_parent: propose granting the containing directory (file targets)
 * rather than the path itself (directory targets). Returns malloc'd hint or
 * NULL (path is within the granted set, or no mount enforcement is active). */
static char *path_grant_hint(const FileReadCtx *ctx, const char *fullpath,
                             int want_write, int suggest_parent) {
    if (!ctx->sb.sandbox) return NULL; /* host trust / direct call: nothing hidden */
    if (path_under(ctx->workspace, fullpath) &&
        !(want_write && ctx->sb.workspace_ro))
        return NULL;
    if (ctx->sb.mount_cwd && ctx->cwd_path && path_under(ctx->cwd_path, fullpath))
        return NULL;
    for (size_t i = 0; i < ctx->sb.write_path_count; i++)
        if (path_under(ctx->sb.write_paths[i], fullpath)) return NULL;
    if (!want_write)
        for (size_t i = 0; i < ctx->sb.read_path_count; i++)
            if (path_under(ctx->sb.read_paths[i], fullpath)) return NULL;

    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", fullpath);
    if (suggest_parent) {
        char *sl = strrchr(dir, '/');
        if (sl) { if (sl == dir) sl[1] = '\0'; else *sl = '\0'; }
    }
    size_t cap = strlen(fullpath) + strlen(dir) + 160;
    char *msg = malloc(cap);
    if (!msg) return NULL;
    snprintf(msg, cap,
             "error: '%s' is outside your granted areas — request access with "
             "request_config {\"action\":\"grant_path\",\"path\":\"%s\",\"mode\":\"%s\"}",
             fullpath, dir, want_write ? "write" : "read");
    return msg;
}

/* ── file_read ────────────────────────────────────────────────────────── */

static const char *FILE_READ_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"File path to read (relative or absolute)\"}"
    "},\"required\":[\"path\"]}";

static char *file_read_run(const RunToolParsed *q, FileReadCtx *ctx) {
    const char *workspace = ctx->workspace;

    const char *req_path = run_tool_param_str(q, "path");
    if (!req_path || !req_path[0])
        return strdup("error: missing or empty 'path' field");

    char fullpath[PATH_MAX];
    if (req_path[0] == '/')
        snprintf(fullpath, sizeof(fullpath), "%s", req_path);
    else
        snprintf(fullpath, sizeof(fullpath), "%s/%s", workspace, req_path);

    FILE *f = fopen(fullpath, "rb");
    if (!f) {
        char *hint = path_grant_hint(ctx, fullpath, 0, 1);
        return hint ? hint : strdup("error: cannot open file");
    }

    char *buf = malloc(FILE_READ_MAX + 1);
    if (!buf) { fclose(f); return strdup("error: out of memory"); }

    size_t n = fread(buf, 1, FILE_READ_MAX, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

int tool_file_read_register(ToolRegistry *reg, FileReadCtx *ctx) {
    int rc = tools_register(reg, "file_read",
                          "Read a file (path relative or absolute)",
                          FILE_READ_PARAMS_JSON, tool_sandboxed_stub,
                          (void *)ctx);
    if (rc == 0)
        tools_set_recipe(reg, "file_read", (ToolRecipe){EXEC_SANDBOX, SBX_FILE, NULL});
    return rc;
}

/* ── file_write ───────────────────────────────────────────────────────── */

static const char *FILE_WRITE_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"path\":{\"type\":\"string\",\"description\":\"File path to write (relative or absolute)\"},"
    "\"content\":{\"type\":\"string\",\"description\":\"Content to write\"}"
    "},\"required\":[\"path\",\"content\"]}";

static char *file_write_run(const RunToolParsed *q, FileReadCtx *ctx) {
    if (ctx->sb.workspace_ro) return strdup("error: workspace is read-only (restricted sandbox profile)");
    const char *workspace = ctx->workspace;

    const char *req_path = run_tool_param_str(q, "path");
    if (!req_path || !req_path[0])
        return strdup("error: missing or empty 'path' field");

    /* Reject memory-file names */
    const char *bn = strrchr(req_path, '/');
    bn = bn ? bn + 1 : req_path;
    if (strcasecmp(bn, "MEMORY.md") == 0 || strcasecmp(bn, "SOUL.md") == 0)
        return strdup("error: memories are not files — use the memory tools (memory_add/memory_edit) instead of writing MEMORY.md");

    const char *content = run_tool_param_str(q, "content");
    if (!content)
        return strdup("error: missing 'content' field");

    char fullpath[PATH_MAX];
    if (req_path[0] == '/')
        snprintf(fullpath, sizeof(fullpath), "%s", req_path);
    else
        snprintf(fullpath, sizeof(fullpath), "%s/%s", workspace, req_path);

    FILE *f = fopen(fullpath, "wb");
    if (!f) {
        char *hint = path_grant_hint(ctx, fullpath, 1, 1);
        return hint ? hint : strdup("error: cannot open file for writing");
    }

    size_t content_len = strlen(content);
    size_t written = fwrite(content, 1, content_len, f);
    fclose(f);

    if (written != content_len)
        return strdup("error: incomplete write");

    char *result = malloc(64);
    if (!result) return strdup("ok");
    snprintf(result, 64, "wrote %zu bytes", written);
    return result;
}

int tool_file_write_register(ToolRegistry *reg, FileReadCtx *ctx) {
    int rc = tools_register(reg, "file_write",
                          "Write content to a file (path relative or absolute)",
                          FILE_WRITE_PARAMS_JSON, tool_sandboxed_stub,
                          (void *)ctx);
    if (rc == 0)
        tools_set_recipe(reg, "file_write", (ToolRecipe){EXEC_SANDBOX, SBX_FILE, NULL});
    return rc;
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

static char *file_list_run(const RunToolParsed *q, FileReadCtx *ctx) {
    const char *workspace = ctx->workspace;

    const char *req_path = run_tool_param_str(q, "path");
    if (!req_path || !req_path[0]) req_path = ".";
    int limit = run_tool_param_int(q, "limit", FILE_LIST_DEFAULT_LIMIT);
    if (limit < 1) limit = FILE_LIST_DEFAULT_LIMIT;

    char resolved[PATH_MAX];
    if (req_path[0] == '/')
        snprintf(resolved, sizeof(resolved), "%s", req_path);
    else
        snprintf(resolved, sizeof(resolved), "%s/%s", workspace, req_path);

    DIR *d = opendir(resolved);
    if (!d) {
        char *hint = path_grant_hint(ctx, resolved, 0, 0);
        return hint ? hint : strdup("error: not a directory or cannot open");
    }

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

    Buf b = {0};
    for (size_t i = 0; i < shown; i++) {
        const char *suffix = entries[i].is_dir ? "/" : entries[i].is_link ? "@" : "";
        buf_appendf(&b, "%s%s\n", entries[i].name, suffix);
    }
    for (size_t i = 0; i < n; i++) free(entries[i].name);
    free(entries);

    if (b.len == 0) { buf_free(&b); return strdup("(empty directory)"); }
    /* Strip trailing newline */
    b.data[--b.len] = '\0';
    if (limited)
        buf_appendf(&b, "\n\n[%d entries limit reached]", limit);

    char *out = buf_take(&b);
    return out ? out : strdup("error: OOM");
}

int tool_file_list_register(ToolRegistry *reg, FileReadCtx *ctx) {
    int rc = tools_register(reg, "file_list",
                          "List directory contents. Returns entries sorted "
                          "alphabetically, with a '/' suffix for directories. Includes dotfiles. "
                          "Use this to see what files exist.",
                          FILE_LIST_PARAMS_JSON, tool_sandboxed_stub, (void *)ctx);
    if (rc == 0)
        tools_set_recipe(reg, "file_list", (ToolRecipe){EXEC_SANDBOX, SBX_FILE, NULL});
    return rc;
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
    Buf b;
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

        buf_appendf(&a->b, "%s\n", relfull);
        a->count++;
    }
    closedir(d);
}

static char *file_find_run(const RunToolParsed *q, FileReadCtx *ctx) {
    const char *workspace = ctx->workspace;

    const char *pattern = run_tool_param_str(q, "pattern");
    if (!pattern || !pattern[0])
        return strdup("error: missing or empty 'pattern' field");
    const char *req_path = run_tool_param_str(q, "path");
    if (!req_path || !req_path[0]) req_path = ".";
    int limit = run_tool_param_int(q, "limit", FIND_DEFAULT_LIMIT);
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

    FindAcc a = {.b = {0}, .count = 0,
                 .limit = limit, .pattern = eff, .path_mode = path_mode};

    find_walk(resolved, "", 0, &a);

    if (a.b.len == 0) { buf_free(&a.b); return strdup("No files found matching pattern"); }
    /* Strip trailing newline */
    a.b.data[--a.b.len] = '\0';
    if (a.count >= limit)
        buf_appendf(&a.b, "\n\n[%d results limit reached]", limit);

    char *out = buf_take(&a.b);
    return out ? out : strdup("error: OOM");
}

int tool_file_find_register(ToolRegistry *reg, FileReadCtx *ctx) {
    int rc = tools_register(reg, "file_find",
                          "Search for files by glob pattern. Returns matching "
                          "file paths relative to the search directory. A pattern without '/' (e.g. "
                          "'*.c') matches a file's name at any depth; use '**' to cross directories "
                          "(e.g. 'src/**/*.spec.ts'). Skips .git and node_modules.",
                          FIND_PARAMS_JSON, tool_sandboxed_stub, (void *)ctx);
    if (rc == 0)
        tools_set_recipe(reg, "file_find", (ToolRecipe){EXEC_SANDBOX, SBX_FILE, NULL});
    return rc;
}

/* ── file_edit (search/replace) ───────────────────────────────────────── */

#define FILE_EDIT_MAX_EDITS 32
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

static char *file_edit_run(const RunToolParsed *q, FileReadCtx *ctx) {
    if (ctx->sb.workspace_ro) return strdup("error: workspace is read-only (restricted sandbox profile)");

    const char *req_path = run_tool_param_str(q, "path");
    if (!req_path || !req_path[0])
        return strdup("error: missing or invalid 'path'");
    /* edits arrive flattened by the parent (tool_args_extract) as
     * [old1,new1,old2,new2,...] — no JSON in this process. */
    size_t pn = 0;
    char **pairs = run_tool_param_list(q, "edits", &pn);
    if (!pairs || pn < 2 || (pn % 2) != 0)
        return strdup("error: missing or invalid 'edits' array");
    size_t nreq = pn / 2;
    if (nreq > FILE_EDIT_MAX_EDITS) return strdup("error: too many edits");

    char fullpath[PATH_MAX * 2];
    if (req_path[0] == '/') snprintf(fullpath, sizeof(fullpath), "%s", req_path);
    else snprintf(fullpath, sizeof(fullpath), "%s/%s", ctx->workspace, req_path);

    FILE *f = fopen(fullpath, "rb");
    if (!f) {
        char *hint = path_grant_hint(ctx, fullpath, 1, 1);
        return hint ? hint : strdup("error: cannot open file");
    }
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
    char *errmsg = NULL;
    for (size_t e = 0; e < nreq; e++) {
        /* The wire's length-0 = NULL convention: an empty newText (pure
         * deletion) arrives as NULL. oldText must be non-empty anyway. */
        const char *op = pairs[e * 2] ? pairs[e * 2] : "";
        const char *np = pairs[e * 2 + 1] ? pairs[e * 2 + 1] : "";

        EditOp *o = &ops[nedits];
        o->old_text = strdup(op); o->new_text = strdup(np);
        if (!o->old_text || !o->new_text) { free(o->old_text); free(o->new_text); errmsg = "error: OOM"; break; }
        o->old_len = strlen(o->old_text);
        o->new_len = strlen(o->new_text);

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
    if (!f) {
        free(out);
        char *hint = path_grant_hint(ctx, fullpath, 1, 1);
        return hint ? hint : strdup("error: cannot open file for writing");
    }
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
    int rc = tools_register(reg, "file_edit",
                          "Apply targeted search/replace edits to a file in the workspace, without "
                          "rewriting the whole file. Each edit's 'oldText' must occur exactly once in "
                          "the file; all edits are matched against the original content and must not "
                          "overlap. Use file_write to create a file or replace it entirely.",
                          FILE_EDIT_PARAMS_JSON, tool_sandboxed_stub, (void *)ctx);
    if (rc == 0)
        tools_set_recipe(reg, "file_edit", (ToolRecipe){EXEC_SANDBOX, SBX_FILE, NULL});
    return rc;
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
    Buf b;
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
            buf_appendf(&a->b, "%s:%d:", relpath, lineno);
            buf_append(&a->b, line, ll);
            buf_append_char(&a->b, '\n');
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

static char *file_grep_run(const RunToolParsed *q, FileReadCtx *ctx) {
    const char *workspace = ctx->workspace;

    const char *pattern = run_tool_param_str(q, "pattern");
    if (!pattern || !pattern[0])
        return strdup("error: missing or empty 'pattern' field");
    const char *req_path = run_tool_param_str(q, "path");
    if (!req_path || !req_path[0]) req_path = ".";
    const char *glob_arg = run_tool_param_str(q, "glob");
    char glob_buf[256];
    if (glob_arg && glob_arg[0])
        snprintf(glob_buf, sizeof(glob_buf), "%s", glob_arg);
    else
        glob_buf[0] = '\0';
    int limit = run_tool_param_int(q, "limit", GREP_DEFAULT_LIMIT);
    if (limit < 1) limit = GREP_DEFAULT_LIMIT;

    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED | REG_NEWLINE) != 0)
        return strdup("error: invalid regex");

    char resolved[PATH_MAX];
    if (req_path[0] == '/')
        snprintf(resolved, sizeof(resolved), "%s", req_path);
    else
        snprintf(resolved, sizeof(resolved), "%s/%s", workspace, req_path);

    GrepAcc a = {.b = {0}, .count = 0,
                 .limit = limit, .re = re, .glob = glob_buf[0] ? glob_buf : NULL};

    grep_walk(resolved, "", 0, &a);
    regfree(&re);

    if (a.b.len == 0) { buf_free(&a.b); return strdup("No matches found"); }
    /* Strip trailing newline */
    a.b.data[--a.b.len] = '\0';
    if (a.count >= limit)
        buf_appendf(&a.b, "\n\n[%d results limit reached]", limit);

    char *out = buf_take(&a.b);
    return out ? out : strdup("error: OOM");
}

int tool_file_grep_register(ToolRegistry *reg, FileReadCtx *ctx) {
    int rc = tools_register(reg, "file_grep",
                          "Search file contents for lines matching a POSIX extended regex. "
                          "Returns matching lines as path:lineno:line. Searches recursively "
                          "under the given directory. Skips binary files, .git, and node_modules.",
                          GREP_PARAMS_JSON, tool_sandboxed_stub, (void *)ctx);
    if (rc == 0)
        tools_set_recipe(reg, "file_grep", (ToolRecipe){EXEC_SANDBOX, SBX_FILE, NULL});
    return rc;
}

/* Dispatch file tool by name on pre-extracted wire params. */
static char *dispatch_file(const RunToolParsed *q, FileReadCtx *ctx) {
    typedef char *(*run_fn)(const RunToolParsed *, FileReadCtx *);
    struct { const char *name; run_fn fn; } tools[] = {
        {"file_read",  file_read_run},
        {"file_write", file_write_run},
        {"file_edit",  file_edit_run},
        {"file_list",  file_list_run},
        {"file_find",  file_find_run},
        {"file_grep",  file_grep_run},
    };
    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++)
        if (strcmp(q->tool_name, tools[i].name) == 0)
            return tools[i].fn(q, ctx);
    return strdup("error: unknown file tool");
}

char *tool_file_tier_run(const RunToolParsed *q) {
    /* Sandbox is already applied on this process; the run fns never set one
     * up themselves. sb.sandbox carries "mount enforcement is active" so
     * open failures outside the granted mounts produce a grant hint. */
    FileReadCtx fctx = {0};
    fctx.workspace    = q->workspace;
    fctx.cwd_path     = q->cwd_path;
    fctx.sb.sandbox      = q->sandbox;
    fctx.sb.workspace_ro = q->workspace_ro;
    fctx.sb.mount_cwd    = q->mount_cwd;
    fctx.sb.read_paths   = q->read_paths;
    fctx.sb.read_path_count = q->read_count;
    fctx.sb.write_paths  = q->write_paths;
    fctx.sb.write_path_count = q->write_count;
    if (!fctx.workspace) return strdup("error: no workspace configured");
    char *r = dispatch_file(q, &fctx);
    return r ? r : strdup("");
}
