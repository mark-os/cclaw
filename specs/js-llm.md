# llm() — LLM completions from the sandboxed JS tier

The `llm()` global lets JS-tier code (js_eval, extension tool handlers, cron
scripts) run one-shot completions against the agent's **own** model routing
list. It is the primitive that self-authored tools compose on — `web_search`
(extensions/gemini-search) is the reference example: grounded search in ~20
lines, no endpoint URL, no key, no `{{SECRET}}`.

## API

```js
llm(prompt)                       // -> completion text (string)
llm({messages:[{role,content}]})  // conversation form
llm(prompt, {
  system: "...",                  // prepended system message
  model: "gemini",                // substring-match against the routing list
  max_tokens: 1024, temperature: 0,
  timeout: 90,                    // secs per attempt (default 120, max 180)
  extra: {...},                   // merged into the provider request body last
  full: true                      // -> {text, model, id, status, body}
})
```

Throws on total failure with a per-candidate trail (`"m1: http 429 ...; m2:
timed out"`), and immediately when `opts.model` matches nothing — llm() can
only use models already routed to the agent.

## How it works

Everything is resolved in the **trusted parent** at dispatch
(`llm_wire_json`, llm_proc.c): the agent's `agent_models → models →
providers` join — the same loader and order that serves the agent's own
turns — rendered into candidate descriptors (resolved URL, complete auth
header, wire format, `request_extra` + reasoning-effort fragment pre-merged
as `extra`) and shipped in the run-tool blob (`RunToolReq.llm_json`). The
child (qjs_llm.c) walks the list in order: builds the body per wire format
(OpenAI chat completions or native Gemini `generateContent`), POSTs through
the same proxied HTTP path as `http_request`, and moves to the next
candidate on any failure. No DB, no routing policy, no key material handling
beyond setting a header.

Merge order for the body: cclaw-built fields ← candidate `extra`
(provider `request_extra` + effort) ← caller `opts.extra`. The caller wins,
same philosophy as `request_extra` itself.

## Trust boundaries

- **Authority is the routing list.** There is no separate grant for llm():
  an agent that can spend a model on its own turns can spend it from a tool.
  A model NOT on the agent's routing list is unreachable regardless of what
  the JS asks for.
- **Keys never enter JS-visible space.** Descriptors (auth headers included)
  live in C-held values, never attached to a global; a handler enumerating
  `globalThis` finds nothing (test: `no_key_leak_into_js`). They are in the
  child's process memory — same stance as JS-tier secret resolution;
  heap-avoidance is not a trust-model goal.
- **Each key travels only to its own provider.** The child sends the auth
  header only to that candidate's URL, and the blob is bzero'd after write
  like every secret-bearing blob.
- **Egress widening is deliberate.** The dispatch adds each candidate's host
  to the call's proxy allowlist, so any JS in that call can also reach
  provider hosts unauthenticated. Accepted: the agent already sends its whole
  context to those hosts every turn, so this adds no new disclosure class.
  Sensitive-host deny rules still win unconditionally.
- **Spend is bounded** by the tool timeout (each HTTP attempt ≤ 180s, the
  child dies at the tool's own timeout) and by candidate count
  (`LLM_WIRE_MAX_CANDIDATES`, 4). llm() calls are not archived in
  `llm_responses` and do not feed model health/degradation — the child has no
  DB; health still shapes what the *parent* ships, since the wire is built
  from the same loader. Known gap, revisit if llm()-heavy tools need
  `cclaw resp` debugging.

## Non-goals

- No streaming, no tool-calling loop inside llm() — one request, one text.
  A tool that needs an agentic loop should be a sub-agent, not a JS loop.
- No per-extension model config: model choice is `opts.model` against the
  agent's routing, so the operator's `agent_models` stays the single source
  of truth (models ≠ providers stance, config-ax).
