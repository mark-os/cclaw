#define _DEFAULT_SOURCE
#include "run_tool.h"
#include "db.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_util.h"

/* per-agent workspace isolation
 * Two agents with different workspaces cannot access each other's files.
 *
 * Isolation is enforced by the kernel namespace set up in the --run-tool
 * broker child, so these tests drive the REAL re-exec path (fork+execve of
 * build/cclaw), one workspace per request. When unprivileged user namespaces
 * are unavailable, tests are SKIPPED (following test_run_tool_escape.c). */

#define BINARY "./build/cclaw"

static char ws_a[256];
static char ws_b[256];

static void setup(void) {
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
    /* Extract wire params exactly as the dispatching parent does. */
    sqlite3 *mdb = db_open(":memory:");
    ToolWireArg *wa = NULL;
    size_t wn = 0;
    if (!mdb || tool_args_extract(mdb, tool_name, NULL, arguments, &wa, &wn) != 0) {
        if (mdb) sqlite3_close(mdb);
        return strdup("error: extract failed");
    }
    sqlite3_close(mdb);

    size_t blob_len = 0;
    RunToolReq req = {
        .tier = RUNTOOL_TIER_FILE,
        .tool_name = tool_name,
        .params = wa, .param_count = wn,
        .workspace = workspace, .sandbox = 1,
        .env_mode = 1, .nproc = 64, .as_mb = 512, .cpu_sec = 60,
    };
    char *blob = run_tool_serialize_request(&req, &blob_len);
    tool_wire_args_free(wa, wn);
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

    /* Strip the fd-3 frame: [status][4-byte meta_len (network order)][meta][result] */
    if (len < 5) { free(buf); return strdup("error: short frame"); }
    size_t meta_len = ((size_t)(unsigned char)buf[1] << 24) |
                      ((size_t)(unsigned char)buf[2] << 16) |
                      ((size_t)(unsigned char)buf[3] << 8) |
                       (size_t)(unsigned char)buf[4];
    if (5 + meta_len > len) { free(buf); return strdup("error: bad frame"); }
    char *result = strdup(buf + 5 + meta_len);
    free(buf);
    return result;
}

static void expect_refused(const char *ws, const char *tool, const char *args) {
    char *r = run_tool_request(ws, tool, args);
    assert(r != NULL);
    if (!strstr(r, "error") && !strstr(r, "cannot open")) {
        printf("    not refused: %s %s -> %s\n", tool, args, r);
        assert(0);
    }
    free(r);
}

/* Positive control: agent A reads its own workspace through the sandbox. */
static int namespace_detect(void) {
    char *r = run_tool_request(ws_a, "file_read", "{\"path\":\"secret_a.txt\"}");
    assert(r != NULL);
    int ok = strstr(r, "namespace sandbox unavailable") == NULL;
    if (ok) assert(strcmp(r, "agent_a_data") == 0);
    free(r);
    return ok;
}

static void test_read_cross_workspace_absolute(void) {
    char args[512];
    snprintf(args, sizeof(args), "{\"path\":\"%s/secret_b.txt\"}", ws_b);
    expect_refused(ws_a, "file_read", args);
}

static void test_write_cross_workspace(void) {
    char args[512];
    snprintf(args, sizeof(args), "{\"path\":\"%s/evil.txt\",\"content\":\"pwned\"}", ws_b);
    expect_refused(ws_a, "file_write", args);
    char check[512];
    snprintf(check, sizeof(check), "%s/evil.txt", ws_b);
    assert(access(check, F_OK) != 0);
}

static void test_read_traversal_to_other_workspace(void) {
    const char *b_name = strrchr(ws_b, '/');
    assert(b_name != NULL);
    char args[512];
    snprintf(args, sizeof(args), "{\"path\":\"..%s/secret_b.txt\"}", b_name);
    expect_refused(ws_a, "file_read", args);
}

static void test_write_traversal_to_other_workspace(void) {
    const char *b_name = strrchr(ws_b, '/');
    assert(b_name != NULL);
    char args[512];
    snprintf(args, sizeof(args), "{\"path\":\"..%s/evil.txt\",\"content\":\"pwned\"}", b_name);
    expect_refused(ws_a, "file_write", args);
}

int main(void) {
    TEST_INIT();
    printf("test_workspace_isolation:\n");
    setup();
    if (!namespace_detect()) {
        printf("  SKIP (namespaces unavailable on this system)\n");
    } else {
        RUN(test_read_cross_workspace_absolute);
        RUN(test_write_cross_workspace);
        RUN(test_read_traversal_to_other_workspace);
        RUN(test_write_traversal_to_other_workspace);
    }
    cleanup();
    return 0;
}
