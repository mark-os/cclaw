/* Containment `basic` on a host with no namespaces, through the real
 * --run-tool child: the capabilities are masked (CCLAW_CONTAIN_CAPS_MASK) to
 * what such a host has, the parent plans with run_tool_req_init exactly as
 * dispatch does, and the child applies the plan. Twice: with the loopback
 * proxy alone (files 'confined' kept by the path check in cclaw's code,
 * FE_CODE, network by js's curl dialling the proxy, NE_CODE; the shell is
 * refused), and with Landlock and seccomp too (FE_LANDLOCK for every vehicle,
 * NE_SECCOMP for the shell under network 'none'). What cannot be kept is
 * refused before any fork. The mock server is on loopback: no real network. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "contain.h"
#include "mock_server.h"
#include "test_run_tool_shell.h"

static char root[64];
static char state[128], db[160], key[160], agent_dir[160], ws[200], other[200];
static char granted[128], writable[128], scratch[128];
static FilesEnforcer g_fe;   /* what the planner must pick for file and js */

#define MASK_BARE     "0x4"    /* the loopback proxy only */
#define MASK_LANDLOCK "0x1c"   /* + Landlock + seccomp, still no namespaces */
#define MASK_NOTIF    "0x3c"   /* + connect() interception (kernel 5.19+) */

static void put(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

static void setup(void) {
    snprintf(root, sizeof(root), "/tmp/cclaw_cc_%d", (int)getpid());
    snprintf(state, sizeof(state), "%s/state", root);
    snprintf(db, sizeof(db), "%s/cclaw.db", state);
    snprintf(key, sizeof(key), "%s/.cclaw_key", state);
    snprintf(agent_dir, sizeof(agent_dir), "%s/agents/A", state);
    snprintf(ws, sizeof(ws), "%s/workspace", agent_dir);
    snprintf(other, sizeof(other), "%s/agents/B/workspace", state);
    snprintf(granted, sizeof(granted), "%s/granted", root);
    snprintf(writable, sizeof(writable), "%s/writable", root);
    snprintf(scratch, sizeof(scratch), "%s/scratch", root);
    char cmd[1024], p[320];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s %s %s %s %s", root, ws, other,
             granted, writable, scratch);
    assert(system(cmd) == 0);
    put(key, "SECRETKEY\n");
    put(db, "SECRETDB\n");
    snprintf(p, sizeof(p), "%s/secret.txt", other);   put(p, "SECRETOTHER\n");
    snprintf(p, sizeof(p), "%s/own.txt", ws);         put(p, "mine\n");
    snprintf(p, sizeof(p), "%s/g.txt", granted);      put(p, "granted\n");
    snprintf(p, sizeof(p), "%s/key_link", ws);        assert(symlink(key, p) == 0);
    snprintf(p, sizeof(p), "%s/other_link", ws);      assert(symlink(other, p) == 0);
    snprintf(p, sizeof(p), "%s/key_hard", ws);        assert(link(key, p) == 0);
}

static Containment named(const char *name) {
    Containment c = { .known = 0 };
    snprintf(c.name, sizeof(c.name), "%s", name);
    for (size_t i = 0; i < CONTAIN_SHIPPED_N; i++)
        if (strcmp(CONTAIN_SHIPPED[i].name, name) == 0)
            c.known = 1, c.lv = CONTAIN_SHIPPED[i].lv;
    static char *reads[1], *writes[1];
    reads[0] = granted;
    writes[0] = writable;
    c.read_paths = reads;   c.read_path_count = 1;
    c.write_paths = writes; c.write_path_count = 1;
    return c;
}

static char *file_call(const char *tool, const char *k1, const char *v1,
                       const char *k2, const char *v2, int *status) {
    Containment c = named("basic");
    RunToolReq req;
    char why[640];
    assert(run_tool_req_init(&req, EXEC_FILE, tool, &c, ws, NULL, db, why, sizeof(why)) == 0);
    assert(req.plan.fe == g_fe);
    req.tmp_dir = scratch;
    ToolWireArg a[3] = {
        { (char *)k1, TOOL_ARG_TEXT, (char *)v1, NULL, 0 },
        { (char *)k2, TOOL_ARG_TEXT, (char *)v2, NULL, 0 },
        { "overwrite", TOOL_ARG_TEXT, "1", NULL, 0 },
    };
    req.params = a;
    req.param_count = k2 ? 3 : 1;
    return run_tool_req_full(&req, NULL, status);
}

/* What a file tool reaches under FE_CODE: its workspace, scratch and grants
 * (ro/rw as granted) and the system directories to read — never the key or
 * the DB (by path, by symlink, by hard link), another agent's workspace, or
 * a path that climbs out. */
