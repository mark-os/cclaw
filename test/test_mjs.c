/* Test: mjs standalone binary (V48, V49, V51) */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <arpa/inet.h>

#define MJS_PATH "./build/mjs"

static char *run_mjs(const char *args)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "%s %s 2>&1", MJS_PATH, args);
    FILE *f = popen(cmd, "r");
    if (!f) return NULL;
    char *buf = (char *)malloc(4096);
    size_t n = fread(buf, 1, 4095, f);
    buf[n] = '\0';
    pclose(f);
    return buf;
}

static void test_basic_eval(void)
{
    char *r = run_mjs("-e '1 + 2'");
    assert(r && strstr(r, "3"));
    free(r);
    printf("  PASS test_basic_eval\n");
}

static void test_string_eval(void)
{
    char *r = run_mjs("-e '\"hello\"'");
    assert(r && strstr(r, "hello"));
    free(r);
    printf("  PASS test_string_eval\n");
}

static void test_file_eval(void)
{
    FILE *f = fopen("/tmp/mjs_test.js", "w");
    fprintf(f, "var a = [1,2,3]; a.length");
    fclose(f);
    char *r = run_mjs("/tmp/mjs_test.js");
    assert(r && strstr(r, "3"));
    free(r);
    unlink("/tmp/mjs_test.js");
    printf("  PASS test_file_eval\n");
}

static void test_no_args_shows_usage(void)
{
    char *r = run_mjs("");
    assert(r && strstr(r, "Usage:"));
    free(r);
    printf("  PASS test_no_args_shows_usage\n");
}

static void test_fetch_no_fd_errors(void)
{
    char *r = run_mjs("-e 'http_fetch(\"http://x.com\")'");
    assert(r && strstr(r, "no proxy fd available"));
    free(r);
    printf("  PASS test_fetch_no_fd_errors\n");
}

static void test_fetch_via_fd(void)
{
    /* V49/V50: create socketpair, fork child with MJS_FETCH_FD set,
     * parent acts as fetch proxy returning canned response */
    int sv[2];
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(rc == 0);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        /* Child: run mjs with fetch fd */
        close(sv[0]);
        /* Clear CLOEXEC so fd survives exec */
        int flags = fcntl(sv[1], F_GETFD);
        fcntl(sv[1], F_SETFD, flags & ~FD_CLOEXEC);
        char fd_str[16];
        snprintf(fd_str, sizeof(fd_str), "%d", sv[1]);
        setenv("MJS_FETCH_FD", fd_str, 1);
        /* Exec mjs: fetch and print body */
        execlp(MJS_PATH, "mjs", "-e",
               "var r = http_fetch('http://test.local/data'); r.body",
               (char *)NULL);
        _exit(127);
    }

    /* Parent: proxy */
    close(sv[1]);

    /* Read request: 4B len + JSON */
    uint32_t req_len_net;
    ssize_t n = read(sv[0], &req_len_net, 4);
    assert(n == 4);
    uint32_t req_len = ntohl(req_len_net);
    assert(req_len > 0 && req_len < 8192);

    char req_buf[8192];
    size_t total = 0;
    while (total < req_len) {
        ssize_t r = read(sv[0], req_buf + total, req_len - total);
        assert(r > 0);
        total += (size_t)r;
    }
    req_buf[req_len] = '\0';

    /* Verify request contains the URL */
    assert(strstr(req_buf, "http://test.local/data"));

    /* Send canned response */
    const char *resp = "{\"status\":200,\"headers\":{},\"body\":\"proxy_works\",\"error\":null}";
    uint32_t resp_len = (uint32_t)strlen(resp);
    uint32_t resp_len_net = htonl(resp_len);
    write(sv[0], &resp_len_net, 4);
    write(sv[0], resp, resp_len);
    close(sv[0]);

    /* Wait for child */
    int status;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    printf("  PASS test_fetch_via_fd\n");
}

static void test_pipe_composability(void)
{
    /* V51: mjs output flows through pipes normally */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "%s -e '\"line1\\nline2\\nline3\"' | grep line2", MJS_PATH);
    FILE *f = popen(cmd, "r");
    assert(f);
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    int rc = pclose(f);
    assert(rc == 0);
    assert(strstr(buf, "line2"));
    assert(!strstr(buf, "line1"));
    printf("  PASS test_pipe_composability\n");
}

