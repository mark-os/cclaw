# Egress Filter Model — Host Policy at the Proxy

Status: spec. Sits under the **web** and **shell** tiers. Both reach the
network only through the proxy (netns → UDS / loopback CONNECT shim → proxy is
the sole route). The proxy is therefore the single, unbypassable chokepoint
where every outbound connection — across every redirect hop and every
federated-login fan-out — is decided.

This note specifies the decision function behind the proxy's per-connection
check. It **replaces** today's exact-string `host_allowed()` with a staged
pipeline and a richer rule grammar. It does **not** change the proxy's
architecture, lifetime, bless set, or resolve-then-dial-literal anti-TOCTOU
behaviour — those are already correct and are reused as-is.

---

## 1. Enforcement point (unchanged)

- `proxy.c` `handle_client()`, per `CONNECT` / `RESOLVE`, keyed on
  `(host, port)`.
- Sole egress route: the netns gives sandboxed children no other path out, so
  a program cannot route around the decision.
- The decision function (`decide()`) replaces the current `host_allowed()`
  exact-match call sites.

What stays exactly as it is today:

- **Deny-by-default.** An empty / NULL allowlist denies every host. No network
  rather than allow-all.
- **resolve_and_bless / bless set / numeric-CONNECT binding.** A hostname is
  resolved, survivors are blessed with a TTL, and a numeric CONNECT only
  succeeds against a blessed IP. This closes the resolve→connect TOCTOU and
  DNS-rebinding window and is **not** modified — the new pipeline feeds it.
- **Per-call proxy lifetime, single-threaded bind-before-fork.** Untouched.
- **The UDS preamble + net_shim CONNECT path.** Untouched — only the decision
  behind `host_allowed()` changes.

---

## 2. Decision pipeline

`decide(agent_ctx, host, port) → ALLOW | DENY`

Evaluated as an ordered pipeline. **First failing stage wins. Default is
DENY.** Flat deny-vetoes — no most-specific-wins arbitration (see §5 for why).

```
Stage 1 — GLOBAL HOST DENYLIST            [floor, absolute]
    host matches any global deny rule → DENY.
    Agent grants CANNOT lift this. Operator-controlled, process-wide.

Stage 2 — AGENT HOST ALLOWLIST            [default-deny gate]
    host matches no agent allow rule → DENY.

Stage 3 — AGENT HOST DENYLIST             [intra-agent carve-out, optional]
    host matches an agent deny rule → DENY.
    Safe carve-outs live here (allow .example.com, deny tracking.example.com).
    No global floor to protect at this layer, so deny-vetoes is fine.

Stage 4a — PORT CHECK                      [optional]
    rule pins a port (e.g. :443) and the dialed port differs → DENY.

Stage 4b — RESOLVE + IP RANGE CHECK        [SSRF / rebind floor]
    resolve host; for each candidate IP apply §4 IP rules.
    No permitted IP survives → DENY. Survivors are blessed (existing TTL
    mechanism). Numeric CONNECT must hit a blessed IP (existing).

Otherwise → ALLOW.
```

"Can agent X reach host H" is answered by walking these four stages and ANDing
the survivors. No specificity tournament, no cross-source ranking.

---

## 3. Rule grammar (minimal — four kinds)

Applies to **allow** lists (agent) and **deny** lists (global + agent).

| Kind          | Example                  | Matches                                            |
|---------------|--------------------------|----------------------------------------------------|
| exact host    | `api.github.com`         | `host == "api.github.com"`                          |
| suffix        | `.github.com`            | `host == "github.com"` OR `host` ends `.github.com`|
| wildcard label| `*.cdn.example.com`      | single-label glob (OPTIONAL — add only if suffix insufficient) |
| CIDR          | `10.0.0.0/8`, `::1/128`  | IP rules; used by Stage 1 (literal-IP hosts) and Stage 4b (resolved IPs) |

Suffix matching MUST be public-suffix aware: a suffix rule may not span a
public-suffix boundary (no `.co.uk` matching everything). See OPEN Q1 on PSL
footprint for ARMv5TE.

**Suffix is the primary grammar.** Federated-login fan-out is almost always
subdomain fan-out, which `.domain` covers without enumerating every host. A
real workflow grant is typically a small set of suffix rules
(`.github.com`, `.githubusercontent.com`, the IdP) — multiple `kind='host'`
grant rows.