static void test_file_tools_confined(void) {
    struct { const char *tool, *path, *content; int ok; const char *want; } t[] = {
        { "file_read",  "own.txt",                 NULL, 1, "mine" },
        { "file_read",  "%G/g.txt",                NULL, 1, "granted" },
        { "file_read",  "/etc/passwd",             NULL, 1, "root" },
        { "file_read",  "%K",                      NULL, 0, NULL },
        { "file_read",  "%D",                      NULL, 0, NULL },
        { "file_read",  "%O/secret.txt",           NULL, 0, NULL },
        { "file_read",  "key_link",                NULL, 0, NULL },
        { "file_read",  "key_hard",                NULL, 0, NULL },
        { "file_read",  "other_link/secret.txt",   NULL, 0, NULL },
        { "file_read",  "../../B/workspace/secret.txt", NULL, 0, NULL },
        { "file_write", "new.txt",                 "x",  1, "wrote" },
        { "file_write", "%W/new.txt",              "x",  1, "wrote" },
        { "file_write", "%S/t.txt",                "x",  1, "wrote" },
        { "file_write", "%G/new.txt",              "x",  0, NULL },
        { "file_write", "%A/escape.txt",           "x",  0, NULL },
        { "file_write", "sub/../../escape.txt",    "x",  0, NULL },
        { "file_write", "key_link",                "x",  0, NULL },
        { "file_list",  "%A",                      NULL, 0, NULL },
        { "file_list",  ".",                       NULL, 1, "own.txt" },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        char path[512];
        const char *p = t[i].path, *sub = NULL;
        if (p[0] == '%') {
            switch (p[1]) {
            case 'G': sub = granted; break;  case 'W': sub = writable; break;
            case 'S': sub = scratch; break;  case 'K': sub = key; break;
            case 'D': sub = db; break;       case 'O': sub = other; break;
            case 'A': sub = agent_dir; break;
            }
            snprintf(path, sizeof(path), "%s%s", sub, p + 2);
        } else {
            snprintf(path, sizeof(path), "%s", p);
        }
        int status = -1;
        char *r = file_call(t[i].tool, "path", path, t[i].content ? "content" : NULL,
                            t[i].content, &status);
        int ok = t[i].ok ? status == 0 && strstr(r, t[i].want)
                         : status == 1 && !strstr(r, "SECRET");
        if (!ok) printf("    row %zu (%s %s): status=%d %s\n", i, t[i].tool, path, status, r);
        assert(ok);
        free(r);
    }
    /* A walk follows no link out: the key behind key_link is never grepped. */
    int status = -1;
    char *r = file_call("file_grep", "pattern", "SECRET", "path", ws, &status);
    if (strstr(r, "SECRET")) printf("    grep: %s\n", r);
    assert(!strstr(r, "SECRET"));
    free(r);
    /* And nothing was written where it was refused. */
    char p[320];
    snprintf(p, sizeof(p), "%s/escape.txt", agent_dir);
    assert(access(p, F_OK) != 0);
    snprintf(p, sizeof(p), "%s/new.txt", granted);
    assert(access(p, F_OK) != 0);
}

static char *js_call(const Containment *c, const char *code, const char **hosts,
                     size_t nhosts, char **out_hosts, int *status) {
    RunToolReq req;
    char why[640];
    assert(run_tool_req_init(&req, EXEC_JS, "js_eval", c, ws, NULL, db, why, sizeof(why)) == 0);
    assert(req.plan.fe == g_fe && req.plan.ne == NE_CODE);
    req.tmp_dir = scratch;
    req.agent_dir = agent_dir;
    req.host_rules = hosts;
    req.host_count = nhosts;
    req.timeout = 15;
    ToolWireArg a = { "code", TOOL_ARG_TEXT, (char *)code, NULL, 0 };
    req.params = &a;
    req.param_count = 1;
    return run_tool_req_full(&req, out_hosts, status);
}

/* js fs.* obeys the same path check. */
static void test_js_fs_confined(void) {
    char code[2048];
    snprintf(code, sizeof(code),
        "function t(f){try{return String(f()).trim()}catch(e){return 'DENIED'}}"
        "[t(()=>fs.readFile('own.txt')), t(()=>fs.readFile('%s/g.txt')),"
        " t(()=>fs.readFile('%s')), t(()=>fs.readFile('key_link')),"
        " t(()=>fs.readFile('%s/secret.txt')), t(()=>fs.readdir('%s').length),"
        " t(()=>fs.writeFile('%s/w.txt','x')), t(()=>fs.writeFile('%s/w.txt','x')),"
        " t(()=>fs.stat('%s').size)].join('|')",
        granted, key, other, state, writable, granted, db);
    Containment c = named("basic");
    int status = -1;
    char *r = js_call(&c, code, NULL, 0, NULL, &status);
    const char *want = "mine|granted|DENIED|DENIED|DENIED|DENIED|1|DENIED|DENIED";
    if (!strstr(r, want)) printf("    js fs: %s\n", r);
    assert(strstr(r, want));
    free(r);
}

/* js http_request under NE_CODE goes through the proxy with no namespace: a
 * granted address is fetched, and an ungranted name for the same server is
 * denied by the proxy — a direct connection would have reached it. Under
 * network 'none' it refuses outright. */
