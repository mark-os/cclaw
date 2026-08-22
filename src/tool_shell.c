#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "tool_shell.h"
#include "sandbox.h"

/* %d = the live per-agent default timeout, rendered at registration time so
 * the schema states the truth instead of a hardcoded guess. */
static const char *SHELL_PARAMS_JSON_FMT =
    "{\"type\":\"object\",\"properties\":{"
    "\"command\":{\"type\":\"string\",\"description\":\"Shell command to execute\"},"
    "\"timeout\":{\"type\":\"integer\",\"description\":\"Timeout in seconds (default %d)\"},"
    "\"save_secret\":{\"type\":\"string\",\"description\":\"Capture a credential from this command's output: NAME (^[A-Z][A-Z0-9_]*$) stores it encrypted and masks it to {{SECRET:NAME}} — the raw value never enters context\"},"
    "\"save_secret_path\":{\"type\":\"string\",\"description\":\"With save_secret: JSON path (e.g. $.token) selecting the credential in JSON output; omit to capture the whole trimmed output\"}"
    "},\"required\":[\"command\"]}";

int tool_shell_register(ToolRegistry *reg, int default_timeout, const char *workspace,
                        const char *shell_path) {
    ShellConfig *sc = malloc(sizeof(ShellConfig));
    if (!sc) return -1;
    sc->timeout = (default_timeout > 0) ? default_timeout : TOOL_SHELL_DEFAULT_TIMEOUT;
    sc->shell_path = (shell_path && shell_path[0]) ? shell_path : NULL;
    sc->workspace = workspace;
    sc->cwd_path = NULL;
    sc->db_path = NULL;
    sc->allowed_hosts = NULL;
    sc->allowed_host_count = 0;
    sc->secrets = NULL;
    sc->secret_count = 0;
    sc->sb.sandbox = 1;
    char params[1024];
    snprintf(params, sizeof(params), SHELL_PARAMS_JSON_FMT, sc->timeout);
    int rc = tools_register(reg, "shell_exec",
                            "Execute a shell command and return stdout+stderr. "
                            "For file exploration prefer file_read/file_list/"
                            "file_find/file_grep — cheaper and sandbox-friendly.",
                            params, NULL, sc);
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

/* Collect CCLAW_SECRET_* env vars, clear from environment */
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

RunToolSecret *shell_filter_secrets(const char *cmd, const ShellSecret *all,
                                    size_t n, size_t *out_count) {
    *out_count = 0;
    if (!cmd || !cmd[0] || !all || n == 0) return NULL;
    RunToolSecret *min = malloc(n * sizeof(RunToolSecret));
    if (!min) return NULL;
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        /* Check if command references $CCLAW_SECRET_<name> */
        char tok[256];
        int tlen = snprintf(tok, sizeof(tok), "CCLAW_SECRET_%s", all[i].name);
        if (tlen <= 0 || tlen >= (int)sizeof(tok)) continue;
        int found = 0;
        for (const char *p = cmd; (p = strstr(p, tok)) != NULL; p += tlen) {
            /* Require a word boundary on BOTH sides: a bare substring of a
             * larger identifier (e.g. the env name MY_CCLAW_SECRET_FOO) is not
             * a reference to CCLAW_SECRET_FOO and must not pull that secret in. */
            char before = (p == cmd) ? '\0' : p[-1];
            char after = p[tlen];
            int before_ident = (before >= 'A' && before <= 'Z') ||
                                (before >= 'a' && before <= 'z') ||
                                (before >= '0' && before <= '9') || before == '_';
            int after_ident = (after >= 'A' && after <= 'Z') ||
                               (after >= 'a' && after <= 'z') ||
                               (after >= '0' && after <= '9') || after == '_';
            if (!before_ident && !after_ident) { found = 1; break; }
        }
        if (found) {
            min[count].name = all[i].name;
            min[count].value = all[i].value;
            min[count].hosts = NULL;   /* shell env injection: no binding gate here */
            count++;
        }
    }
    if (count == 0) { free(min); return NULL; }
    *out_count = count;
    return min;
}

void tool_shell_tier_exec(RunToolParsed *q) {
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
    /* The one inner exec — shell is the only tier with a foreign program.
     * command carries interpolated secrets; exec replaces the address space so
     * they don't persist after execl. shell_path is operator/agent config
     * (see agent_setup.c), not model input — the command string is what's
     * untrusted, not the interpreter running it. */
    const char *shell = (q->shell_path && q->shell_path[0]) ? q->shell_path : "/bin/sh";
    execl(shell, shell, "-c", q->command, (char *)NULL);
    _exit(127);
}