static void test_fetch_with_pipeline(void)
{
    /* T136/V51: fetch fd (side-channel) does NOT interfere with stdout piping.
     * mjs does a fetch AND prints to stdout, piped through grep. */
    int sv[2];
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(rc == 0);

    /* Use pipe to capture final output after grep */
    int outpipe[2];
    rc = pipe(outpipe);
    assert(rc == 0);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        close(sv[0]);
        close(outpipe[0]);
        int flags = fcntl(sv[1], F_GETFD);
        fcntl(sv[1], F_SETFD, flags & ~FD_CLOEXEC);
        char fd_str[16];
        snprintf(fd_str, sizeof(fd_str), "%d", sv[1]);
        setenv("MJS_FETCH_FD", fd_str, 1);
        /* Redirect stdout to outpipe for capture */
        dup2(outpipe[1], STDOUT_FILENO);
        close(outpipe[1]);
        /* mjs fetches, then prints multiple lines; pipe through grep */
        execlp("/bin/sh", "sh", "-c",
               MJS_PATH " -e '"
               "var r = http_fetch(\"http://pipe.test/data\"); "
               "\"BEFORE\\n\" + r.body + \"\\nAFTER\""
               "' | grep fetched",
               (char *)NULL);
        _exit(127);
    }

    close(sv[1]);
    close(outpipe[1]);

    /* Parent: act as fetch proxy */
    uint32_t req_len_net;
    ssize_t n = read(sv[0], &req_len_net, 4);
    assert(n == 4);
    uint32_t req_len = ntohl(req_len_net);
    char req_buf[8192];
    size_t total = 0;
    while (total < req_len) {
        ssize_t r = read(sv[0], req_buf + total, req_len - total);
        assert(r > 0);
        total += (size_t)r;
    }
    assert(strstr(req_buf, "http://pipe.test/data"));

    /* Respond with body containing "fetched" */
    const char *resp = "{\"status\":200,\"headers\":{},\"body\":\"data_fetched_ok\",\"error\":null}";
    uint32_t resp_len = (uint32_t)strlen(resp);
    uint32_t resp_len_net = htonl(resp_len);
    write(sv[0], &resp_len_net, 4);
    write(sv[0], resp, resp_len);
    close(sv[0]);

    /* Read grep output */
    char buf[4096];
    size_t out_total = 0;
    while (out_total < sizeof(buf) - 1) {
        ssize_t r = read(outpipe[0], buf + out_total, sizeof(buf) - 1 - out_total);
        if (r <= 0) break;
        out_total += (size_t)r;
    }
    buf[out_total] = '\0';
    close(outpipe[0]);

    int status;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    /* grep should have matched the line containing "fetched" */
    assert(strstr(buf, "fetched"));
    /* "BEFORE" and "AFTER" should NOT appear (grep filtered them) */
    assert(!strstr(buf, "BEFORE"));
    assert(!strstr(buf, "AFTER"));

    printf("  PASS test_fetch_with_pipeline\n");
}

static void test_fetch_alias(void)
{
    /* T133: fetch() is an alias for http_fetch() */
    int sv[2];
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(rc == 0);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        close(sv[0]);
        int flags = fcntl(sv[1], F_GETFD);
        fcntl(sv[1], F_SETFD, flags & ~FD_CLOEXEC);
        char fd_str[16];
        snprintf(fd_str, sizeof(fd_str), "%d", sv[1]);
        setenv("MJS_FETCH_FD", fd_str, 1);
        execlp(MJS_PATH, "mjs", "-e",
               "var r = fetch('http://alias.test/ok'); r.body",
               (char *)NULL);
        _exit(127);
    }

    close(sv[1]);

    /* Read request */
    uint32_t req_len_net;
    assert(read(sv[0], &req_len_net, 4) == 4);
    uint32_t req_len = ntohl(req_len_net);
    char req_buf[8192];
    size_t total = 0;
    while (total < req_len) {
        ssize_t r = read(sv[0], req_buf + total, req_len - total);
        assert(r > 0);
        total += (size_t)r;
    }
    req_buf[req_len] = '\0';
    assert(strstr(req_buf, "http://alias.test/ok"));

    /* Send response */
    const char *resp = "{\"status\":200,\"headers\":{},\"body\":\"alias_ok\",\"error\":null}";
    uint32_t resp_len = (uint32_t)strlen(resp);
    uint32_t resp_len_net = htonl(resp_len);
    write(sv[0], &resp_len_net, 4);
    write(sv[0], resp, resp_len);
    close(sv[0]);

    int status;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    printf("  PASS test_fetch_alias\n");
}