static void test_js_http_through_proxy(void) {
    int port = mock_server_start();
    assert(port > 0);
    mock_server_enqueue(200, "hello-from-mock");
    mock_server_enqueue(200, "must-not-be-served");
    char code[512];
    snprintf(code, sizeof(code),
        "var a=http_request('http://127.0.0.1:%d/v1/chat/completions');"
        "var b=http_request('http://localhost:%d/v1/chat/completions');"
        "a.status+'|'+a.body+'|'+b.status+'|'+(b.denied?'denied':b.error)", port, port);
    const char *grant[] = { "127.0.0.1/32" };
    Containment c = named("basic");
    char *hosts = NULL;
    int status = -1;
    char *r = js_call(&c, code, grant, 1, &hosts, &status);
    int ok = strstr(r, "200|hello-from-mock|-1|denied") && hosts &&
             strstr(hosts, "127.0.0.1") && mock_server_request_count() == 1;
    if (!ok) printf("    js http: %s hosts=%s requests=%d\n", r, hosts ? hosts : "-",
                    mock_server_request_count());
    assert(ok);
    free(r);
    free(hosts);

    Containment off = { .name = "confined-offline", .known = 1,
                        .lv = { FILES_CONFINED, NET_NONE, 0, 0 } };
    r = js_call(&off, code, grant, 1, NULL, &status);
    if (!strstr(r, "allows no network")) printf("    js none: %s\n", r);
    assert(strstr(r, "allows no network") && mock_server_request_count() == 1);
    free(r);
    mock_server_stop();
}

/* What a bare host cannot keep is refused by the parent with the reason and
 * a containment that works — never planned weaker. */
static void test_refused_not_weakened(void) {
    RunToolReq req;
    char why[640];
    Containment basic = named("basic"), std = named("standard"), off = named("offline");
    assert(run_tool_req_init(&req, EXEC_SHELL, "shell_exec", &basic, ws, NULL, db,
                             why, sizeof(why)) != 0);
    assert(strstr(why, "'basic'") && strstr(why, "shell") && strstr(why, "'none'"));
    assert(run_tool_req_init(&req, EXEC_FILE, "file_read", &std, ws, NULL, db,
                             why, sizeof(why)) != 0);
    assert(strstr(why, "'standard'") && strstr(why, "user namespaces"));
    assert(run_tool_req_init(&req, EXEC_JS, "js_eval", &off, ws, NULL, db,
                             why, sizeof(why)) != 0);
    assert(strstr(why, "'offline'"));
}

/* Landlock and seccomp but no connect() interception: the forced network
 * level has no enforcer for the shell, and the refusal says what is missing. */
static void test_firewalled_needs_notif(void) {
    RunToolReq req;
    char why[640];
    Containment bf = named("basic-firewalled");
    assert(run_tool_req_init(&req, EXEC_SHELL, "shell_exec", &bf, ws, NULL, db,
                             why, sizeof(why)) != 0);
    assert(strstr(why, "'basic-firewalled'") && strstr(why, "5.19") && strstr(why, "'basic'"));
}

/* A shell call planned from the row, as dispatch plans it. */
static char *shell_call(const Containment *c, const char *cmd, FilesEnforcer fe,
                        NetEnforcer ne, const char **hosts, size_t nhosts) {
    RunToolReq req;
    char why[640];
    if (run_tool_req_init(&req, EXEC_SHELL, "shell_exec", c, ws, NULL, db, why,
                          sizeof(why)) != 0) {
        printf("    refused: %s\n", why);
        assert(0);
    }
    assert(req.plan.fe == fe && req.plan.ne == ne);
    req.tmp_dir = scratch;
    req.agent_dir = agent_dir;
    req.host_rules = hosts;
    req.host_count = nhosts;
    req.command = cmd;
    req.timeout = 10;
    return run_tool_req_full(&req, NULL, NULL);
}

/* The shell under `basic` with Landlock: the key, the DB, another agent's
 * workspace and the host's own writable places (/tmp, the user's home,
 * /dev/shm) are out of reach; its workspace, scratch and grants are as
 * granted; programs run; other processes' /proc secrets are refused by
 * Landlock's ptrace scoping. Every "DENIED" row fails without Landlock. */
