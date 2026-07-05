#define _POSIX_C_SOURCE 200809L
#include "skills.h"
#include "config_registry.h"
#include "buf.h"
#include "log.h"
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Frontmatter head cap: name(64) + description(1024) + slack. */
#define SKILL_FM_MAX 8192

static int str_ends_with(const char *s, const char *suffix) {
    size_t slen = strlen(s), suflen = strlen(suffix);
    return suflen <= slen && strcmp(s + slen - suflen, suffix) == 0;
}

/* Trim leading/trailing whitespace and one layer of matching quotes, in
 * place; returns the trimmed start. */
static char *trim_value(char *v) {
    while (*v == ' ' || *v == '\t') v++;
    size_t len = strlen(v);
    while (len > 0 && (v[len-1] == ' ' || v[len-1] == '\t' ||
                       v[len-1] == '\r' || v[len-1] == '\n'))
        v[--len] = '\0';
    if (len >= 2 && ((v[0] == '"' && v[len-1] == '"') ||
                     (v[0] == '\'' && v[len-1] == '\''))) {
        v[len-1] = '\0';
        v++;
    }
    return v;
}

int skill_frontmatter_parse(const char *path, char **name_out, char **desc_out) {
    *name_out = NULL;
    *desc_out = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char head[SKILL_FM_MAX + 1];
    size_t got = fread(head, 1, SKILL_FM_MAX, f);
    fclose(f);
    head[got] = '\0';

    /* Must open with a frontmatter fence. */
    if (strncmp(head, "---", 3) != 0) return -1;
    char *p = head + 3;
    if (*p == '\r') p++;
    if (*p != '\n') return -1;
    p++;

    /* Scan top-level `key: value` lines until the closing fence. Nested
     * mappings (e.g. metadata.openclaw.*) are indented and skipped. */
    while (p && *p) {
        char *eol = strchr(p, '\n');
        size_t linelen = eol ? (size_t)(eol - p) : strlen(p);
        if (strncmp(p, "---", 3) == 0 &&
            (linelen == 3 || p[3] == '\r')) break;
        if (*p != ' ' && *p != '\t' && *p != '#') {
            char line[2048];
            if (linelen < sizeof(line)) {
                memcpy(line, p, linelen);
                line[linelen] = '\0';
                char *colon = strchr(line, ':');
                if (colon) {
                    *colon = '\0';
                    char *val = trim_value(colon + 1);
                    if (strcmp(line, "name") == 0 && !*name_out && val[0])
                        *name_out = strdup(val);
                    else if (strcmp(line, "description") == 0 && !*desc_out && val[0])
                        *desc_out = strdup(val);
                }
            }
        }
        p = eol ? eol + 1 : NULL;
    }

    if (!*desc_out) {   /* description is the index — useless without it */
        free(*name_out);
        *name_out = NULL;
        return -1;
    }
    return 0;
}

/* Expand a leading "~/" against $HOME. Returns heap string. */
static char *expand_tilde(const char *path) {
    if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
        const char *home = getenv("HOME");
        if (home && home[0]) {
            size_t need = strlen(home) + strlen(path);
            char *out = malloc(need);
            if (out) snprintf(out, need, "%s%s", home, path + 1);
            return out;
        }
    }
    return strdup(path);
}

