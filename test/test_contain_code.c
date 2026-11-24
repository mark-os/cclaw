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

/* Files 'open' with network 'proxied' on a host with user namespaces but
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
    Containment c = { .name = "open-proxied", .known = 1, .lv = { FILES_OPEN, NET_PROXIED, 0, 0 } };
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