static void test_shell_landlock(void) {
    char home_probe[300], shm_probe[64], tmp_probe[64];
    struct passwd *pw = getpwuid(getuid());
    assert(pw && pw->pw_dir);
    snprintf(home_probe, sizeof(home_probe), "%s/.cclaw_ll_probe_%d", pw->pw_dir, (int)getpid());
    snprintf(shm_probe, sizeof(shm_probe), "/dev/shm/cclaw_ll_probe_%d", (int)getpid());
    snprintf(tmp_probe, sizeof(tmp_probe), "/tmp/cclaw_ll_probe_%d", (int)getpid());
    struct { const char *fmt, *arg, *want; } t[] = {
        { "cat %s 2>/dev/null || echo DENIED",            key,        "DENIED" },
        { "cat %s 2>/dev/null || echo DENIED",            db,         "DENIED" },
        { "cat %s/secret.txt 2>/dev/null || echo DENIED", other,      "DENIED" },
        { "cat key_link 2>/dev/null || echo DENIED",      NULL,       "DENIED" },
        { "echo x > %s 2>/dev/null || echo DENIED",       tmp_probe,  "DENIED" },
        { "echo x > %s 2>/dev/null || echo DENIED",       home_probe, "DENIED" },
        { "echo x > %s 2>/dev/null || echo DENIED",       shm_probe,  "DENIED" },
        { "echo x > %s/new.txt 2>/dev/null || echo DENIED", granted,  "DENIED" },
        { "cat /proc/$PPID/environ 2>/dev/null || echo DENIED", NULL, "DENIED" },
        { "cat /proc/%s/environ 2>/dev/null || echo DENIED", "<test>", "DENIED" },
        { "cat %s/g.txt",                                 granted,    "granted" },
        { "echo w > %s/w.txt && cat %s/w.txt",            writable,   "w\n" },
        { "echo own > o2.txt && cat o2.txt",              NULL,       "own\n" },
        { "echo t > \"$TMPDIR/t\" && cat \"$TMPDIR/t\" && echo $TMPDIR", NULL, scratch },
        { "head -c 4 /usr/bin/env | od -c | head -1",     NULL,       "E   L   F" },
        { "grep -c ^Name: /proc/self/status",             NULL,       "1\n" },
        { "ls / | grep -c ^usr$; echo a b | grep b | wc -l >/dev/null && echo PIPE", NULL, "PIPE" },
    };
    Containment c = named("basic");
    char testpid[16];
    snprintf(testpid, sizeof(testpid), "%d", (int)getpid());
    int bad = 0;
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        char cmd[1024];
        const char *arg = t[i].arg && strcmp(t[i].arg, "<test>") == 0 ? testpid : t[i].arg;
        snprintf(cmd, sizeof(cmd), t[i].fmt, arg, arg);
        char *r = shell_call(&c, cmd, FE_LANDLOCK, NE_ENV, NULL, 0);
        int ok = strstr(r, t[i].want) && !strstr(r, "SECRET") && strstr(r, "[exit 0]");
        if (!ok) printf("    row %zu (%s): %s\n", i, cmd, r);
        bad += !ok;
        free(r);
    }
    int left = access(home_probe, F_OK) == 0 || access(shm_probe, F_OK) == 0 ||
               access(tmp_probe, F_OK) == 0;
    unlink(home_probe), unlink(shm_probe), unlink(tmp_probe);  /* after a failure */
    assert(bad == 0 && !left);
}

/* Files 'confined' + network 'none' with no namespaces: Landlock and the
 * seccomp filter. No socket of any family — inet, unix (so no pathname socket
 * on the host either, docker.sock's kind, which Landlock does not mediate),
 * netlink; a socketpair still works. Under `basic` (advisory) inet must work: the
 * proxy variables point at a loopback port. */
static void test_shell_seccomp_none(void) {
    const char *py = access("/usr/bin/python3", X_OK) == 0 ? "/usr/bin/python3" : NULL;
    if (!py) { printf("    SKIP: no /usr/bin/python3 to open sockets with\n"); return; }
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "cat > s.py <<'EOF'\nimport socket\nr = []\n"
             "for f in (socket.AF_INET, socket.AF_INET6, socket.AF_UNIX, socket.AF_NETLINK):\n"
             "    try: socket.socket(f); r.append('open')\n"
             "    except OSError as e: r.append('errno%%d' %% e.errno)\n"
             "socket.socketpair(); print(' '.join(r) + ' pair')\nEOF\n%s s.py", py);
    Containment off = { .name = "confined-none", .known = 1,
                        .lv = { FILES_CONFINED, NET_NONE, 0, 0 } };
    char *r = shell_call(&off, cmd, FE_LANDLOCK, NE_SECCOMP, NULL, 0);
    int ok = strstr(r, "errno13 errno13 errno13 errno13 pair") != NULL;
    if (!ok) printf("    none: %s\n", r);
    assert(ok);
    free(r);
    Containment basic = named("basic");
    r = shell_call(&basic, cmd, FE_LANDLOCK, NE_ENV, NULL, 0);
    ok = strstr(r, "open open open ") != NULL;   /* inet, inet6, unix */
    if (!ok) printf("    basic: %s\n", r);
    assert(ok);
    free(r);
}

/* Something only the kernel catches for file and js: the path check lets
 * them read the system directories, /dev included; Landlock allows only the
 * device nodes programs need, so a file in the host's /dev/shm is refused. */
static void test_file_kernel_backstop(void) {
    char shm[64];
    snprintf(shm, sizeof(shm), "/dev/shm/cclaw_cc_%d", (int)getpid());
    put(shm, "SHMSECRET\n");
    int status = -1;
    char *r = file_call("file_read", "path", shm, NULL, NULL, &status);
    unlink(shm);
    if (status != 1 || strstr(r, "SHMSECRET")) printf("    shm: status=%d %s\n", status, r);
    assert(status == 1 && !strstr(r, "SHMSECRET"));
    free(r);
}

/* Files 'open' with network 'firewalled' on a host with user namespaces but
 * (masked) no mount namespace: a user + network namespace alone. The proxy
 * fetches a granted address (--noproxy '': the variables exempt loopback);
 * a program that ignores the proxy variables reaches nothing on the host,
 * which advisory could not promise. */