---

## 4. IP range rules (Stage 4b) — the CIDR-grant model

Private / catastrophic ranges are reachable **only deliberately**, never
implicitly via resolution or redirect. The threat is not "this IP is
reachable" — it is "this IP is reachable without anyone having decided it
should be" (SSRF, DNS rebinding, malicious redirect).

Two legitimate access patterns are supported (these are the real operator
use cases; the dangerous third pattern is excluded):

1. **Direct literal IP in a granted CIDR.** Operator grants `10.0.0.0/8` (or
   `192.168.1.0/24`); the agent dials `192.168.1.50` directly; it is in range
   → permitted + blessed. ("I go to those local IPs directly.")
2. **Internal hostname resolving into a granted CIDR.** `something.local`
   resolves via the operator's own resolver to `10.x` → permitted + blessed.
   ("I have local DNS pointing at internal hosts.")

The **excluded** pattern — the only one that is an attack and is never a real
use case: an *external, public-DNS-resolved* allowlist hostname
(`api.vendor.com`) resolving into a private range. See the resolver-trust
caveat below.

### Decision for a resolved / dialed IP

```
addr_permitted(agent_ctx, ip):
    if ip in METADATA_RANGE (169.254.0.0/16, v6 equivalents):
        # catastrophic, no legitimate "naming" case — literal-only, always
        return (ip explicitly granted as a literal to this agent)
    if ip is public (not private/loopback/link-local/ULA):
        return ALLOW
    # ip is private/loopback/etc:
    if ip in an agent-granted CIDR:        return ALLOW   # patterns 1 & 2
    if ip explicitly granted as a literal: return ALLOW
    return DENY
```

- A granted CIDR widens both literal dials and hostname resolutions into that
  range. That widening is the ergonomic feature (patterns 1 & 2).
- **Metadata range is carved out**: never reachable via a CIDR grant or via
  resolution. Only an explicit literal grant for the specific metadata IP
  works. There is no `something.local`-style legitimate naming case for
  `169.254.169.254`, and the downside (cloud IAM credential theft) is
  categorically worse than reaching an internal service.
- Default-deny is preserved: no CIDR grant → no private access at all, resolved
  or literal.

### Resolver-trust caveat (document, do not silently default)

Granting a CIDR to an agent that **also** has public, externally-resolved
allowlist entries means those public hostnames could, in principle, resolve
into the granted private range (DNS rebinding). This is:

- **A non-issue for the homelab / own-resolver deployment** (the target case):
  you control DNS for `.local`; the attack requires untrusted control of the
  resolution for a name you explicitly trusted, which does not hold.
- **A real consideration for an agent pointed at public SaaS** with a broad
  CIDR grant.

The matcher does not pick for either deployment. The widening (hostname →
granted CIDR) is documented so each operator chooses knowingly. The metadata
carve-out is non-negotiable regardless of deployment.

---

## 5. Why flat deny-vetoes, not most-specific-wins

- Stage 1 (global denylist) must be an **absolute floor** that agent grants
  cannot override. A longest-match model would let a narrow agent allow outrank
  the floor, forcing a special-case to protect it anyway → two precedence
  models in one engine, harder to audit.
- Flat ordered pipeline keeps the floor absolute **by construction** and keeps
  the audit story a readable walk of four stages.
- The expressiveness given up (broad-deny-with-narrow-allow-exception across
  sources) is unnecessary under default-deny: you build up from specific
  allows rather than carving down from broad denies. Intra-agent carve-outs
  that ARE safe (allow `.example.com`, deny `tracking.example.com`) are handled
  by Stage 3, where there is no cross-source floor to protect.

---

## 6. Data model

| Concern                         | Where                                                  |
|---------------------------------|--------------------------------------------------------|
| Agent allow (Stage 2)           | `grants`, `kind='host'` (exists) — exact/suffix/wildcard/CIDR values |
| Agent deny (Stage 3)            | `grants`, `kind='host_deny'` (new)                     |
| Global host floor (Stage 1)     | process-wide config, loaded once at daemon start, passed to every `proxy_bind` (NOT per-agent, NOT in `grants`) |
| IP range floor (Stage 4b)       | the `addr_permitted` logic above + the metadata carve-out |

