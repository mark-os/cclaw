# LLM() — completions from the sandboxed JS tier

The `LLM()` global lets JS-tier code (js_eval, extension tools, cron scripts)
run completions against the agent's **own** model routing list. It is a plain
function, and that is the point: compose it — map it over data, use it as a
classifier, chain models — and only what your code *returns* enters the
agent's context; intermediate calls burn provider tokens but not context.
`web_search` (extensions/gemini-search) is the reference example: grounded
search in ~20 lines, no endpoint URL, no key, no `{{SECRET}}`.

Design doc: `plan/projects/llm-core.md` (untracked). LLM is a callable
namespace (all-caps, like `XML`): future shapes — `LLM.embed`,
`LLM.transcribe`, `LLM.batch` — hang off the same global as the core grows
them.

## API

```js
LLM(prompt)                       // -> completion text (string)
LLM({messages:[{role,content}]})  // conversation form
LLM(prompt, {
  system: "...",                  // prepended system message
  model: "gemini",                // substring-match against the routing list
  max_tokens: 1024, temperature: 0,
  timeout: 90,                    // secs per attempt (default 120, max 180)
  extra: {...},                   // merged into the provider body last
  full: true                      // -> {text, model, id, status, body}
})
```

Throws on total failure with a per-candidate trail (`"m1: http 503 …; m2:
timed out"`); immediately when `opts.model` matches nothing — LLM() can only
use models already routed to the agent; and when the per-call cap is hit
(config `llm_max_calls_per_tool`, default 25, 0 = unlimited).

## How it works

The child holds **no key, no URL, no routing knowledge** — `qjs_llm.c` is a
marshaling stub. It ships `{messages, opts}` (length-prefixed JSON, one
request per connection) over a per-call UDS the dispatching parent serves
(`llm_bridge.c`, `<agent-dir>/.llm.<pid>.<seq>.sock`, bind-mounted into the
sandbox as `$CCLAW_LLM_SOCK`). The parent (`llm_request`, llm_proc.c) does
everything:

- **Routing**: the agent's `agent_models → models → providers` join — the
  same loader and order that serves the agent's own turns. Walks the ladder
  on failure; actively-degraded models are sidelined but the walk is never
  emptied; success/failure feeds the same health counters as turn traffic.
- **Body in SQL**: one prepared statement per wire format (OpenAI chat /
  native Gemini `generateContent`), llm_payload.c style. Merge order:
  cclaw-built fields ← provider `request_extra` + effort fragment ← caller
  `opts.extra` (caller wins). RFC-7386 `json_patch` null-removal is what
  makes optional fields disappear.
- **Accounting**: gated by `token_rate_limit`; every attempt archived to
  `llm_responses` stamped with the calling session/iteration and a source
  label (`jsllm_ok js:web_search`, `jsllm_http_429 cron:digest`) — `cclaw
  resp` shows them like any other row.
- **Keys**: resolved at call time (env or `scope='system'` secrets, same
  rule as turn routing), used for one request, wiped. They exist only in the
  parent process.

The per-call socket is the identity: the parent knows which tool call (and
therefore which session/agent) each connection belongs to without the
sandboxed child asserting anything.

## Trust boundaries

- **Authority is the routing list.** No separate grant: what an agent can
  spend on its turns is exactly what its tools can spend. `opts.model`
  narrows within the list and can never widen it.
- **Children never see keys.** Not "held C-side" — absent from the process.
- **No egress widening.** The child's allowlist is untouched; completions
  never leave the parent. Profiles with `net_mode=1` (restricted) get no
  bridge socket at all — no packets on the child's behalf, whoever sends
  them.
- **Spend is metered**: `llm_max_calls_per_tool` per call,
  `token_rate_limit` globally, the tool timeout as the outer wall-clock
  bound.

## Non-goals

- No streaming, no tool-calling loop inside LLM() — one request, one text.
  A tool needing an agentic loop should be a sub-agent, not a JS loop.
- No per-extension model config: model choice is `opts.model` against the
  agent's routing, so the operator's `agent_models` stays the single source
  of truth.
- No concurrent calls — `LLM()` blocks inside the native function, so
  `Promise.all` over it is legal and correct but executes serially.
  Deliberate: no promise lifetimes held C-side, no interleavings to get
  wrong; the failure mode is slowness against the (caller-raisable) tool
  timeout, never a wrong answer. Revisit only when a real extension needs
  fan-out (mapping `LLM()`/`http_request` over a list, where serial
  wall-clock = N × latency — e.g. grounded Gemini calls run ~48s each).
  The known design when that day comes is a two-layer promise bridge
  (seen in `reference/pi_agent_rust`): the native side returns an opaque
  call_id immediately and a small JS shim owns the pending-promise map, so
  completions arrive as plain data over the existing per-call socket and
  all promise lifetime management stays in JS where GC handles it — C
  never holds a JSValue across an await.