static void test_shell_netns_only(void) {
    int port = mock_server_start();
    assert(port > 0);
    mock_server_enqueue(200, "via-proxy");
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "curl -s -m 5 --noproxy '' http://127.0.0.1:%d/v1/chat/completions; echo;"
             " curl -s -m 5 --noproxy '*' http://127.0.0.1:%d/v1/chat/completions || echo NO_ROUTE", port, port);
    const char *grant[] = { "127.0.0.1/32" };
    Containment c = { .name = "open-firewalled", .known = 1, .lv = { FILES_OPEN, NET_FIREWALLED, 0, 0 } };
    char *r = shell_call(&c, cmd, FE_NONE, NE_NETNS, grant, 1);
    int ok = strstr(r, "via-proxy") && strstr(r, "NO_ROUTE") && mock_server_request_count() == 1;
    if (!ok) printf("    netns: %s requests=%d\n", r, mock_server_request_count());
    assert(ok);
    free(r);
    mock_server_stop();
}

/* Under `basic` the shell's shim dials the proxy socket at its real path in
 * the agent folder; one deep enough to pass sun_path (108 bytes) still gets
 * through. net_shim truncated the path and the shell had no proxy (fails on
 * 7980e0c). */
static void test_shell_advisory_deep_dir(void) {
    char deep[256], mk[300];
    int n = snprintf(deep, sizeof(deep), "%s/", agent_dir);
    while (n < 130) n += snprintf(deep + n, sizeof(deep) - (size_t)n, "deep_agent_dir/");
    deep[n - 1] = '\0';
    snprintf(mk, sizeof(mk), "mkdir -p %s", deep);
    assert(system(mk) == 0);
    int port = mock_server_start();
    assert(port > 0);
    mock_server_enqueue(200, "deep-ok");
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "curl -s -m 5 --noproxy '' http://127.0.0.1:%d/v1/chat/completions", port);
    const char *grant[] = { "127.0.0.1/32" };
    Containment c = named("basic");
    RunToolReq req;
    char why[640];
    assert(run_tool_req_init(&req, EXEC_SHELL, "shell_exec", &c, ws, NULL, db, why, sizeof(why)) == 0);
    req.tmp_dir = scratch;
    req.agent_dir = deep;
    req.host_rules = grant;
    req.host_count = 1;
    req.command = cmd;
    req.timeout = 10;
    char *r = run_tool_req_full(&req, NULL, NULL);
    if (!strstr(r, "deep-ok")) printf("    deep: %s\n", r);
    assert(strstr(r, "deep-ok"));
    free(r);
    mock_server_stop();
}

/* The DNS stub parses whatever a contained program sends to 127.0.0.1:53.
 * One table of packets: a reply's rcode and answer count, or none at all. A
 * name reaches a CONNECT line, so only hostname characters are kept. */