`caps->hosts` already loads `kind='host'` grants, expiry-filtered. Add
`caps->host_deny[]` alongside. `ProxyContext` gains borrowed pointers to the
global floor lists and the agent deny list (same "must outlive the proxy"
borrow contract as `hosts[]` today; the broker holds no DB handle).

### Floor seeding vs. structural guarantee

The global floor and the catastrophic-range protection are **two different
things** and must not be conflated:

- **Seeded defaults** (global host denylist, default-denied CIDR ranges) live
  in a table for auditability and operator extension. These are *defaults* —
  operators may extend them. Deleting a seeded deny row fails *safe* in the
  sense that it widens policy only as far as the structural guarantee below
  still allows.
- **Structural guarantee** (the metadata-range literal-only rule + the
  resolution-cannot-reach-private-without-grant rule) lives in `decide()` /
  `addr_permitted`, in code, not as a deletable row. This is the line between
  "this deny is policy" (table, extensible) and "this deny is structural"
  (code, holds regardless of table contents). A seed alone is a default, not a
  floor; the structural rule is what makes catastrophic access require a
  deliberate grant even if the table is edited.

---

## 7. Matcher shape

```
host_match(rules[], n, host)  → bool   # exact | suffix | wildcard
cidr_match(cidrs[], n, ip)    → bool   # Stage 1 literal-IP host + Stage 4b
decide(agent_ctx, host, port) → ALLOW | DENY    # composes per §2 pipeline
```

`http_is_private_ip` is reframed as the baked default CIDR floor list, so the
"private IP" knowledge becomes auditable data rather than a hardcoded
predicate — but the metadata carve-out and the
resolution-cannot-reach-private rule remain in code (§6 structural guarantee).

---

## 8. What web_fetch gains (and deletes)

Wrapping `web_fetch` into the netns/proxy means its egress is decided by the
same `decide()` across every redirect hop. Therefore:

- `web_fetch`'s separate pre-flight `http_check_policy(url)` is **subsumed** and
  can be removed — a single pre-flight URL check cannot see redirects; the
  proxy sees every hop.
- The federated-login / cross-host redirect reality is handled for free: each
  hop is just another connection the proxy approves against the agent's grants.

---

## 9. Status and open questions

### Landed (2026-06)

- **§4 IP range model + §6 structural guarantee**: implemented in
  `src/proxy.c` (`addr_permitted`, `host_decide`) and `src/host_match.c`
  (`cidr_parse`, `host_match`, `cidr_match`, `granted_exact`). Grants are
  partitioned into hostname rules and CIDR rules at `proxy_bind` time.
- **Metadata carve-out** (`169.254.0.0/16`, `fe80::/10`, `fd00:ec2::254`):
  enforced on both the numeric-CONNECT path (`host_decide`) and the
  RESOLVE path (`addr_permitted`). IPv4-mapped IPv6 (`::ffff:x.x.x.x`) is
  canonicalized to its v4 form before any check, closing the mapped-address
  bypass.
- **Suffix matching** (`.github.com` → exact base + dot-boundary suffix),
  case-insensitive. No PSL yet — open Q1.
- **CIDR grants for private ranges** (patterns 1 and 2 from §4): working.
- **Stage 2** (agent allowlist) is the sole implemented stage. Stages 1, 3,
  4a are deferred.

### Still open

- **Q1 — PSL footprint on ARMv5TE.** Full Public Suffix List vs. a minimal
  built-in ICANN suffix set. Decide before implementing suffix matching;
  document the limitation if shipping the minimal set.
- **Q2 — Global floor storage + extension API** (Stage 1). Table + seed,
  with the structural guarantee (§6) in code. Confirm the seed/extend path
  is not reachable by any agent-facing write path (`config-write`,
  write-capable `db_query`), so an agent cannot widen its own egress floor.
- **Q3 — Agent denylist** (Stage 3). Not yet wired in; `ProxyContext` gains
  `host_deny[]` alongside `hosts[]` when this lands.
- **Q4 — Port pinning** (Stage 4a). Per-rule (`api.github.com:443`) or a
  global default-allowed-ports floor?
- **Q5 — Wildcard-label rules.** Needed, or does suffix cover every real
  case? (Suffix handles `.github.com` fan-out; wildcard only earns its
  place for mid-label matching.)
