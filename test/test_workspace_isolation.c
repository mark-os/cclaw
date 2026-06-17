#define _POSIX_C_SOURCE 200809L
#include "tool_file.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* T50: per-agent workspace isolation (V1, V12)
 * Two agents with different workspaces cannot access each other's files.
 *
 * After the sandbox refactor, workspace isolation is enforced by the kernel
 * namespace (pivot_root). These tests drive the FORKED sandbox path so they
 * assert the namespace confines the escape. When unprivileged user namespaces
 * are unavailable, tests are SKIPPED (following test_shell_namespace.c). */

static char ws_a[256];
static char ws_b[256];
static FileReadCtx ctx_a;
static FileReadCtx ctx_b;
static int ns_available = 1;

static void setup(void) {
    snprintf(ws_a, sizeof(ws_a), "/tmp/cclaw_ws_a_XXXXXX");
    snprintf(ws_b, sizeof(ws_b), "/tmp/cclaw_ws_b_XXXXXX");
    assert(mkdtemp(ws_a) != NULL);
    assert(mkdtemp(ws_b) != NULL);

    memset(&ctx_a, 0, sizeof(ctx_a));
    ctx_a.workspace = ws_a;
    ctx_a.sandbox = 1;

    memset(&ctx_b, 0, sizeof(ctx_b));
    ctx_b.workspace = ws_b;
    ctx_b.sandbox = 1;

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

/* Detect if namespaces work by doing a trivial sandboxed file_read */
static void test_namespace_detect(void) {
    char *r = tool_file_read_handler("{\"path\":\"secret_a.txt\"}", (void *)&ctx_a);
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
    char *r = tool_file_read_handler(args, (void *)&ctx_a);
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
    char *r = tool_file_read_handler(args, (void *)&ctx_b);
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
    char *r = tool_file_write_handler(args, (void *)&ctx_a);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL || strstr(r, "cannot open") != NULL);
    free(r);
    printf("  PASS test_write_cross_workspace\n");
}

/* V1,V12: relative traversal from A into B blocked */
static void test_read_traversal_to_other_workspace(void) {
    if (!ns_available) { printf("  SKIP test_read_traversal_to_other_workspace\n"); return; }
    const char *b_name = strrchr(ws_b, '/');
    assert(b_name != NULL);
    char args[512];
    snprintf(args, sizeof(args), "{\"path\":\"..%s/secret_b.txt\"}", b_name);
    char *r = tool_file_read_handler(args, (void *)&ctx_a);
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
    char *r = tool_file_write_handler(args, (void *)&ctx_a);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL || strstr(r, "cannot open") != NULL);
    free(r);
    printf("  PASS test_write_traversal_to_other_workspace\n");
}

/* V1: each agent CAN access its own workspace (positive case) */
static void test_read_own_workspace(void) {
    if (!ns_available) { printf("  SKIP test_read_own_workspace\n"); return; }
    char *r = tool_file_read_handler("{\"path\":\"secret_a.txt\"}", (void *)&ctx_a);
    assert(r != NULL);
    assert(strcmp(r, "agent_a_data") == 0);
    free(r);

    r = tool_file_read_handler("{\"path\":\"secret_b.txt\"}", (void *)&ctx_b);
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

    FileReadCtx fb_ctx = {.workspace = agent_ws, .sandbox = 1};
    char *r = tool_file_read_handler("{\"path\":\"data.txt\"}", (void *)&fb_ctx);
    assert(r != NULL);
    assert(strcmp(r, "agent1_only") == 0);
    free(r);

    /* Agent cannot escape to parent via traversal */
    r = tool_file_read_handler("{\"path\":\"../../etc/passwd\"}", (void *)&fb_ctx);
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