#define HDR(op, qd) 0x12, 0x34, op, 0, 0, qd, 0, 0, 0, 0, 0, 0
#define EXAMPLE 1, 'a', 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0
static void test_dns_stub_parser(void) {
    static const unsigned char a[] = { HDR(1, 1), EXAMPLE, 0, 1, 0, 1 },
        a_again[] = { HDR(1, 1), 1, 'A', 7, 'E', 'x', 'a', 'm', 'p', 'l', 'e', 0, 0, 1, 0, 1 },
        b[] = { HDR(1, 1), 1, 'b', 0, 0, 1, 0, 1 },
        aaaa[] = { HDR(1, 1), EXAMPLE, 0, 28, 0, 1 },
        mx[] = { HDR(1, 1), EXAMPLE, 0, 15, 0, 1 },
        chaos[] = { HDR(1, 1), EXAMPLE, 0, 1, 0, 3 },
        shorthdr[] = { 0x12, 0x34, 1, 0, 0, 1, 0, 0, 0, 0, 0 },
        reply[] = { HDR(0x81, 1), EXAMPLE, 0, 1, 0, 1 },
        two_q[] = { HDR(1, 2), EXAMPLE, 0, 1, 0, 1 },
        no_q[] = { HDR(1, 0) },
        opcode[] = { HDR(0x11, 1), EXAMPLE, 0, 1, 0, 1 },
        overrun[] = { HDR(1, 1), 9, 'a', 'b', 0, 1, 0, 1 },
        pointer[] = { HDR(1, 1), 0xc0, 0x0c, 0, 1, 0, 1 },
        no_end[] = { HDR(1, 1), 1, 'a', 1, 'b' },
        no_type[] = { HDR(1, 1), EXAMPLE, 0, 1 },
        badchar[] = { HDR(1, 1), 3, 'a', ':', 'b', 0, 0, 1, 0, 1 },
        crlf[] = { HDR(1, 1), 3, 'a', '\r', '\n', 0, 0, 1, 0, 1 },
        root[] = { HDR(1, 1), 0, 0, 1, 0, 1 };
    unsigned char lab64[12 + 66 + 4] = { HDR(1, 1), 64 }, long_name[12 + 5 * 64 + 5] = { HDR(1, 1) };
    memset(lab64 + 13, 'x', 64);
    for (int i = 0; i < 5; i++) {
        long_name[12 + i * 64] = 63;
        memset(long_name + 13 + i * 64, 'y', 63);
    }
    long_name[sizeof(long_name) - 4] = 1;   /* type A after a missing root: still too long */
    struct { const unsigned char *p; size_t n; int rcode, an; unsigned addr; } t[] = {
        { a,        sizeof(a),        0, 1, 1 },
        { a_again,  sizeof(a_again),  0, 1, 1 },   /* same name, any case: same address */
        { b,        sizeof(b),        0, 1, 2 },
        { aaaa,     sizeof(aaaa),     0, 0, 0 },
        { mx,       sizeof(mx),       4, 0, 0 },
        { chaos,    sizeof(chaos),    4, 0, 0 },
        { shorthdr, sizeof(shorthdr), -1, 0, 0 },
        { reply,    sizeof(reply),    -1, 0, 0 },
        { two_q,    sizeof(two_q),    1, 0, 0 },
        { no_q,     sizeof(no_q),     1, 0, 0 },
        { opcode,   sizeof(opcode),   4, 0, 0 },
        { overrun,  sizeof(overrun),  1, 0, 0 },
        { pointer,  sizeof(pointer),  1, 0, 0 },
        { no_end,   sizeof(no_end),   1, 0, 0 },
        { no_type,  sizeof(no_type),  1, 0, 0 },
        { badchar,  sizeof(badchar),  3, 0, 0 },
        { crlf,     sizeof(crlf),     3, 0, 0 },
        { root,     sizeof(root),     3, 0, 0 },
        { lab64,    sizeof(lab64),    1, 0, 0 },
        { long_name, sizeof(long_name), 1, 0, 0 },
    };
    static ContainDnsNames names;
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        unsigned char out[512];
        size_t n = contain_dns_reply(&names, t[i].p, t[i].n, out, sizeof(out));
        int ok = t[i].rcode < 0 ? n == 0
            : n >= 12 && out[0] == 0x12 && out[1] == 0x34 && (out[2] & 0x80) &&
              (out[3] & 0x0f) == t[i].rcode && out[7] == t[i].an &&
              (!t[i].an || (n == t[i].n + 16 && out[n - 4] == 198 && out[n - 3] == 18 &&
                            out[n - 2] == 0 && out[n - 1] == t[i].addr));
        if (!ok) printf("    row %zu: n=%zu rcode=%d an=%d\n", i, n, n ? out[3] & 0x0f : -1,
                        n ? out[7] : -1);
        assert(ok);
    }
    assert(names.n == 2 && strcmp(names.name[0], "a.example") == 0);
}

/* A non-loopback IPv4 address of this host, for destinations that must go
 * through the proxy (the mock listens on every address). "" when none. */
static void host_ip(char *out, size_t cap) {
    struct ifaddrs *ifa, *i;
    out[0] = '\0';
    if (getifaddrs(&ifa) != 0) return;
    for (i = ifa; i; i = i->ifa_next)
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET &&
            (ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr) >> 24) != 127) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, out, (socklen_t)cap);
            break;
        }
    freeifaddrs(ifa);
}

/* A shell call that must be planned transparent (the namespace) or not. */
static char *shell_call_t(const Containment *c, const char *cmd, FilesEnforcer fe,
                          NetEnforcer ne, int transparent, const char **hosts, size_t n) {
    RunToolReq req;
    char why[640];
    assert(run_tool_req_init(&req, EXEC_SHELL, "shell_exec", c, ws, NULL, db, why,
                             sizeof(why)) == 0);
    assert(req.plan.fe == fe && req.plan.ne == ne && req.plan.transparent == transparent);
    req.tmp_dir = scratch;
    req.agent_dir = agent_dir;
    req.host_rules = hosts;
    req.host_count = n;
    req.command = cmd;
    req.timeout = 20;
    return run_tool_req_full(&req, NULL, NULL);
}

static const char PY_GET[] =
    "import socket, sys\n"
    "def get(h, p):\n"
    "    try:\n"
    "        s = socket.create_connection((h, p), timeout=5)\n"
    "        s.sendall(b'GET /v1/chat/completions HTTP/1.0\\r\\nConnection: close\\r\\n\\r\\n')\n"
    "        d = b''\n"
    "        while True:\n"
    "            c = s.recv(4096)\n"
    "            if not c: break\n"
    "            d += c\n"
    "        return d.split(b'\\r\\n\\r\\n', 1)[-1].decode()\n"
    "    except OSError as e:\n"
    "        return 'errno%d' % e.errno\n"
    /* From a second thread: the supervisor is told the calling thread's id,
     * and must find the process (and its descriptors) from that. */
    "import threading\n"
    "out = []\n"
    "t = threading.Thread(target=lambda: out.append("
    "' '.join(get(a, int(sys.argv[1])) for a in sys.argv[2:])))\n"
    "t.start(); t.join(); print(out[0])\n";

static void write_ws_file(const char *name, const char *text) {
    char p[320];
    snprintf(p, sizeof(p), "%s/%s", ws, name);
    put(p, text);
}

