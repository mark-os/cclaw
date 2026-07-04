# Trust Model — Axes, Boundaries, Threat Classes

The model doc: what we defend against, where each boundary actually lives, and
which mechanism owns which question. Companions: [security.md](security.md)
(secret handling, DLP, defense-in-depth), [sandbox-profiles.md](sandbox-profiles.md)
(the containment bundles), [egress-filter.md](egress-filter.md) (proxy host rules).

## The four axes

Every "can the agent do X?" question belongs to exactly one axis. Keeping them
separate is the design; conflating them is the failure mode this doc exists to
prevent.

| Axis | Question | Mechanism | Enforced by |
|------|----------|-----------|-------------|
| **Containment** | What can a tool child touch if it misbehaves? | `agents.sandbox_profile` → policy bundle | kernel (namespaces, rlimits, netns+proxy) |
| **Authority** | What is this agent allowed to do? | `grants` rows (kind/value, `approval_mode`, `expires_at`) | dispatch gate in the trusted process |
| **Escalation** | Does this call need a human right now? | `approvals` park/resolve (`rerun`/`apply`), `request_config` | dispatch gate + channel/CLI approver |
| **Sensitivity** *(future)* | Is this *target* special regardless of grants? | label on targets; `sensitive ⇒ always escalate` | dispatch gate |

Containment is kernel-enforced and coarse. Authority is additive: absence of a
grant is the denial. Escalation is per-call. Sensitivity (when built) is the
one place a label *subtracts* from standing authority — no standing grant
satisfies a sensitive-labeled target. It comes with exactly one sibling rule,
the fail-closed action default: *unrecognized target + credential about to be
submitted ⇒ escalate*. The two rules are a pair (known-sensitive vs
unknown-plus-credential); neither may be "simplified" away without the other.

## The invariant

**Only sandbox setup reads `sandbox_profile`. Every authority decision reads
`grants`.**

`sandbox_policy_from_profile()` (`src/sandbox.c`) is the single consumer that
turns the profile string into a containment bundle. If a change makes tool,
secret, or host access depend on the profile, it is on the wrong axis — express
it as a grant. Concretely:

- Gating a privileged tool (e.g. a future `secret_store_write`) is a
  `grants` row, never a profile check or a magic profile value.
- A future `create_agent` caps the child on both axes independently: child
  `sandbox_profile` ≤ parent's (the four values are ordered
  `host > trusted > standard > restricted` by looseness) AND child grants ⊆
  parent grants. Subset-of-grants, not a fuzzy "trust" comparison.
- The profile is a *creation-time preset* from the operator's point of view —
  pick one word, get a containment bundle plus default grants
  (`agent_grant_defaults()`). After creation the axes are edited
  independently; `search_config` reports both.

## Threat model — two classes, stated honestly

### Native compromise (RAT-class) — out of scope in-process

Binary exploited → arbitrary native code at the daemon's privilege. On the
common deployment (dedicated box, passwordless sudo or root) this is full
system compromise. Nothing in-process — including the approval gate, which
runs at the same privilege — defends against this. Mitigation is *prevention*:
minimal auditable C surface, untrusted input decoded only in disposable
sandboxed children, fail-closed at every boundary.

### Capability misuse (injection-class) — the primary threat

The binary runs exactly as built, but the *model* is manipulated — prompt
injection from a web page, a channel message, a poisoned search result — into
using its legitimately-granted tools against the user. The exploit is text;
the malicious action still flows through the dispatch gate. **This is the
threat the authority/escalation/sensitivity axes defend.** The approval gate
is load-bearing here and useless against native compromise; never let its
existence imply the stronger property.

## Deployment reality

cclaw targets dedicated boxes (Mac Minis, old laptops, SBCs, rooted phones)
where the daemon often runs as or can escalate to root. There, the kernel
sandbox is **not** a security boundary against an adversary already at the
daemon's privilege (ns-root can remount and tear down namespaces). What it
still provides, and why it stays mandatory (fail-closed, `host` excepted):

1. **Stability** — a misbehaving tool can't break the machine.
2. **Fail-closed egress** — empty netns + default-deny proxy is the floor.
3. **File/context separation** — the child sees only what was mounted; a
   malicious binary run *as a tool* is born into the jail (phones home only
   to allowlisted hosts, sees only the cordoned fs). This holds only if
   everything goes through the sandboxed tool path — which is the argument
   for never adding a host-shell convenience door.

The real security boundary on such a box moves up to the authority/escalation
layer. The kernel cannot enforce intent.

## Where each boundary lives

| Concern | Enforced by | Holds against |
|---------|-------------|---------------|
| Stability / machine breakage | namespaces + rlimits | misbehaving tools |
| Network egress | netns + proxy `host_decide()` (default-deny) | normally-running agent |
| File / context separation | mount ns (workspace + path grants only) | normally-running agent |
| Downloaded malware run as tool | sandbox | contained iff via the sandboxed tool path |
| Secrets at rest | ChaCha20 in DB; key on disk (ceiling), enclave/keychain provider (future) | DB-file exfiltration; not full-disk capture |
| Unwanted / sensitive actions | grants + approval gate (+ sensitivity, future) | agent mistakes, injection |
| System mutation (`sudo`, `-g` installs) | escalation gate (setup-time, human-present) | routine/injected host-touching attempts |
| Native code exploit at daemon privilege | prevention only | nothing post-compromise |

## Known limitations (do not paper over)

1. **The approval gate runs at the agent's own privilege.** It defends against
   mistakes and injection, not a subverted binary on a root box. Moving it to
   a separate lower-trust process is a real architectural cost, deliberately
   deferred.
2. **Lookalike-target phishing is mitigated, not solved.** The future
   fail-closed credential rule raises the bar; a convincing lookalike the user
   personally approves is beyond the system's reach.
3. **Monitor agents (if added) are defense-in-depth, not a boundary.** Veto or
   escalate only, never grant — an injectable LLM must not hand out
   permissions. Useful against injection, useless against native compromise.

## History

`agents.sandbox_profile` was named `trust_level` until 2026-07. The rename
records the resolution of a two-meanings ambiguity: the column only ever
selected the containment bundle, but the name invited authority checks to
accrete onto it. Design notes that referenced "trust levels" mean the
containment axis.
