/* The containment planner (specs/containment.md): (row, vehicle, host
 * capabilities) → the enforcer for each level, or a refusal. Pure, no
 * namespaces needed. Every refusal rule has a row; a plan that would be
 * weaker than its level is the bug this guards against. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sandbox.h"
#include "contain.h"
#include "test_util.h"

#define NS   (CONTAIN_CAP_MOUNTNS | CONTAIN_CAP_NETNS | CONTAIN_CAP_SHIM)
#define BARE CONTAIN_CAP_SHIM   /* a Linux host without user namespaces */

static Containment row(const char *name) {
    Containment c = { .known = 0 };
    snprintf(c.name, sizeof(c.name), "%s", name);
    for (size_t i = 0; i < CONTAIN_SHIPPED_N; i++)
        if (strcmp(CONTAIN_SHIPPED[i].name, name) == 0) {
            c.known = 1;
            c.lv = CONTAIN_SHIPPED[i].lv;
        }
    return c;
}

#define OK(fe, ne) fe, ne, NULL
#define NO(word)   FE_NONE, NE_NONE, word

static void test_plan_table(void) {
    static const struct {
        const char *row; FilesLevel files; NetLevel net;  /* row NULL: use files/net */
        ExecVehicle v; unsigned caps;
        FilesEnforcer fe; NetEnforcer ne; const char *refusal;
    } t[] = {
        /* With namespaces: the shipped rows run as they always have. */
        { "standard", 0, 0, EXEC_SHELL, NS, OK(FE_MOUNTNS, NE_NETNS) },
        { "standard", 0, 0, EXEC_JS,    NS, OK(FE_MOUNTNS, NE_NETNS) },
        { "standard", 0, 0, EXEC_FILE,  NS, OK(FE_MOUNTNS, NE_NETNS) },
        { "offline",  0, 0, EXEC_SHELL, NS, OK(FE_MOUNTNS, NE_NETNS) },
        { "basic",    0, 0, EXEC_SHELL, NS, OK(FE_MOUNTNS, NE_NETNS) },
        { "none",     0, 0, EXEC_SHELL, NS, OK(FE_NONE, NE_NONE) },
        { "none",     0, 0, EXEC_JS,    NS, OK(FE_NONE, NE_NONE) },
        /* files private needs the mount namespace for every vehicle. */
        { "standard", 0, 0, EXEC_FILE,  BARE, NO("user namespaces") },
        { "standard", 0, 0, EXEC_JS,    BARE, NO("user namespaces") },
        { "offline",  0, 0, EXEC_SHELL, BARE, NO("user namespaces") },
        { "standard", 0, 0, EXEC_SHELL, 0,    NO("user namespaces") },
        /* files confined: file and js keep it in code, the shell refuses. */
        { "basic",    0, 0, EXEC_FILE,  BARE, OK(FE_CODE, NE_NONE) },
        { "basic",    0, 0, EXEC_JS,    BARE, OK(FE_CODE, NE_CODE) },
        { "basic",    0, 0, EXEC_JS,    0,    OK(FE_CODE, NE_CODE) },
        { "basic",    0, 0, EXEC_SHELL, BARE, NO("confined") },
        /* Network with no namespace: js in code, the shell refuses, except
         * advisory, which is the proxy variables — when the backend has the
         * loopback proxy at all. */
        { NULL, FILES_OPEN, NET_PROXIED, EXEC_JS,    BARE, OK(FE_NONE, NE_CODE) },
        { NULL, FILES_OPEN, NET_NONE,    EXEC_JS,    BARE, OK(FE_NONE, NE_CODE) },
        { NULL, FILES_OPEN, NET_NONE,    EXEC_FILE,  BARE, OK(FE_NONE, NE_NONE) },
        { NULL, FILES_OPEN, NET_PROXIED, EXEC_SHELL, BARE, NO("proxied") },
        { NULL, FILES_OPEN, NET_PROXIED, EXEC_SHELL, NS,   NO("proxied") },
        { NULL, FILES_OPEN, NET_NONE,    EXEC_SHELL, BARE, NO("'none'") },
        { NULL, FILES_OPEN, NET_ADVISORY, EXEC_SHELL, BARE, OK(FE_NONE, NE_ENV) },
        { NULL, FILES_OPEN, NET_ADVISORY, EXEC_SHELL, 0,   NO("advisory") },
        /* The namespace cannot be direct. */
        { NULL, FILES_CONFINED, NET_DIRECT, EXEC_JS, NS,   NO("direct") },
        { NULL, FILES_CONFINED, NET_DIRECT, EXEC_JS, BARE, OK(FE_CODE, NE_NONE) },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        Containment c = t[i].row ? row(t[i].row)
            : (Containment){ .name = "custom", .known = 1, .lv = { t[i].files, t[i].net, 0, 0 } };
        ContainPlan p;
        char why[640];
        int rc = contain_plan(&c, t[i].v, t[i].caps, &p, why, sizeof(why));
        int ok = t[i].refusal
            ? rc != 0 && strstr(why, t[i].refusal) && strstr(why, c.name)
            : rc == 0 && p.fe == t[i].fe && p.ne == t[i].ne &&
              p.lv.files == c.lv.files && p.lv.net == c.lv.net &&
              p.lv.nproc == c.lv.nproc && p.lv.cpu_sec == c.lv.cpu_sec;
        if (!ok) printf("    row %zu: rc=%d fe=%d ne=%d %s\n", i, rc, p.fe, p.ne, why);
        assert(ok);
    }
}

/* A refusal names a shipped containment this host can keep for the vehicle,
 * found by the same rules; an unknown name is refused outright, never read
 * as a default. */
static void test_refusal_names_alternative(void) {
    char why[640];
    ContainPlan p;
    Containment std = row("standard");
    assert(contain_plan(&std, EXEC_JS, BARE, &p, why, sizeof(why)) != 0);
    assert(strstr(why, "'basic'"));
    assert(contain_plan(&std, EXEC_SHELL, BARE, &p, why, sizeof(why)) != 0);
    assert(strstr(why, "'none'"));

    static const char *unknown[] = { "", "trusted", "host", "restricted", "STANDARD" };
    for (size_t i = 0; i < sizeof(unknown) / sizeof(unknown[0]); i++) {
        Containment u = row(unknown[i]);
        assert(contain_plan(&u, EXEC_FILE, NS, &p, why, sizeof(why)) != 0);
        assert(strstr(why, "not a row"));
    }

    assert(contain_suggest(NS) && strcmp(contain_suggest(NS), "standard") == 0);
    assert(contain_suggest(BARE) && strcmp(contain_suggest(BARE), "none") == 0);
    Containment basic = row("basic");
    assert(contain_keepable(&basic, NS) && !contain_keepable(&basic, BARE));
}

int main(void) {
    TEST_INIT();
    RUN(test_plan_table);
    RUN(test_refusal_names_alternative);
    return 0;
}