/* In the namespace, with interception: a program that knows nothing of
 * proxies resolves a name (the DNS stub) and connects; the proxy lets the
 * granted name through and refuses the other. A proxy-aware call still
 * works. Without interception the first would have had no route at all. */
static void test_shell_transparent(void) {
    struct addrinfo *ai;
    if (access("/usr/bin/python3", X_OK) != 0 ||
        getaddrinfo("cclaw-mock.localhost", NULL, NULL, &ai) != 0) {
        printf("    SKIP: needs /usr/bin/python3 and a host resolver that answers"
               " *.localhost\n");
        return;
    }
    freeaddrinfo(ai);
    write_ws_file("get.py", PY_GET);
    int port = mock_server_start();
    assert(port > 0);
    mock_server_enqueue(200, "transparent-ok");
    mock_server_enqueue(200, "via-proxy");
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "python3 get.py %d cclaw-mock.localhost cclaw-denied.localhost;"
             " curl -s -m 5 --noproxy '' http://127.0.0.1:%d/v1/chat/completions", port, port);
    const char *grant[] = { "cclaw-mock.localhost", "127.0.0.1/32" };
    Containment c = named("standard");
    char *r = shell_call_t(&c, cmd, FE_MOUNTNS, NE_NETNS, 1, grant, 2);
    int ok = strstr(r, "transparent-ok errno111") && strstr(r, "via-proxy") &&
             mock_server_request_count() == 2;
    if (!ok) printf("    transparent: %s requests=%d\n", r, mock_server_request_count());
    assert(ok);
    free(r);
    mock_server_stop();
}

/* No namespaces, interception the enforcer (basic-firewalled): a direct
 * connection to an ungranted address is refused, to a granted one it goes
 * through the proxy; no UDP or raw socket can be made, and no Fast Open send
 * connects past connect(); a proxy-aware call works. */
static void test_shell_notif_forced(void) {
    char ip[64];
    host_ip(ip, sizeof(ip));
    if (!ip[0] || access("/usr/bin/python3", X_OK) != 0) {
        printf("    SKIP: needs a non-loopback IPv4 address and /usr/bin/python3\n");
        return;
    }
    int port = mock_server_start();
    assert(port > 0);
    mock_server_enqueue(200, "direct-tunnelled");
    mock_server_enqueue(200, "proxy-aware");
    char cmd[1024], grant_ip[80];
    snprintf(cmd, sizeof(cmd),
             "curl -s -m 5 --noproxy '*' http://%s:%d/v1/chat/completions || echo DIRECT_REFUSED;"
             " curl -s -m 5 http://%s:%d/v1/chat/completions; echo;"
             " python3 -c \"import socket\n"
             "for t in (socket.SOCK_STREAM, socket.SOCK_DGRAM, socket.SOCK_RAW):\n"
             "    try: socket.socket(socket.AF_INET, t); print('open')\n"
             "    except OSError as e: print('errno%%d' %% e.errno)\n"
             "try: socket.socket().sendto(b'x', socket.MSG_FASTOPEN, ('%s', %d)); print('fastopen')\n"
             "except OSError as e: print('errno%%d' %% e.errno)\"",
             ip, port, ip, port, ip, port);
    snprintf(grant_ip, sizeof(grant_ip), "%s/32", ip);
    const char *grant[] = { grant_ip };
    Containment c = named("basic-firewalled");
    char *r = shell_call_t(&c, cmd, FE_LANDLOCK, NE_NOTIF, 0, NULL, 0);
    int ok = strstr(r, "DIRECT_REFUSED") && !strstr(r, "direct-tunnelled") &&
             strstr(r, "open\nerrno13\nerrno13\nerrno1\n") && mock_server_request_count() == 0;
    if (!ok) printf("    ungranted: %s requests=%d\n", r, mock_server_request_count());
    assert(ok);
    free(r);
    r = shell_call_t(&c, cmd, FE_LANDLOCK, NE_NOTIF, 0, grant, 1);
    ok = strstr(r, "direct-tunnelled") && strstr(r, "proxy-aware") &&
         !strstr(r, "DIRECT_REFUSED") && mock_server_request_count() == 2;
    if (!ok) printf("    granted: %s requests=%d\n", r, mock_server_request_count());
    assert(ok);
    free(r);
    mock_server_stop();
}

/* The supervisor reads the address once and connects the socket itself, so
 * a second thread rewriting the sockaddr during connect() cannot move the
 * connection: the address flips between closed loopback (connected directly)
 * and an ungranted listener on this host's own address, which a continued
 * call would reach. No grants: nothing may ever be accepted there. */
