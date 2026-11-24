/* Containment `basic` on a host with no namespaces, through the real
 * --run-tool child: the capabilities are masked to the loopback proxy alone
 * (CCLAW_CONTAIN_CAPS_MASK), the parent plans with run_tool_req_init exactly
 * as dispatch does, and the child keeps files 'confined' with the path check
 * in cclaw's code (FE_CODE) and network with js's curl dialling the egress
 * proxy itself (NE_CODE). What cannot be kept is refused before any fork.
 * The mock server is on loopback: no real network. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "contain.h"
#include "mock_server.h"
#include "test_run_tool_shell.h"

static char root[64];
static char state[128], db[160], key[160], agent_dir[160], ws[200], other[200];
static char granted[128], writable[128], scratch[128];

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
    assert(req.plan.fe == FE_CODE);
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
    assert(req.plan.fe == FE_CODE && req.plan.ne == NE_CODE);
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

int main(void) {
    TEST_INIT();
    /* The loopback proxy only: what a Linux host without user namespaces
     * has. (CONTAIN=none has nothing, and nothing here needs more.) */
    setenv("CCLAW_CONTAIN_CAPS_MASK", "0x4", 1);
    assert(!(contain_caps(NULL, 0) & (CONTAIN_CAP_MOUNTNS | CONTAIN_CAP_NETNS)));
    setup();
    RUN(test_file_tools_confined);
    RUN(test_js_fs_confined);
    RUN(test_js_http_through_proxy);
    RUN(test_refused_not_weakened);
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    assert(system(cmd) == 0);
    return 0;
}