static void test_fetch_error_response(void)
{
    /* T133: proxy returns error JSON — fetch returns it gracefully */
    int sv[2];
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(rc == 0);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        close(sv[0]);
        int flags = fcntl(sv[1], F_GETFD);
        fcntl(sv[1], F_SETFD, flags & ~FD_CLOEXEC);
        char fd_str[16];
        snprintf(fd_str, sizeof(fd_str), "%d", sv[1]);
        setenv("MJS_FETCH_FD", fd_str, 1);
        /* fetch returns error object — check .error field */
        execlp(MJS_PATH, "mjs", "-e",
               "var r = fetch('http://denied.host/x'); r.error",
               (char *)NULL);
        _exit(127);
    }

    close(sv[1]);

    /* Read request */
    uint32_t req_len_net;
    assert(read(sv[0], &req_len_net, 4) == 4);
    uint32_t req_len = ntohl(req_len_net);
    char req_buf[8192];
    size_t total = 0;
    while (total < req_len) {
        ssize_t r = read(sv[0], req_buf + total, req_len - total);
        assert(r > 0);
        total += (size_t)r;
    }

    /* V50: respond with 403 error */
    const char *resp = "{\"status\":403,\"headers\":{},\"body\":null,\"error\":\"host not in allowed_hosts\"}";
    uint32_t resp_len = (uint32_t)strlen(resp);
    uint32_t resp_len_net = htonl(resp_len);
    write(sv[0], &resp_len_net, 4);
    write(sv[0], resp, resp_len);
    close(sv[0]);

    int status;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    printf("  PASS test_fetch_error_response\n");
}

static void test_fetch_timeout(void)
{
    /* T133: fd never responds — fetch returns error after timeout.
     * Use a short test: close fd immediately to simulate broken proxy. */
    int sv[2];
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    assert(rc == 0);

    pid_t pid = fork();
    assert(pid >= 0);

    if (pid == 0) {
        close(sv[0]);
        int flags = fcntl(sv[1], F_GETFD);
        fcntl(sv[1], F_SETFD, flags & ~FD_CLOEXEC);
        char fd_str[16];
        snprintf(fd_str, sizeof(fd_str), "%d", sv[1]);
        setenv("MJS_FETCH_FD", fd_str, 1);
        /* fetch should return error object (not crash) when proxy dies */
        execlp(MJS_PATH, "mjs", "-e",
               "var r = fetch('http://x.com/t'); r.error",
               (char *)NULL);
        _exit(127);
    }

    close(sv[1]);
    /* Read request then close fd — simulates proxy crash */
    uint32_t req_len_net;
    read(sv[0], &req_len_net, 4);
    uint32_t req_len = ntohl(req_len_net);
    char discard[8192];
    size_t total = 0;
    while (total < req_len) {
        ssize_t r = read(sv[0], discard + total, req_len - total);
        if (r <= 0) break;
        total += (size_t)r;
    }
    close(sv[0]); /* Close without responding */

    int status;
    waitpid(pid, &status, 0);
    /* mjs should exit cleanly (0) — error is in the returned object, not a crash */
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    printf("  PASS test_fetch_timeout\n");
}

int main(void)
{
    printf("test_mjs:\n");
    test_basic_eval();
    test_string_eval();
    test_file_eval();
    test_no_args_shows_usage();
    test_fetch_no_fd_errors();
    test_fetch_via_fd();
    test_pipe_composability();
    test_fetch_with_pipeline();
    test_fetch_alias();
    test_fetch_error_response();
    test_fetch_timeout();
    printf("All mjs tests passed.\n");
    return 0;
}