char **skills_dirs_resolve(sqlite3 *db, const char *agents_dir,
                           const char *agent, size_t *count) {
    *count = 0;
    size_t cap = 4, n = 0;
    char **dirs = calloc(cap, sizeof(char *));
    if (!dirs) return NULL;

    /* skills_dirs config key: JSON array of paths. json_each keeps parsing
     * out of C (the value is data we own). */
    char *list = config_get(db, "skills_dirs");
    if (list && list[0] && db) {
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db, "SELECT value FROM json_each(?1)",
                               -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, list, -1, SQLITE_STATIC);
            while (sqlite3_step(st) == SQLITE_ROW) {
                const char *d = (const char *)sqlite3_column_text(st, 0);
                if (!d || !d[0]) continue;
                char *full = expand_tilde(d);
                if (!full) continue;
                struct stat sb;
                if (stat(full, &sb) == 0 && S_ISDIR(sb.st_mode)) {
                    if (n == cap) {
                        cap *= 2;
                        char **tmp = realloc(dirs, cap * sizeof(char *));
                        if (!tmp) { free(full); break; }
                        dirs = tmp;
                    }
                    dirs[n++] = full;
                } else {
                    free(full);
                }
            }
            sqlite3_finalize(st);
        }
    }
    free(list);

    /* Per-agent dir last (lowest precedence loses name collisions to it?
     * No — first name wins, so config dirs shadow the per-agent dir). */
    if (agents_dir && agent) {
        char path[PATH_MAX];
        int pn = snprintf(path, sizeof(path), "%s/%s/skills", agents_dir, agent);
        struct stat sb;
        if (pn > 0 && (size_t)pn < sizeof(path) &&
            stat(path, &sb) == 0 && S_ISDIR(sb.st_mode)) {
            if (n == cap) {
                cap *= 2;
                char **tmp = realloc(dirs, cap * sizeof(char *));
                if (tmp) dirs = tmp;
            }
            if (n < cap) dirs[n++] = strdup(path);
        }
    }

    if (n == 0) { free(dirs); return NULL; }
    *count = n;
    return dirs;
}

void skills_dirs_free(char **dirs, size_t count) {
    if (!dirs) return;
    for (size_t i = 0; i < count; i++) free(dirs[i]);
    free(dirs);
}

typedef struct { Buf out; Buf names; int count; } SkillIndex;

static int name_seen(SkillIndex *ix, const char *name) {
    /* names buf holds "\x1fname\x1f" tokens — linear scan, few skills. */
    char tok[80];
    snprintf(tok, sizeof(tok), "\x1f%s\x1f", name);
    return ix->names.len > 0 && ix->names.data &&
           strstr(ix->names.data, tok) != NULL;
}

static void index_add(SkillIndex *ix, const char *name, const char *desc,
                      const char *location) {
    if (name_seen(ix, name)) return;
    buf_appendf(&ix->names, "\x1f%s\x1f", name);
    buf_appendf(&ix->out,
                "  <skill>\n"
                "    <name>%s</name>\n"
                "    <description>%s</description>\n"
                "    <location>%s</location>\n"
                "  </skill>\n",
                name, desc, location);
    ix->count++;
}

/* One candidate SKILL.md (or bare .md): parse, apply the name fallback,
 * add to the index. */
static void index_try_file(SkillIndex *ix, const char *path,
                           const char *fallback_name) {
    char *name = NULL, *desc = NULL;
    if (skill_frontmatter_parse(path, &name, &desc) != 0) {
        LOG_DEBUG_("skill skipped (no frontmatter description) path=%s", path);
        return;
    }
    index_add(ix, name ? name : fallback_name, desc, path);
    free(name);
    free(desc);
}

