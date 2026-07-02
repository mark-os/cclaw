/* Extension support: draft discovery + DB-driven hook loading.
 *
 * The declarative model (specs/extensions.md): nothing registers by running.
 * Tools load from the DB join (tools_load_extension_tools); hooks load from the
 * `hooks` table here. No JS is evaluated at load time — a hook handler file is
 * read as text and stored as the function source, re-evaluated in a fresh
 * QuickJS context per dispatch. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "extension.h"
#include <sqlite3.h>
#include "log.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char **)a, *(const char **)b);
}

char **extension_discover(const char *workspace, size_t *count) {
    *count = 0;
    if (!workspace) return NULL;

    size_t cap = 8;
    char **paths = malloc(cap * sizeof(char *));
    if (!paths) return NULL;

    /* Scan workspace/extensions/ for local drafts (index.qjs or *.qjs) */
    char ext_dir[1024];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", workspace);

    DIR *d = opendir(ext_dir);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char full[2048];
            snprintf(full, sizeof(full), "%s/%s", ext_dir, ent->d_name);
            struct stat st;
            if (stat(full, &st) != 0) continue;

            const char *to_add = NULL;
            char idx_path[2080];
            if (S_ISREG(st.st_mode)) {
                size_t len = strlen(ent->d_name);
                if (len > 4 && strcmp(ent->d_name + len - 4, ".qjs") == 0)
                    to_add = full;
            } else if (S_ISDIR(st.st_mode)) {
                snprintf(idx_path, sizeof(idx_path), "%s/index.qjs", full);
                if (stat(idx_path, &st) == 0 && S_ISREG(st.st_mode))
                    to_add = idx_path;
            }
            if (to_add) {
                if (*count >= cap) { cap *= 2; paths = realloc(paths, cap * sizeof(char *)); }
                paths[*count] = strdup(to_add);
                if (paths[*count]) (*count)++;
            }
        }
        closedir(d);
    }

    if (*count > 1)
        qsort(paths, *count, sizeof(char *), cmp_str);
    return paths;
}

void extension_list_free(char **paths, size_t count) {
    if (!paths) return;
    for (size_t i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len <= 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_len) *out_len = rd;
    return buf;
}

int hook_event_from_name(const char *name) {
    if (!name) return -1;
    if (strcmp(name, "preAdvance") == 0) return HOOK_PRE_ADVANCE;
    if (strcmp(name, "postAdvance") == 0) return HOOK_POST_ADVANCE;
    if (strcmp(name, "beforeToolCall") == 0) return HOOK_BEFORE_TOOL_CALL;
    if (strcmp(name, "afterToolCall") == 0) return HOOK_AFTER_TOOL_CALL;
    if (strcmp(name, "turnStart") == 0) return HOOK_TURN_START;
    if (strcmp(name, "turnEnd") == 0) return HOOK_TURN_END;
    return -1;
}

void extension_ctx_init(ExtensionCtx *ctx, JsSessionRuntime *rt) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->rt = rt;
}

void extension_ctx_destroy(ExtensionCtx *ctx) {
    for (int i = 0; i < HOOK_EVENT_COUNT; i++) {
        for (size_t j = 0; j < ctx->hooks[i].count; j++)
            free(ctx->hooks[i].fns[j]);
        free(ctx->hooks[i].fns);
    }
}

int extension_load_hooks(ExtensionCtx *ctx, sqlite3 *db, const char *agent_name) {
    if (!ctx || !db || !agent_name) return 0;

    /* Hooks for this agent's attached, enabled, visible extensions. Each
     * handler file holds a function expression; it is stored as source text and
     * evaluated per dispatch (hook_dispatch.c) in a fresh context. */
    static const char *sql =
        "SELECT h.event, h.path FROM hooks h "
        "JOIN agent_extensions ae ON ae.extension_name = h.extension_name "
        "JOIN extensions e ON e.name = h.extension_name "
        "WHERE ae.agent_name = ?1 AND ae.enabled = 1 AND h.enabled = 1 "
        "  AND (e.published = 1 OR e.owner_agent = ?1) "
        "ORDER BY h.extension_name, h.event, h.path";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, agent_name, -1, SQLITE_STATIC);

    int loaded = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *event = (const char *)sqlite3_column_text(st, 0);
        const char *path = (const char *)sqlite3_column_text(st, 1);
        int ev = hook_event_from_name(event);
        if (ev < 0 || !path) continue;
        char *src = read_file(path, NULL);
        if (!src) continue;
        HookList *hl = &ctx->hooks[ev];
        char **tmp = realloc(hl->fns, (hl->count + 1) * sizeof(char *));
        if (!tmp) { free(src); continue; }
        hl->fns = tmp;
        hl->cap = hl->count + 1;
        hl->fns[hl->count++] = src;  /* heap-owned; freed by extension_ctx_destroy */
        loaded++;
    }
    sqlite3_finalize(st);
    return loaded;
}