static void test_shell_notif_toctou(void) {
    char ip[64];
    host_ip(ip, sizeof(ip));
    if (!ip[0] || system("command -v cc >/dev/null 2>&1") != 0) {
        printf("    SKIP: needs a non-loopback IPv4 address and a C compiler\n");
        return;
    }
    static const char src[] =
        "#include <arpa/inet.h>\n#include <pthread.h>\n#include <stdio.h>\n"
        "#include <stdlib.h>\n#include <string.h>\n#include <unistd.h>\n"
        "static struct sockaddr_in a, good, bad;\n"
        "static void *flip(void *x) { volatile struct sockaddr_in *p = &a;"
        " for (;;) { *p = bad; *p = good; } return x; }\n"
        "int main(int c, char **v) { (void)c;\n"
        "  good.sin_family = bad.sin_family = AF_INET;\n"
        "  inet_pton(AF_INET, \"127.0.0.1\", &good.sin_addr); good.sin_port = htons(atoi(v[1]));\n"
        "  inet_pton(AF_INET, v[2], &bad.sin_addr); bad.sin_port = htons(atoi(v[3]));\n"
        "  a = good; pthread_t t; pthread_create(&t, 0, flip, 0); int ok = 0;\n"
        "  for (int i = 0; i < 300; i++) { int s = socket(AF_INET, SOCK_STREAM, 0);\n"
        "    if (connect(s, (struct sockaddr *)&a, sizeof a) == 0) ok++; close(s); }\n"
        "  printf(\"connected=%d\\n\", ok); return 0; }\n";
    write_ws_file("flip.c", src);
    char cmd[640];
    snprintf(cmd, sizeof(cmd), "cc -O2 -pthread -o %s/flip %s/flip.c", ws, ws);
    assert(system(cmd) == 0);
    int lis = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0), closed = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in la = { .sin_family = AF_INET }, ca = { .sin_family = AF_INET };
    inet_pton(AF_INET, ip, &la.sin_addr);
    inet_pton(AF_INET, "127.0.0.1", &ca.sin_addr);
    socklen_t ll = sizeof(la), cl = sizeof(ca);
    assert(bind(lis, (struct sockaddr *)&la, sizeof(la)) == 0 && listen(lis, 512) == 0);
    assert(bind(closed, (struct sockaddr *)&ca, sizeof(ca)) == 0);
    assert(getsockname(lis, (struct sockaddr *)&la, &ll) == 0 &&
           getsockname(closed, (struct sockaddr *)&ca, &cl) == 0);
    close(closed);   /* nothing listens on that loopback port now */
    snprintf(cmd, sizeof(cmd), "./flip %d %s %d", ntohs(ca.sin_port), ip, ntohs(la.sin_port));
    Containment c = named("basic-firewalled");
    char *r = shell_call_t(&c, cmd, FE_LANDLOCK, NE_NOTIF, 0, NULL, 0);
    int accepted = 0;
    while (accept(lis, NULL, NULL) >= 0) accepted++;
    close(lis);
    int ok = strstr(r, "connected=0") && accepted == 0;
    if (!ok) printf("    toctou: %s accepted=%d\n", r, accepted);
    assert(ok);
    free(r);
}

int main(void) {
    TEST_INIT();
    unsigned host = contain_caps(NULL, 0);
    setup();

    /* No namespaces, no Landlock: the path check, the shell refused. */
    setenv("CCLAW_CONTAIN_CAPS_MASK", MASK_BARE, 1);
    assert(!(contain_caps(NULL, 0) & (CONTAIN_CAP_MOUNTNS | CONTAIN_CAP_NETNS)));
    g_fe = FE_CODE;
    RUN(test_file_tools_confined);
    RUN(test_js_fs_confined);
    RUN(test_js_http_through_proxy);
    RUN(test_refused_not_weakened);

    /* No namespaces, with Landlock and seccomp. */
    if ((host & (CONTAIN_CAP_LANDLOCK | CONTAIN_CAP_SECCOMP)) !=
        (CONTAIN_CAP_LANDLOCK | CONTAIN_CAP_SECCOMP)) {
        printf("  SKIP: this kernel has no Landlock or no seccomp (caps %#x)\n", host);
    } else {
        setenv("CCLAW_CONTAIN_CAPS_MASK", MASK_LANDLOCK, 1);
        g_fe = FE_LANDLOCK;
        RUN(test_file_tools_confined);
        RUN(test_js_fs_confined);
        RUN(test_js_http_through_proxy);
        RUN(test_file_kernel_backstop);
        RUN(test_shell_landlock);
        RUN(test_shell_seccomp_none);
        RUN(test_shell_advisory_deep_dir);
        RUN(test_firewalled_needs_notif);
    }

    RUN(test_dns_stub_parser);
    if (!(host & CONTAIN_CAP_NOTIF)) {
        printf("  SKIP: no seccomp user notification with pidfd_getfd here (kernel"
               " 5.19+; caps %#x)\n", host);
    } else {
        if (host & CONTAIN_CAP_MOUNTNS) {
            unsetenv("CCLAW_CONTAIN_CAPS_MASK");
            RUN(test_shell_transparent);
        } else {
            printf("  SKIP: no user namespaces, so no transparent namespace test\n");
        }
        setenv("CCLAW_CONTAIN_CAPS_MASK", MASK_NOTIF, 1);
        RUN(test_shell_notif_forced);
        RUN(test_shell_notif_toctou);
    }

    if (!(host & CONTAIN_CAP_NETNS)) {
        printf("  SKIP: no unprivileged network namespace here\n");
    } else {
        setenv("CCLAW_CONTAIN_CAPS_MASK", "0x6", 1);
        RUN(test_shell_netns_only);
    }
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    assert(system(cmd) == 0);
    return 0;
}
