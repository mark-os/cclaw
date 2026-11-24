/* Unit tests for sandbox_policy_from_profile / sandbox_profile_resolve: the
 * profile-name → containment-bundle mapping (specs/sandbox-profiles.md).
 * Pure functions, no namespaces required. Locks in the fall-through rule:
 * unknown/NULL profiles get the standard bundle, never something looser. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "sandbox.h"
#include "test_util.h"

static SandboxConfig cfg_for(const char *profile) {
    SandboxConfig c; memset(&c, 0xAA, sizeof(c)); /* poison: catch unset fields */
    sandbox_policy_from_profile(profile, &c);
    return c;
}

static void test_host(void) {
    SandboxConfig c = cfg_for("host");
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

static void test_restricted(void) {
    /* What makes restricted restricted is net_mode=1 (empty netns, no proxy
     * socket). The workspace stays rw so $HOME works, and the limits are a
     * runaway backstop that still permits a real build. */
    SandboxConfig c = cfg_for("restricted");
    assert(c.sandbox == 1 && c.env_mode == 1 && c.net_mode == 1 &&
        c.mount_cwd == 0 &&
        c.rlimits.nproc == 64 && c.rlimits.as_mb == 0 && c.rlimits.cpu_sec == 120);
}

static void test_fallthrough(void) {
    /* Unknown values must land on standard — a typo'd profile, or a retired
     * one ('bootstrap', 'trusted'), must never yield a looser bundle than
     * standard. 'trusted' is deleted: a DB row still carrying it (the schema
     * patch rewrites them, but a hand-edited row could) resolves to standard,
     * which is strictly tighter — clean env instead of inherit+scrub. */
    const char *unknowns[] = {NULL, "", "bootstrap", "trusted", "TRUSTED",
                              "hosT", "yolo"};
    SandboxConfig std = cfg_for("standard");
    for (size_t i = 0; i < sizeof(unknowns)/sizeof(unknowns[0]); i++) {
        SandboxConfig c = cfg_for(unknowns[i]);
        if (memcmp(&c, &std, sizeof(c)) != 0) {
            printf("  row '%s'\n", unknowns[i] ? unknowns[i] : "(null)");
            assert(!"unknown profile must resolve to the standard bundle");
        }
    }
}

static void test_profile_resolve_matches_policy(void) {
    /* sandbox_profile_resolve copies the policy half field-by-field (the
     * structs differ, so no memcmp across them) — pin each field instead. */
    static const char *profiles[] = {"host", "standard", "restricted"};
    for (size_t i = 0; i < sizeof(profiles)/sizeof(profiles[0]); i++) {
        SandboxConfig c = cfg_for(profiles[i]);
        SandboxProfile p; memset(&p, 0, sizeof(p));
        sandbox_profile_resolve(profiles[i], &p);
        if (p.sandbox != c.sandbox || p.env_mode != c.env_mode ||
            p.net_mode != c.net_mode || p.mount_cwd != c.mount_cwd ||
            p.rlimits.nproc != c.rlimits.nproc ||
            p.rlimits.as_mb != c.rlimits.as_mb ||
            p.rlimits.cpu_sec != c.rlimits.cpu_sec) {
            printf("  row %s\n", profiles[i]);
            assert(!"profile/policy field drift");
        }
        if (p.read_paths != NULL || p.write_paths != NULL ||
            p.read_path_count != 0 || p.write_path_count != 0) {
            printf("  row %s\n", profiles[i]);
            assert(!"resolve touched grant-path fields");
        }
    }
}

/* The workspace is unconditionally writable now — HOME points at it, so
 * npm/cargo/go need it, and `workspace_ro` was deleted rather than left as a
 * field no profile sets. Nothing to assert: it is structural, not policy.
 * /tmp is likewise a parent-created host bind, not a profile field. */

int main(void) {
    TEST_INIT();
    RUN(test_host);
    RUN(test_standard);
    RUN(test_restricted);
    RUN(test_fallthrough);
    RUN(test_profile_resolve_matches_policy);
    return 0;
}
