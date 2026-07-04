#define _GNU_SOURCE
#include "secret_store.h"
#include "db.h"
#include <stdlib.h>
#include <string.h>

ShellSecret *secrets_snapshot(sqlite3 *db, const ShellSecret *env_base,
                              size_t env_count, size_t *out_count) {
    *out_count = 0;
    size_t db_n = 0;
    ShellSecret *db_secrets = db ? db_secrets_load(db, &db_n) : NULL;

    size_t cap = env_count + db_n;
    if (cap == 0) { free(db_secrets); return NULL; }
    ShellSecret *out = calloc(cap, sizeof(ShellSecret));
    if (!out) {
        shell_secrets_free(db_secrets, db_n);
        return NULL;
    }

    size_t k = 0;
    for (size_t i = 0; i < env_count; i++) {
        out[k].name = strdup(env_base[i].name);
        out[k].value = strdup(env_base[i].value);
        k++;
    }
    for (size_t i = 0; i < db_n; i++) {
        int dup = 0;
        for (size_t j = 0; j < env_count; j++)
            if (strcmp(db_secrets[i].name, env_base[j].name) == 0) { dup = 1; break; }
        if (dup) {
            free(db_secrets[i].name);
            if (db_secrets[i].value) {
                explicit_bzero(db_secrets[i].value, strlen(db_secrets[i].value));
                free(db_secrets[i].value);
            }
            continue;
        }
        out[k++] = db_secrets[i];   /* transfer ownership */
    }
    free(db_secrets);   /* shell array only; items moved or freed above */

    *out_count = k;
    return out;
}

void secrets_snapshot_free(ShellSecret *secrets, size_t count) {
    shell_secrets_free(secrets, count);
}
