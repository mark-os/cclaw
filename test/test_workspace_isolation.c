#define _DEFAULT_SOURCE
#include "run_tool.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* T50: per-agent workspace isolation (V1, V12)
 * Two agents with different workspaces cannot access each other's files.
 *
 * Isolation is enforced by the kernel namespace set up in the --run-tool
 * broker child, so these tests drive the REAL re-exec path (fork+execve of
 * build/cclaw), one workspace per request. When unprivileged user namespaces
 * are unavailable, tests are SKIPPED (following test_run_tool_escape.c). */

#define BINARY "./build/cclaw"

static char ws_a[256];
static char ws_b[256];
static int ns_available = 1;

static void setup(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    snprintf(ws_a, sizeof(ws_a), "/tmp/cclaw_ws_a_XXXXXX");
    snprintf(ws_b, sizeof(ws_b), "/tmp/cclaw_ws_b_XXXXXX");
    assert(mkdtemp(ws_a) != NULL);
    assert(mkdtemp(ws_b) != NULL);

    /* Agent A's file */
    char path[512];
    snprintf(path, sizeof(path), "%s/secret_a.txt", ws_a);
    FILE *f = fopen(path, "w");
    assert(f);
    fprintf(f, "agent_a_data");
    fclose(f);

    /* Agent B's file */
    snprintf(path, sizeof(path), "%s/secret_b.txt", ws_b);
    f = fopen(path, "w");
    assert(f);
    fprintf(f, "agent_b_data");
    fclose(f);
}

static void cleanup(void) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s", ws_a, ws_b);
    system(cmd);
}

/* Send a file-tier request through the real broker path for `workspace`.
 * Returns the tool result (malloc'd). */
static char *run_tool_request(const char *workspace, const char *tool_name,
                              const char *arguments) {
    size_t blob_len = 0;
    RunToolReq req = {
        .tier = RUNTOOL_TIER_FILE,
        .tool_name = tool_name, .arguments = arguments,
        .workspace = workspace, .sandbox = 1,
        .env_mode = 1, .nproc = 64, .as_mb = 512, .cpu_sec = 60,
    };
    char *blob = run_tool_serialize_request(&req, &blob_len);
    if (!blob) return strdup("error: serialize failed");

    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) {
        free(blob);
        return strdup("error: socketpair failed");
    }

    pid_t pid = fork();
    if (pid < 0) {
        free(blob); close(sp[0]); close(sp[1]);
        return strdup("error: fork failed");
    }
    if (pid == 0) {
        close(sp[0]);
        if (sp[1] != 3) { dup2(sp[1], 3); close(sp[1]); }
        char *const argv[] = {"cclaw", "--run-tool", NULL};
        char *const envp[] = {NULL};
        execve(BINARY, argv, envp);
        _exit(127);
    }

    close(sp[1]);
    size_t written = 0;
    while (written < blob_len) {
        ssize_t w = write(sp[0], blob + written, blob_len - written);
        if (w <= 0) break;
        written += (size_t)w;
    }
    free(blob);
    shutdown(sp[0], SHUT_WR);

    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    assert(buf);
    for (;;) {
        if (len + 1024 > cap) { cap *= 2; buf = realloc(buf, cap); assert(buf); }
        ssize_t n = read(sp[0], buf + len, cap - len);
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(sp[0]);
    buf[len] = '\0';

    int status;
    waitpid(pid, &status, 0);

    /* Strip the fd-3 frame: [4-byte meta_len (network order)][meta][result] */
    if (len < 4) { free(buf); return strdup("error: short frame"); }
    size_t meta_len = ((size_t)(unsigned char)buf[0] << 24) |
                      ((size_t)(unsigned char)buf[1] << 16) |
                      ((size_t)(unsigned char)buf[2] << 8) |
                       (size_t)(unsigned char)buf[3];
    if (4 + meta_len > len) { free(buf); return strdup("error: bad frame"); }
    char *result = strdup(buf + 4 + meta_len);
    free(buf);
    return result;
}

/* Detect if namespaces work by doing a trivial sandboxed file_read */
static void test_namespace_detect(void) {
    char *r = run_tool_request(ws_a, "file_read", "{\"path\":\"secret_a.txt\"}");
    assert(r != NULL);
    if (strstr(r, "namespace sandbox unavailable")) {
        ns_available = 0;
        printf("  SKIP (namespaces unavailable on this system)\n");
    } else {
        assert(strcmp(r, "agent_a_data") == 0);
        printf("  PASS test_namespace_detect\n");
    }
    free(r);
}

/* V1,V12: agent A cannot read agent B's file via absolute path */
static void test_read_cross_workspace_absolute(void) {
    if (!ns_available) { printf("  SKIP test_read_cross_workspace_absolute\n"); return; }
    char args[512];
    snprintf(args, sizeof(args), "{\"path\":\"%s/secret_b.txt\"}", ws_b);
    char *r = run_tool_request(ws_a, "file_read", args);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL || strstr(r, "cannot open") != NULL);
    free(r);
    printf("  PASS test_read_cross_workspace_absolute\n");
}

