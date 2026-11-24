#define _DEFAULT_SOURCE
#include "run_tool.h"
#include "db.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_util.h"

/* Test the --run-tool re-exec path for escape resistance.
 * Exercises the REAL fork+execve path (not the in-process fallback).
 * Requires unprivileged user namespaces; skips if unavailable. */

#define BINARY "./build/cclaw"

static char workspace[256];

static void setup(void) {
    snprintf(workspace, sizeof(workspace), "/tmp/cclaw_escape_%d", getpid());
    mkdir(workspace, 0755);

    /* Create a file in the workspace */
    char path[512];
    snprintf(path, sizeof(path), "%s/hello.txt", workspace);
    FILE *f = fopen(path, "w");
    assert(f);
    fprintf(f, "hello from workspace");
    fclose(f);

    /* Create a canary file outside the mount set (host /tmp is NOT the sandbox
     * /tmp — the sandbox gets a fresh tmpfs). Use a path under /var which is
     * never in sys_dirs. */
    snprintf(path, sizeof(path), "/tmp/cclaw_canary_%d", getpid());
    f = fopen(path, "w");
    assert(f);
    fprintf(f, "HOST_CANARY");
    fclose(f);

    /* Create symlink pointing to the canary (outside mount set) */
    char link[512];
    snprintf(link, sizeof(link), "%s/escape_link", workspace);
    symlink(path, link);
}

static void cleanup(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s /tmp/cclaw_canary_%d", workspace, getpid());
    system(cmd);
}

/* Send a file request via the real re-exec path.
 * Returns the tool result (malloc'd) or NULL on spawn failure. */
static char *run_tool_request(const char *tool_name, const char *arguments) {
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
        .vehicle = EXEC_FILE,
        .tool_name = tool_name,
        .params = wa, .param_count = wn,
        .workspace = workspace, .plan = TEST_PLAN_OFFLINE,
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

    /* Parent: write request, read result */
    close(sp[1]);
    size_t written = 0;
    while (written < blob_len) {
        ssize_t w = write(sp[0], blob + written, blob_len - written);
        if (w <= 0) break;
        written += (size_t)w;
    }
    free(blob);

    /* Shutdown write direction so child sees EOF after request */
    shutdown(sp[0], SHUT_WR);

    /* Read result */
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

/* The refusal text varies with where the sandbox stops the call (mount view,
 * open(), path check); any of these means the read did not reach the target. */
static int refused(const char *r) {
    return strstr(r, "error") || strstr(r, "cannot open") ||
           strstr(r, "No such file") || strstr(r, "outside workspace");
}

static void expect_refused(const char *tool, const char *args) {
    char *r = run_tool_request(tool, args);
    assert(r);
    if (!refused(r)) { printf("    not refused: %s %s -> %s\n", tool, args, r); assert(0); }
    free(r);
}

/* Positive control: a sandboxed read of the workspace works, so the
 * refusals below are the sandbox and not a broken spawn. */
static int ns_detect(void) {
    char *r = run_tool_request("file_read", "{\"path\":\"hello.txt\"}");
    assert(r);
    int ok = strstr(r, "namespace sandbox unavailable") == NULL;
    if (ok) assert(strcmp(r, "hello from workspace") == 0);
    free(r);
    return ok;
}

/* Symlink in the workspace pointing at a host file outside the mount set */
static void test_symlink_escape(void) {
    expect_refused("file_read", "{\"path\":\"escape_link\"}");
}

/* Relative traversal to /home, which is not mounted */
static void test_traversal_escape(void) {
    expect_refused("file_read", "{\"path\":\"../../../../home\"}");
}

static void test_absolute_escape(void) {
    expect_refused("file_read", "{\"path\":\"/root/.bashrc\"}");
}

/* Write traversal into /etc: read-only in the sandbox, and must not land */
static void test_write_escape(void) {
    char *r = run_tool_request("file_write",
        "{\"path\":\"../../etc/cclaw_evil.txt\",\"content\":\"bad\"}");
    assert(r);
    assert(strstr(r, "error") != NULL || strstr(r, "cannot") != NULL);
    assert(access("/etc/cclaw_evil.txt", F_OK) != 0);
    free(r);
}

/* No /proc in the mount view */
static void test_proc_absent(void) {
    expect_refused("file_list", "{\"path\":\"/proc\"}");
    expect_refused("file_read", "{\"path\":\"/proc/1/cmdline\"}");
}

int main(void) {
    TEST_INIT();
    printf("test_run_tool_escape:\n");
    setup();
    if (!ns_detect()) {
        printf("  SKIP (namespaces unavailable)\n");
    } else {
        RUN(test_symlink_escape);
        RUN(test_traversal_escape);
        RUN(test_absolute_escape);
        RUN(test_write_escape);
        RUN(test_proc_absent);
    }
    cleanup();
    return 0;
}
