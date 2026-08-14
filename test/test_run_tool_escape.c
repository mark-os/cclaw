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
static int ns_available = 1;

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

/* Send a file-tier request via the real re-exec path.
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

/* Detect namespace availability */
static void test_ns_detect(void) {
    char *r = run_tool_request("file_read", "{\"path\":\"hello.txt\"}");
    assert(r);
    if (strstr(r, "namespace sandbox unavailable")) {
        ns_available = 0;
        printf("  SKIP (namespaces unavailable)\n");
    } else {
        assert(strcmp(r, "hello from workspace") == 0);
        printf("  PASS test_ns_detect\n");
    }
    free(r);
}

/* 3.1: Symlink escape — symlink to /etc/hostname must fail */
static void test_symlink_escape(void) {
    if (!ns_available) { printf("  SKIP test_symlink_escape\n"); return; }
    char *r = run_tool_request("file_read", "{\"path\":\"escape_link\"}");
    assert(r);
    /* Must get an error — symlink target is outside the mount set */
    assert(strstr(r, "error") != NULL || strstr(r, "cannot open") != NULL ||
           strstr(r, "No such file") != NULL);
    printf("  PASS test_symlink_escape\n");
    free(r);
}

/* 3.2: Path traversal — attempt to reach a path outside the mount set */
static void test_traversal_escape(void) {
    if (!ns_available) { printf("  SKIP test_traversal_escape\n"); return; }
    /* /home is not in sys_dirs, not bind-mounted — unreachable in sandbox */
    char *r = run_tool_request("file_read", "{\"path\":\"../../../../home\"}");
    assert(r);
    /* Must not succeed — /home doesn't exist in the sandbox */
    assert(strstr(r, "error") != NULL || strstr(r, "cannot open") != NULL ||
           strstr(r, "outside workspace") != NULL);
    printf("  PASS test_traversal_escape\n");
    free(r);
}

/* 3.2b: absolute path outside the mount set must fail */
static void test_absolute_escape(void) {
    if (!ns_available) { printf("  SKIP test_absolute_escape\n"); return; }
    char *r = run_tool_request("file_read", "{\"path\":\"/root/.bashrc\"}");
    assert(r);
    assert(strstr(r, "error") != NULL || strstr(r, "cannot open") != NULL);
    printf("  PASS test_absolute_escape\n");
    free(r);
}

/* 3.2c: write traversal outside the workspace must fail */
static void test_write_escape(void) {
    if (!ns_available) { printf("  SKIP test_write_escape\n"); return; }
    char *r = run_tool_request("file_write",
        "{\"path\":\"../../etc/cclaw_evil.txt\",\"content\":\"bad\"}");
    assert(r);
    /* /etc is read-only in the sandbox; the write must not succeed */
    assert(strstr(r, "error") != NULL || strstr(r, "cannot") != NULL);
    assert(access("/etc/cclaw_evil.txt", F_OK) != 0);
    printf("  PASS test_write_escape\n");
    free(r);
}

/* 3.3: /proc absence — /proc must not be accessible */
static void test_proc_absent(void) {
    if (!ns_available) { printf("  SKIP test_proc_absent\n"); return; }
    /* Try to list /proc */
    char *r = run_tool_request("file_list", "{\"path\":\"/proc\"}");
    assert(r);
    /* Must get error (no /proc in mount view) */
    assert(strstr(r, "error") != NULL || strstr(r, "cannot open") != NULL ||
           strstr(r, "No such file") != NULL || strstr(r, "outside workspace") != NULL);
    printf("  PASS test_proc_absent (file_list /proc)\n");
    free(r);

    /* Try to read /proc/1/cmdline */
    r = run_tool_request("file_read", "{\"path\":\"/proc/1/cmdline\"}");
    assert(r);
    assert(strstr(r, "error") != NULL || strstr(r, "cannot open") != NULL ||
           strstr(r, "outside workspace") != NULL);
    printf("  PASS test_proc_absent (file_read /proc/1/cmdline)\n");
    free(r);
}

int main(void) {
    TEST_INIT();
    printf("test_run_tool_escape:\n");
    setup();
    test_ns_detect();
    test_symlink_escape();
    test_traversal_escape();
    test_absolute_escape();
    test_write_escape();
    test_proc_absent();
    cleanup();
    printf("  ALL PASSED\n");
    return 0;
}