/* V1,V12: agent B cannot read agent A's file via absolute path */
static void test_read_cross_workspace_reverse(void) {
    if (!ns_available) { printf("  SKIP test_read_cross_workspace_reverse\n"); return; }
    char args[512];
    snprintf(args, sizeof(args), "{\"path\":\"%s/secret_a.txt\"}", ws_a);
    char *r = run_tool_request(ws_b, "file_read", args);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL || strstr(r, "cannot open") != NULL);
    free(r);
    printf("  PASS test_read_cross_workspace_reverse\n");
}

/* V1,V12: agent A cannot write into agent B's workspace */
static void test_write_cross_workspace(void) {
    if (!ns_available) { printf("  SKIP test_write_cross_workspace\n"); return; }
    char args[512];
    snprintf(args, sizeof(args), "{\"path\":\"%s/evil.txt\",\"content\":\"pwned\"}", ws_b);
    char *r = run_tool_request(ws_a, "file_write", args);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL || strstr(r, "cannot open") != NULL);
    free(r);

    char check[512];
    snprintf(check, sizeof(check), "%s/evil.txt", ws_b);
    assert(access(check, F_OK) != 0);
    printf("  PASS test_write_cross_workspace\n");
}

/* V1,V12: relative traversal from A into B blocked */
static void test_read_traversal_to_other_workspace(void) {
    if (!ns_available) { printf("  SKIP test_read_traversal_to_other_workspace\n"); return; }
    const char *b_name = strrchr(ws_b, '/');
    assert(b_name != NULL);
    char args[512];
    snprintf(args, sizeof(args), "{\"path\":\"..%s/secret_b.txt\"}", b_name);
    char *r = run_tool_request(ws_a, "file_read", args);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL || strstr(r, "cannot open") != NULL);
    free(r);
    printf("  PASS test_read_traversal_to_other_workspace\n");
}

/* V1,V12: write via relative traversal to other workspace blocked */
static void test_write_traversal_to_other_workspace(void) {
    if (!ns_available) { printf("  SKIP test_write_traversal_to_other_workspace\n"); return; }
    const char *b_name = strrchr(ws_b, '/');
    assert(b_name != NULL);
    char args[512];
    snprintf(args, sizeof(args), "{\"path\":\"..%s/evil.txt\",\"content\":\"pwned\"}", b_name);
    char *r = run_tool_request(ws_a, "file_write", args);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL || strstr(r, "cannot open") != NULL);
    free(r);
    printf("  PASS test_write_traversal_to_other_workspace\n");
}

/* V1: each agent CAN access its own workspace (positive case) */
static void test_read_own_workspace(void) {
    if (!ns_available) { printf("  SKIP test_read_own_workspace\n"); return; }
    char *r = run_tool_request(ws_a, "file_read", "{\"path\":\"secret_a.txt\"}");
    assert(r != NULL);
    assert(strcmp(r, "agent_a_data") == 0);
    free(r);

    r = run_tool_request(ws_b, "file_read", "{\"path\":\"secret_b.txt\"}");
    assert(r != NULL);
    assert(strcmp(r, "agent_b_data") == 0);
    free(r);
    printf("  PASS test_read_own_workspace\n");
}

/* V12: workspace fallback path pattern (./workspace/{agent_id}) */
static void test_workspace_fallback_path(void) {
    if (!ns_available) { printf("  SKIP test_workspace_fallback_path\n"); return; }

    char base[128];
    snprintf(base, sizeof(base), "/tmp/cclaw_ws_fb_XXXXXX");
    assert(mkdtemp(base) != NULL);

    char agent_ws[256];
    snprintf(agent_ws, sizeof(agent_ws), "%s/workspace/agent_1", base);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", agent_ws);
    system(cmd);

    char path[512];
    snprintf(path, sizeof(path), "%s/data.txt", agent_ws);
    FILE *f = fopen(path, "w");
    assert(f);
    fprintf(f, "agent1_only");
    fclose(f);

    char *r = run_tool_request(agent_ws, "file_read", "{\"path\":\"data.txt\"}");
    assert(r != NULL);
    assert(strcmp(r, "agent1_only") == 0);
    free(r);

    /* Agent cannot escape to parent via traversal — <base>/etc/passwd does
     * not exist in the sandbox (only the workspace leaf is bound) */
    r = run_tool_request(agent_ws, "file_read", "{\"path\":\"../../etc/passwd\"}");
    assert(r != NULL);
    assert(strstr(r, "error") != NULL || strstr(r, "cannot open") != NULL);
    free(r);

    snprintf(cmd, sizeof(cmd), "rm -rf %s", base);
    system(cmd);
    printf("  PASS test_workspace_fallback_path\n");
}

int main(void) {
    printf("test_workspace_isolation:\n");
    setup();
    test_namespace_detect();
    test_read_cross_workspace_absolute();
    test_read_cross_workspace_reverse();
    test_write_cross_workspace();
    test_read_traversal_to_other_workspace();
    test_write_traversal_to_other_workspace();
    test_read_own_workspace();
    test_workspace_fallback_path();
    cleanup();
    printf("All workspace isolation tests passed.\n");
    return 0;
}
