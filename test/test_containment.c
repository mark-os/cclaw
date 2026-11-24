/* Unit tests for sandbox_policy_from_containment / containment_resolve: the
 * containment value → policy-bundle mapping (specs/containment.md).
 * Pure functions, no namespaces required. Locks in the fall-through rule:
 * unknown/NULL values get the standard bundle, never something looser. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sandbox.h"
#include "test_util.h"

static SandboxConfig cfg_for(const char *containment) {
    SandboxConfig c; memset(&c, 0xAA, sizeof(c)); /* poison: catch unset fields */
    sandbox_policy_from_containment(containment, &c);
    return c;
}

static void test_none(void) {
    SandboxConfig c = cfg_for("none");
    assert(c.sandbox == 0 && c.env_mode == 0 && c.net_mode == 0 &&
        c.mount_cwd == 1 &&
        c.rlimits.nproc == 0 && c.rlimits.as_mb == 0 && c.rlimits.cpu_sec == 0);
}

static void test_standard(void) {
    /* standard is the merged former 'trusted': clean env, no CWD mount, no
     * CPU cap. NPROC stays as the fork-bomb backstop, nothing else. */
    SandboxConfig c = cfg_for("standard");
    assert(c.sandbox == 1 && c.env_mode == 1 && c.net_mode == 0 &&
        c.mount_cwd == 0 &&
        c.rlimits.nproc == 256 && c.rlimits.as_mb == 0 && c.rlimits.cpu_sec == 0);
}

static void test_offline(void) {
    /* What makes offline offline is net_mode=1 (empty netns, no proxy
     * socket). The workspace stays rw so $HOME works, and the limits are a
     * runaway backstop that still permits a real build. */
    SandboxConfig c = cfg_for("offline");
    assert(c.sandbox == 1 && c.env_mode == 1 && c.net_mode == 1 &&
        c.mount_cwd == 0 &&
        c.rlimits.nproc == 64 && c.rlimits.as_mb == 0 && c.rlimits.cpu_sec == 120);
}

static void test_fallthrough(void) {
    /* Unknown values must land on standard — a typo'd value, or a retired
     * one ('bootstrap', 'trusted', and the pre-v71 'host'), must never yield
     * the uncontained bundle. The v71 patch rewrites stored 'host' and
     * 'restricted'; a hand-edited row could still carry one. */
    const char *unknowns[] = {NULL, "", "bootstrap", "trusted", "TRUSTED",
                              "host", "NONE", "yolo"};
    SandboxConfig std = cfg_for("standard");
    for (size_t i = 0; i < sizeof(unknowns)/sizeof(unknowns[0]); i++) {
        SandboxConfig c = cfg_for(unknowns[i]);
        if (memcmp(&c, &std, sizeof(c)) != 0) {
            printf("  row '%s'\n", unknowns[i] ? unknowns[i] : "(null)");
            assert(!"unknown containment must resolve to the standard bundle");
        }
    }
}

static void test_resolve_matches_policy(void) {
    /* containment_resolve copies the policy half field-by-field (the
     * structs differ, so no memcmp across them) — pin each field instead. */
    static const char *levels[] = {"none", "standard", "offline"};
    for (size_t i = 0; i < sizeof(levels)/sizeof(levels[0]); i++) {
        SandboxConfig c = cfg_for(levels[i]);
        Containment p; memset(&p, 0, sizeof(p));
        containment_resolve(levels[i], &p);
        if (p.sandbox != c.sandbox || p.env_mode != c.env_mode ||
            p.net_mode != c.net_mode || p.mount_cwd != c.mount_cwd ||
            p.rlimits.nproc != c.rlimits.nproc ||
            p.rlimits.as_mb != c.rlimits.as_mb ||
            p.rlimits.cpu_sec != c.rlimits.cpu_sec) {
            printf("  row %s\n", levels[i]);
            assert(!"containment/policy field drift");
        }
        if (p.read_paths != NULL || p.write_paths != NULL ||
            p.read_path_count != 0 || p.write_path_count != 0) {
            printf("  row %s\n", levels[i]);
            assert(!"resolve touched grant-path fields");
        }
    }
}

/* The workspace is unconditionally writable now — HOME points at it, so
 * npm/cargo/go need it, and `workspace_ro` was deleted rather than left as a
 * field no containment sets. Nothing to assert: it is structural, not policy.
 * /tmp is likewise a parent-created host bind, not a containment field. */

int main(void) {
    TEST_INIT();
    RUN(test_none);
    RUN(test_standard);
    RUN(test_offline);
    RUN(test_fallthrough);
    RUN(test_resolve_matches_policy);
    return 0;
}