/* Read at most `cap` bytes of a file into a NUL-terminated heap buffer. */
static char *read_head(const char *path, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc(cap + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, cap, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static const char *path_basename(const char *p) {
    const char *sl = strrchr(p, '/');
    return sl ? sl + 1 : p;
}

/* Skills declared by the agent's attached extensions (manifest $.skills[],
 * bundle-relative paths). Read from the shared-store copy of extension.json —
 * the DB stores no skill state; the store is the source of truth. Runs after
 * the dir scan, so operator/agent skills shadow extension skills. */
static void index_extension_skills(SkillIndex *ix, sqlite3 *db, const char *agent) {
    if (!db || !agent) return;
    /* Collect store paths first — keep only one statement active at a time. */
    size_t cap = 4, n = 0;
    char **paths = calloc(cap, sizeof(char *));
    if (!paths) return;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT e.path FROM extensions e "
            "JOIN agent_extensions ae ON ae.extension_name = e.name "
            "WHERE ae.agent_name=?1 AND ae.enabled=1 AND e.enabled=1 "
            "AND (e.published=1 OR e.owner_agent=?1)",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, agent, -1, SQLITE_STATIC);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *p = (const char *)sqlite3_column_text(st, 0);
            if (!p || !p[0]) continue;
            if (n == cap) {
                cap *= 2;
                char **tmp = realloc(paths, cap * sizeof(char *));
                if (!tmp) break;
                paths = tmp;
            }
            paths[n++] = strdup(p);
        }
        sqlite3_finalize(st);
    }

    for (size_t i = 0; i < n; i++) {
        char mpath[PATH_MAX];
        int pn = snprintf(mpath, sizeof(mpath), "%s/extension.json", paths[i]);
        if (pn < 0 || (size_t)pn >= sizeof(mpath)) continue;
        char *manifest = read_head(mpath, 65536);
        if (!manifest) continue;
        sqlite3_stmt *se;
        if (sqlite3_prepare_v2(db,
                "SELECT value FROM json_each(COALESCE(json_extract(?1,'$.skills'),'[]'))",
                -1, &se, NULL) == SQLITE_OK) {
            sqlite3_bind_text(se, 1, manifest, -1, SQLITE_STATIC);
            while (sqlite3_step(se) == SQLITE_ROW) {
                const char *s = (const char *)sqlite3_column_text(se, 0);
                if (!s || !s[0] || s[0] == '/' || strstr(s, "..")) continue;
                char full[PATH_MAX];
                pn = snprintf(full, sizeof(full), "%s/%s", paths[i], s);
                if (pn < 0 || (size_t)pn >= sizeof(full)) continue;
                struct stat sb;
                if (stat(full, &sb) != 0) continue;
                char fallback[256];
                if (S_ISDIR(sb.st_mode)) {
                    snprintf(fallback, sizeof(fallback), "%s", path_basename(full));
                    size_t l = strlen(full);
                    if (l + 10 >= sizeof(full)) continue;
                    snprintf(full + l, sizeof(full) - l, "/SKILL.md");
                } else {
                    const char *bn = path_basename(full);
                    size_t bl = strlen(bn);
                    if (bl > 3 && strcmp(bn + bl - 3, ".md") == 0) bl -= 3;
                    snprintf(fallback, sizeof(fallback), "%.*s", (int)bl, bn);
                }
                index_try_file(ix, full, fallback);
            }
            sqlite3_finalize(se);
        }
        free(manifest);
    }
    for (size_t i = 0; i < n; i++) free(paths[i]);
    free(paths);
}

char *skills_index_build(sqlite3 *db, const char *agents_dir, const char *agent) {
    size_t dir_count = 0;
    char **dirs = skills_dirs_resolve(db, agents_dir, agent, &dir_count);

    SkillIndex ix = {0};
    for (size_t i = 0; i < dir_count; i++) {
        DIR *d = opendir(dirs[i]);
        if (!d) continue;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char path[PATH_MAX];
            int pn = snprintf(path, sizeof(path), "%s/%s", dirs[i], ent->d_name);
            if (pn < 0 || (size_t)pn >= sizeof(path)) continue;
            struct stat sb;
            if (stat(path, &sb) != 0) continue;
            if (S_ISDIR(sb.st_mode)) {
                char smd[PATH_MAX];
                int sn = snprintf(smd, sizeof(smd), "%s/SKILL.md", path);
                if (sn > 0 && (size_t)sn < sizeof(smd) && stat(smd, &sb) == 0)
                    index_try_file(&ix, smd, ent->d_name);
            } else if (str_ends_with(ent->d_name, ".md")) {
                /* bare .md skill: fallback name = filename minus .md */
                char fname[256];
                snprintf(fname, sizeof(fname), "%.*s",
                         (int)(strlen(ent->d_name) - 3), ent->d_name);
                index_try_file(&ix, path, fname);
            }
        }
        closedir(d);
    }
    skills_dirs_free(dirs, dir_count);
    index_extension_skills(&ix, db, agent);
    buf_free(&ix.names);

    if (ix.count == 0) { buf_free(&ix.out); return NULL; }

    Buf final = {0};
    buf_appendf(&final,
        "\n\n## Skills\n"
        "The following skills are available. When a task matches a skill's "
        "description, use file_read on its location to load the full "
        "instructions before proceeding.\n"
        "<available_skills>\n");
    buf_append(&final, ix.out.data, ix.out.len);
    buf_appendf(&final, "</available_skills>\n");
    buf_free(&ix.out);
    return buf_take(&final);
}
