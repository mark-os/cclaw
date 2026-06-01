#define _POSIX_C_SOURCE 200809L
#include "extension.h"
#include "log.h"
#include <dirent.h>
#include <mquickjs.h>
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

    char ext_dir[1024];
    int n = snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", workspace);
    if (n < 0 || (size_t)n >= sizeof(ext_dir)) return NULL;

    DIR *d = opendir(ext_dir);
    if (!d) return NULL;

    size_t cap = 8;
    char **paths = malloc(cap * sizeof(char *));
    if (!paths) { closedir(d); return NULL; }

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
            /* *.js files only */
            size_t len = strlen(ent->d_name);
            if (len > 3 && strcmp(ent->d_name + len - 3, ".js") == 0)
                to_add = full;
        } else if (S_ISDIR(st.st_mode)) {
            /* subdirs with index.js */
            snprintf(idx_path, sizeof(idx_path), "%s/index.js", full);
            if (stat(idx_path, &st) == 0 && S_ISREG(st.st_mode))
                to_add = idx_path;
        }

        if (to_add) {
            if (*count >= cap) {
                cap *= 2;
                char **tmp = realloc(paths, cap * sizeof(char *));
                if (!tmp) break;
                paths = tmp;
            }
            paths[*count] = strdup(to_add);
            if (paths[*count]) (*count)++;
        }
    }
    closedir(d);

    if (*count > 1)
        qsort(paths, *count, sizeof(char *), cmp_str);

    return paths;
}

void extension_list_free(char **paths, size_t count) {
    if (!paths) return;
    for (size_t i = 0; i < count; i++) free(paths[i]);
    free(paths);
}

/* Read file into malloc'd buffer. Returns NULL on failure. */
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

int extension_load(char **paths, size_t count, JsSessionRuntime *rt,
                   ToolRegistry *reg, const Config *cfg) {
    if (!paths || count == 0 || !rt || !rt->ctx) return 0;
    (void)reg; /* T256 will use this for cclaw API object registration */

    JSContext *ctx = (JSContext *)rt->ctx;
    int loaded = 0;

    for (size_t i = 0; i < count; i++) {
        size_t src_len = 0;
        char *src = read_file(paths[i], &src_len);
        if (!src) {
            LOG_INFO(cfg, "extension: skip %s (unreadable)", paths[i]);
            continue;
        }

        /* Wrap: (function(cclaw){ <source> })(globalThis.__cclaw_api || {}) */
        size_t wrap_len = src_len + 64;
        char *wrapped = malloc(wrap_len);
        if (!wrapped) { free(src); continue; }
        snprintf(wrapped, wrap_len,
                 "(function(cclaw){%s})(globalThis.__cclaw_api||{})", src);
        free(src);

        JSValue val = JS_Eval(ctx, wrapped, strlen(wrapped), paths[i], 0);
        if (JS_IsException(val)) {
            JSValue exc = JS_GetException(ctx);
            JSCStringBuf buf;
            const char *msg = JS_ToCString(ctx, exc, &buf);
            LOG_INFO(cfg, "extension: skip %s (error: %s)", paths[i],
                     msg ? msg : "unknown");
        } else {
            loaded++;
            LOG_DEBUG(cfg, "extension: loaded %s", paths[i]);
        }
        free(wrapped);
    }

    return loaded;
}
