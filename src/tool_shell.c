#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "tool_shell.h"
#include "sandbox.h"

static const char *SHELL_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"command\":{\"type\":\"string\",\"description\":\"Shell command to execute\"},"
    "\"timeout\":{\"type\":\"integer\",\"description\":\"Timeout in seconds (default 30)\"}"
    "},\"required\":[\"command\"]}";

int tool_shell_register(ToolRegistry *reg, int default_timeout, const char *workspace) {
    ShellConfig *sc = malloc(sizeof(ShellConfig));
    if (!sc) return -1;
    sc->timeout = (default_timeout > 0) ? default_timeout : TOOL_SHELL_DEFAULT_TIMEOUT;
    sc->workspace = workspace;
    sc->cwd_path = NULL;
    sc->db_path = NULL;
    sc->allowed_hosts = NULL;
    sc->allowed_host_count = 0;
    sc->secrets = NULL;
    sc->secret_count = 0;
    sc->sb.sandbox = 1;
    int rc = tools_register(reg, "shell_exec",
                            "Execute a shell command and return stdout+stderr",
                            SHELL_PARAMS_JSON, NULL, sc);
    if (rc == 0) {
        ToolEntry *e = tools_lookup(reg, "shell_exec");
        if (e) {
            e->free_fn = free;
            e->recipe = (ToolRecipe){EXEC_SANDBOX, SBX_SHELL, NULL};
        }
    } else {
        free(sc);
    }
    return rc;
}

/* V88: Collect CCLAW_SECRET_* env vars, clear from environment */
ShellSecret *shell_secrets_collect(size_t *count) {
    extern char **environ;
    *count = 0;

    /* Count matching vars */
    size_t n = 0;
    for (int i = 0; environ[i]; i++) {
        if (strncmp(environ[i], "CCLAW_SECRET_", 13) == 0)
            n++;
    }
    if (n == 0) return NULL;

    ShellSecret *secrets = calloc(n, sizeof(ShellSecret));
    if (!secrets) return NULL;

    size_t idx = 0;
    for (int i = 0; environ[i] && idx < n; ) {
        if (strncmp(environ[i], "CCLAW_SECRET_", 13) == 0) {
            char *eq = strchr(environ[i], '=');
            if (eq) {
                size_t namelen = (size_t)(eq - environ[i]) - 13;
                secrets[idx].name = strndup(environ[i] + 13, namelen);
                secrets[idx].value = strdup(eq + 1);
                idx++;
                /* Unset from env (modifies environ, don't increment i) */
                char key[256];
                size_t klen = (size_t)(eq - environ[i]);
                if (klen < sizeof(key)) {
                    memcpy(key, environ[i], klen);
                    key[klen] = '\0';
                    unsetenv(key);
                } else {
                    i++;
                }
            } else {
                i++;
            }
        } else {
            i++;
        }
    }
    *count = idx;
    return secrets;
}

void shell_secrets_free(ShellSecret *secrets, size_t count) {
    if (!secrets) return;
    for (size_t i = 0; i < count; i++) {
        free(secrets[i].name);
        if (secrets[i].value) {
            explicit_bzero(secrets[i].value, strlen(secrets[i].value));
            free(secrets[i].value);
        }
    }
    free(secrets);
}
