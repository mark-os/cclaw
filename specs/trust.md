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
| **Sensitivity** | Is this *target* special regardless of grants? | `sensitive_targets` labels + `secret_hosts` bindings | dispatch gate + proxy deny |

Containment is kernel-enforced and coarse. Authority is additive: absence of a
grant is the denial. Escalation is per-call. Sensitivity is the one place a
label *subtracts* from standing authority, and it is exactly two rules — a
pair (known-sensitive vs unknown-plus-credential); neither may be
"simplified" away without the other:

1. **Sensitive targets always escalate** (`sensitive_targets`, operator-set
   via `cclaw sensitive`). Enforced twice, both fail-closed: the labels ride
   every network-tier call as proxy *deny-before-allow* rules (`host_decide`),
   so no grant makes a sensitive host ambiently reachable; and the dispatch
   gate scans raw tool args (`host_in_text`, registered-domain+subdomain,
   lookalike-robust) and parks any match (`action='sensitive'`). An approval
   is consumed per call — ALWAYS is coerced to ONCE in `resolve_approval`,
   and an approved call gets only a per-call egress exception.
2. **Unrecognized target + credential ⇒ escalate** (`secret_hosts`, seeded
   via `cclaw secret-bind` or accreted by "approve & bind"). A loaded secret
   with no bindings parks on first use; a url-carrying call parks unless the
   url host is covered by every used secret's bindings; and a shell/js call
   carrying secrets has its egress *narrowed to the union of the bound
   hosts* — the proxy enforces the binding at the actual connection, so an
   unbound secret means deny-all unless a human approved that exact call.
   ALWAYS on a url-carrying park records the binding; shell/js ALWAYS
   coerces to ONCE (no standing binding without an attributable target).

## The invariant

**Only sandbox setup reads `sandbox_profile`. Every authority decision reads
`grants`.**

`sandbox_policy_from_profile()` (`src/sandbox.c`) is the single consumer that
turns the profile string into a containment bundle. If a change makes tool,
secret, or host access depend on the profile, it is on the wrong axis — express
it as a grant. Concretely:

- Gating a privileged tool (e.g. a future `secret_store_write`) is a
  `grants` row, never a profile check or a magic profile value.
- `js_eval` today has exactly `web_fetch`'s reach (same proxy, same host
  rules, same sensitivity deny and credential narrowing), so its grant cost
  already tracks its reach. When `cclaw.exec` lands (JS→shell bridge), that
  stops being true — **`cclaw.exec` must require the agent to also hold the
  `shell_exec` grant**, so granting `js_eval` never silently grants the
  union. Grant cost tracks actual reach, always.
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
| Unwanted / sensitive actions | grants + approval gate + sensitivity labels | agent mistakes, injection |
| Credential to wrong host | secret_hosts bindings (park + egress narrowing) | injection-driven exfil, lookalike targets |
| System mutation (`sudo`, `-g` installs) | escalation gate (setup-time, human-present) | routine/injected host-touching attempts |
| Native code exploit at daemon privilege | prevention only | nothing post-compromise |

## Known limitations (do not paper over)

1. **The approval gate runs at the agent's own privilege.** It defends against
   mistakes and injection, not a subverted binary on a root box. Moving it to
   a separate lower-trust process is a real architectural cost, deliberately
   deferred.
2. **Lookalike-target phishing is mitigated, not solved.** The fail-closed
   credential rule raises the bar (a lookalike host is never bound, so the
   call parks); a convincing lookalike the user personally approves is beyond
   the system's reach.
3. **The sensitivity arg-scan sees what the model wrote, not what runs.** A
   shell command that computes a hostname at runtime evades `host_in_text` —
   and then hits the proxy deny layer, which is why that layer is the
   load-bearing one. The scan is the early, legible half; the proxy is the
   boundary.
4. **Monitor agents (if added) are defense-in-depth, not a boundary.** Veto or
   escalate only, never grant — an injectable LLM must not hand out
   permissions. Useful against injection, useless against native compromise.

## History

`agents.sandbox_profile` was named `trust_level` until 2026-07. The rename
records the resolution of a two-meanings ambiguity: the column only ever
selected the containment bundle, but the name invited authority checks to
accrete onto it. Design notes that referenced "trust levels" mean the
containment axis.
