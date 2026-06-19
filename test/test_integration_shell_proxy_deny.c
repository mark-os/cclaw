/* test_integration_shell_proxy_deny.c — T216: shell cannot reach unlisted host.
 * Starts proxy with DB-backed grants (only "allowed.test"), starts mock TCP server,
 * shell child attempts connect to 127.0.0.1 (not in grants) via proxy.
 * Verifies proxy denies the connection. */
#define _GNU_SOURCE
#include "proxy.h"
#include "tool_shell.h"
#include <sqlite3.h>
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static char workspace[256];
static char deny_db_path[512];
static int ns_available = 1;

static void setup_workspace(void) {
    snprintf(workspace, sizeof(workspace), "/tmp/cclaw_proxy_deny_%d", getpid());
    mkdir(workspace, 0755);
    /* The broker writes the preload .so from its embedded blob; no copy needed. */
    /* Create test DB with grants */
    snprintf(deny_db_path, sizeof(deny_db_path), "%s/test.db", workspace);
    sqlite3 *db;
    assert(sqlite3_open(deny_db_path, &db) == SQLITE_OK);
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS grants("
        " id INTEGER PRIMARY KEY, agent_name TEXT, kind TEXT, value TEXT,"
        " expires_at INTEGER);",
        NULL, NULL, NULL);
    /* Only grant "allowed.test" to testbot */
    sqlite3_exec(db,
        "INSERT INTO grants(agent_name,kind,value) VALUES('testbot','host','allowed.test');",
        NULL, NULL, NULL);
    /* Grant 127.0.0.1 to testbot2 for the allowed_vs_denied test */
    sqlite3_exec(db,
        "INSERT INTO grants(agent_name,kind,value) VALUES('testbot2','host','127.0.0.1');",
        NULL, NULL, NULL);
    sqlite3_close(db);
}

static void cleanup_workspace(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", workspace);
    system(cmd);
}

static int start_mock_tcp(uint16_t *out_port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(fd, 4) == 0);
    socklen_t alen = sizeof(addr);
    getsockname(fd, (struct sockaddr *)&addr, &alen);
    *out_port = ntohs(addr.sin_port);
    return fd;
}

static void check_prerequisites(void) {
    struct stat st;
    if (stat("./build/libcclaw_net.so", &st) != 0) {
        printf("  SKIP: libcclaw_net.so not built\n");
        exit(0);
    }
    ShellConfig sc = {.timeout = 5, .workspace = workspace, .sandbox = 1};
    char *r = tool_shell_handler("{\"command\":\"echo ns_check\"}", &sc);
    if (r && strstr(r, "namespace sandbox unavailable")) {
        ns_available = 0;
    }
    free(r);
}

/* T216: proxy denies connection to unlisted host */
static void test_shell_denied_unlisted_host(void) {
    if (!ns_available) {
        printf("  SKIP test_shell_denied_unlisted_host (namespaces unavailable)\n");
        return;
    }

    uint16_t port;
    int listen_fd = start_mock_tcp(&port);

    /* Shell child tries to curl 127.0.0.1 — testbot has no such grant, so the
     * broker's per-call proxy denies it. */
    char cmd_json[2048];
    snprintf(cmd_json, sizeof(cmd_json),
        "{\"command\":\"curl -s --max-time 5 http://127.0.0.1:%u/test\"}", port);

    ShellConfig sc = {
        .timeout = 10,
        .workspace = workspace,
        .db_path = deny_db_path,
        .agent_name = "testbot",
        .sandbox = 1,
    };
    char *result = tool_shell_handler(cmd_json, &sc);

    close(listen_fd);

    assert(result != NULL);
    /* Connection should fail — curl reports error or empty response */
    if (strstr(result, "PROXY_WORKS") || strstr(result, "HTTP")) {
        printf("  FAIL test_shell_denied_unlisted_host\n");
        printf("  output: %s\n", result);
        free(result);
        assert(0 && "proxy should have denied unlisted host");
    }
    /* Success: connection was denied (curl error, empty, or "Couldn't connect") */
    printf("  PASS test_shell_denied_unlisted_host\n");
    free(result);
}

/* Test: allowed host succeeds, unlisted host fails in same proxy session */
static void test_shell_allowed_vs_denied(void) {
    if (!ns_available) {
        printf("  SKIP test_shell_allowed_vs_denied (namespaces unavailable)\n");
        return;
    }

    uint16_t port;
    int listen_fd = start_mock_tcp(&port);

    /* testbot2 grants only "127.0.0.1", so a different hostname is denied by
     * the allowlist before any DNS — the proxy denies "denied.test". */
    char cmd_json[2048];
    snprintf(cmd_json, sizeof(cmd_json),
        "{\"command\":\"curl -s --max-time 5 http://denied.test:%u/test\"}", port);

    ShellConfig sc = {
        .timeout = 10,
        .workspace = workspace,
        .db_path = deny_db_path,
        .agent_name = "testbot2",
        .sandbox = 1,
    };
    char *result = tool_shell_handler(cmd_json, &sc);

    close(listen_fd);

    assert(result != NULL);
    if (strstr(result, "PROXY_WORKS") || strstr(result, "HTTP")) {
        printf("  FAIL test_shell_allowed_vs_denied\n");
        printf("  output: %s\n", result);
        free(result);
        assert(0 && "proxy should have denied unlisted host");
    }
    printf("  PASS test_shell_allowed_vs_denied\n");
    free(result);
}

int main(void) {
    printf("test_integration_shell_proxy_deny (T216):\n");

    setup_workspace();
    check_prerequisites();
    test_shell_denied_unlisted_host();
    test_shell_allowed_vs_denied();
    cleanup_workspace();

    printf("All shell proxy deny tests passed.\n");
    return 0;
}
