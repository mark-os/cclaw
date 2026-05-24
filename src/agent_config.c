#define _POSIX_C_SOURCE 200809L
#include "agent_config.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stdio.h>

char **agent_discover(const char *agents_dir, size_t *count) {
    *count = 0;
    DIR *d = opendir(agents_dir);
    if (!d) return NULL;

    size_t cap = 8;
    char **names = malloc(cap * sizeof(char *));
    if (!names) { closedir(d); return NULL; }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        /* Check it's a directory */
        char path[1024];
        int n = snprintf(path, sizeof(path), "%s/%s", agents_dir, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) continue;

        struct stat st;
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        if (*count >= cap) {
            cap *= 2;
            char **tmp = realloc(names, cap * sizeof(char *));
            if (!tmp) break;
            names = tmp;
        }
        names[*count] = strdup(ent->d_name);
        if (names[*count]) (*count)++;
    }
    closedir(d);
    return names;
}

void agent_discover_free(char **names, size_t count) {
    if (!names) return;
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
}
