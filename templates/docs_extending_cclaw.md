---
name: extending-cclaw
description: How to author CClaw extensions — manifest format, tool/skill/channel components, the draft → promote → publish → attach lifecycle, and QuickJS handler contracts.
---

# Extending CClaw

You can add new tools, skills, channels, and even agents to CClaw by
authoring an **extension**: a directory bundle in your workspace containing an
`extension.json` manifest plus handler files. Code lives in files; the database
stores only definitions and paths.

## Lifecycle

1. **Draft** — write the bundle under `workspace/extensions/<name>/`. Test tool
   handlers with `js_eval` first.
2. **Promote** (`extension_promote {name, bundle_dir}`) — validates the
   manifest and parks an operator approval enumerating what the bundle adds
   ("adds 2 tools, 1 skill…"). On approval it is installed into the shared
   store and its components registered. Your drafts never auto-promote.
3. **Publish** (`extension_publish`) — makes an installed extension attachable
   by other agents. You must own the extension. It is not a baseline grant:
   the default agent has it, other agents request it with `request_config`,
   and a grant of it can carry an approval gate (the `agent_approval_tools`
   config key).
4. **Attach** (`extension_attach`) — links an agent to a published extension
   (or one it owns) through a row in `agent_extensions`. Attached extensions
   load at agent startup (tools registered, skills indexed). Attaching never
   grants authority: the extension's tools still need the agent's own grants.

## Manifest (`extension.json`)

```json
{
  "name": "my-ext",
  "version": "0.1.0",
  "tools": [
    { "name": "weather_get", "description": "…",
      "parameters": { "type": "object", "properties": { "city": {"type": "string"} } },
      "handler": "tools/weather.qjs" }
  ],
  "skills":  [ "skills/my-skill" ],
  "scripts": [ { "name": "cleanup", "handler": "scripts/cleanup.qjs" } ],
  "hooks":   [ { "event": "Stop", "handler": "hooks/settled.qjs" } ],
  "config":  [ { "key": "api_base", "default": "https://…", "description": "…",
                 "secret": false, "required": false } ],
  "channel": { "type": "…", "handler": "channel.qjs" },
  "agents":  [ { "name": "Scout", "containment": "offline", "…": "…" } ]
}
```

All handler paths are bundle-relative; no `..`, no absolute paths. `config[]`
keys register as `<name>.<key>` in the config registry. `agents[]` entries use
the agent-definition schema (see the creating-agents skill); an entry whose name
already exists is skipped, never overwritten. A `hooks[]` entry runs its handler
when its `event` happens in a session of an agent the extension is attached to:

- `Stop` — a top-level turn ended with an answer (after the reply is out);
- `SubagentStop` — the same, in a sub-agent's session;
- `ContextThreshold` — an iteration ended (no call in flight) with the session
  over its trim threshold; `args.context` carries `tokens`, `window`,
  `trim_threshold` and `prune_target`, and every entry in `args` its
  `tokens`. If the session is still over the threshold afterwards, the
  runtime trims it itself (oldest turns first, down to `prune_target`).
  Every agent has one such hook already: the bundled extension `compaction`,
  which summarises the oldest part of the session with your own model. An
  agent without it (its attachment switched off) is on the runtime's trim
  alone; another extension's `ContextThreshold` hook may replace it.
- `CacheExpiring` — the session has sat idle for 90% of its model's prompt
  cache lifetime (`cache_lifetime` on the agent's first chat model; none set,
  never) and is above its `prune_target`: the next request will pay full
  price for the whole session, so this is the cheap moment to shrink it.
  Once per idle period, in any session (sub-agents too). `args.context`
  carries what `ContextThreshold` gets plus `depth` (0 = top level),
  `cache_lifetime`, `idle_seconds` (since the last answer came back) and that
  answer's `cache_read_tokens` / `cache_write_tokens` (null when not
  reported). Nothing follows it: no edits or a failed run leave the session
  as it was. The bundled `compaction` subscribes too: top-level sessions
  only, down to half the session's size or the prune target, whichever is
  lower.

The entry takes `event`, `handler`, `input` (below), `async` and `timeout`,
nothing else. The session waits for the script (at most `timeout` seconds,
default config `hook_timeout`) and its JSON reply may edit the session — any
turn of it, including the one still running when a `ContextThreshold` script
runs between iterations (a message arriving meanwhile waits for the script); with `"async": true` it is not waited for and its
reply is ignored. Three failures in a row (crash, timeout, refused reply)
disable the hook until the extension is promoted again (or the operator
clears `hooks.disabled`). The reply:

```json
{"edits":[{"op":"drop","call_id":"c4"},
          {"op":"replace","call_id":"c1","text":"the 5 lines that mattered"},
          {"op":"note","text":"dropped the empty search"}],
 "record":{"any":"bookkeeping of yours"}}
```

| op | fields | does |
|----|--------|------|
| `drop` | `call_id`, `turn_id`? | the call and its result leave |
| `replace` | `call_id`, `turn_id`?, `text` | a new result in the old one's place |
| `rewrite` | `entry_id`, `text` | new text for a user or assistant message, a note or a summary |
| `rewrite` | `call_id`, `turn_id`?, `args` | new arguments (an object) for a call |
| `note` | `text`, `before`? | a note before an iteration (`before`: its `entry_id`); default before the answer, or at the end while the turn runs |
| `drop_turns` | `from_turn`, `to_turn` | a run of whole turns before the newest leaves |
| `summarise` | `text`, `from_turn`, `to_turn` | a run of whole turns before the newest is replaced by your text |
| `summarise` | `text`, `from_iteration`, `to_iteration` | a run of whole iterations of the newest turn (never its answer's) is replaced by your text; the notes and summaries standing right before `from_iteration` (in `notes`, `before_iteration` = `from_iteration`) leave with it, so fold their text into yours |

A `call_id` is looked up in the newest turn unless `turn_id` names another
(providers reuse call ids across turns). `args` lists the newest turn's
iterations (`iteration_id`, `entry_id` = its first entry, `message_id` = its
assistant message, `text`, `calls`, and `reasoning` — the model's summary of
its reasoning for that response, present only when one was kept: see
`reasoning_display` on the routing entry; never the raw thinking or its
signature) with each call's `call_id`, `entry_id`,
`tool`, `args`, `protected` flag and verbatim `result.text`; `user_messages`
and `notes` (a summary among them has `"summary": true`) with their
`entry_id`; the answer as `final` (null while the turn is still running; it
carries `reasoning` the same way); and
every entry its `tokens`. By default that is all: the newest turn. To read the
conversation before it, the entry declares
`"input": {"history": 5}` (the last five turns) or `{"history": "all"}`; add
`"history_results": false` to get earlier tool results without their text,
and `"max_bytes"` (default 1048576, from 1024 to 16777216) to size the whole input
— when history does not fit, its oldest turns are left out and
`args.history_omitted` says how many. `args.history` is then oldest first:
each earlier turn in the same shape (`turn_id`, `user_messages`,
`notes`, `iterations`, `final`, `tokens`), and `{"summary":{"entry_id","text","tokens"}}`
where a summary stands between turns; `args.input` echoes what was asked for.

The runtime checks structure, never content: a call leaves only with its
result (an iteration left with no call leaves whole); a call still pending
or running cannot leave, not even with its turn; a `protected` call (one made
for its effect, `check_session`, or one that saved a secret) cannot be
dropped on its own, but may be rewritten and may leave with its whole turn or
iteration; the newest turn's answer is never edited; each entry is named by
one edit at most. Originals are never deleted — the runtime records each
edit. Text containing a secret refuses the whole reply; any refusal leaves
the session untouched. Return `"{}"` to leave it alone. The model sees a note
as `[note from extension <name>] …` and a summary as `[Summary of earlier
conversation] …`. On Anthropic, an edit inside the running turn makes the
model re-think from that point (its earlier reasoning is not replayed after
the edit); other providers keep their reasoning as it was. On every provider
the prompt cache restarts at the first changed entry, so edit in one
worthwhile pass.

## Handler contract (QuickJS)

Tool handlers are modern (ES2023-class) QuickJS scripts run in a sandboxed
engine. The handler file is evaluated as a **function body** with the call's
parsed arguments in scope as `args` — roughly `(function(args){ <file> })(args)`
— so `return` your result string from the top level (a bare trailing expression
evaluates to `undefined`). If nothing is returned, buffered `console.log`
output becomes the result. No Node APIs, no `require`/`import`, no event loop,
no `Atomics`; use the provided host bridges:

- `http_request(url[, {method, body, headers:{Name:Value}, markdownify, timeout, save_to, max_bytes}])` —
  synchronous HTTP, returns `{status, content_type, body}` (and
  `truncated: true` when the body passed 2 MB and was cut); on transport
  failure it returns `{status: -1, body: "", error}` instead of throwing
  (`body` is always a string) — when your host grants do not cover the URL,
  `error` names the grant to request and `denied` is `true`. A `User-Agent`
  header replaces the default `cclaw/1.0`. `markdownify: true` converts an HTML
  body to markdown for readability — it is a rendering aid, not a security
  boundary (network results are wrapped as untrusted content when sent to the
  model regardless).
  `timeout` is seconds, default 30, capped at 180 — the fetch blocks the whole
  tool child, so the cap is what stops a slow endpoint becoming a hang. Raise
  it only for calls that are legitimately slow (an LLM-backed search that
  grounds a query can take a minute from a small box), not as a blanket retry.
  `body` is text only — a binary response (image, PDF, archive) throws rather
  than hand you a corrupted string. Download those with `save_to: 'name.jpg'`:
  the bytes stream straight to that file and the call returns
  `{status, path, bytes}`. `max_bytes` caps the file (default 64 MiB); a
  non-2xx answer leaves no file and returns the error text in `body`.
- `fs.readFile` / `fs.writeFile` / `fs.readdir` / `fs.stat` / `fs.lstat` /
  `fs.cwd` — workspace-scoped file access.
- `LLM(prompt[, {system, purpose, model, max_tokens, temperature, timeout, images}])` — a
  completion from your own model routing list, returned as a string. Only
  what your handler returns enters your context, so you can map it over data
  or chain calls without spending context on the intermediate results.
  `purpose: 'utility'` uses your utility list — the cheaper models your
  operator set aside for side work — and falls back to your chat list when
  you have none; the only other value is `'chat'`, the default.
  `images: ['photo.jpg']` attaches image files (JPEG, PNG, GIF, WebP) for the
  model to look at — this is how you see a picture you downloaded. Pick a
  model that can see with `model`.
- `HTML.toMarkdown(str)` — convert an HTML page to markdown (scripts and
  styles dropped, headings, lists and links kept). The converter `web_fetch`
  uses; `web_fetch` itself is a shipped extension built on these globals
  (`extensions/web-fetch/fetch.qjs` in the cclaw source).
- `XML.parse(str)` (alias `xml.parse`) — parse XML/RSS/Atom/sitemaps into a
  plain object, the JSON.parse of XML. Reach for it instead of regex.

  ```js
  var d = XML.parse(http_request(feedUrl).body);
  var items = d.rss.channel.item;              // array when repeated, object when single
  return items.map(function (it) { return it.title + ' — ' + it.link; }).join('\n');
  ```

  The shape follows the fast-xml-parser convention: element names become keys,
  attributes become `@_name` keys, a text-only element collapses to a plain
  string, and an element with both text and children keeps its text under
  `#text`. **A repeated element is an array but a single one is not** — write
  `[].concat(d.rss.channel.item)` when a feed might carry only one entry.

  Entity handling is deliberately forgiving, because real feeds are not
  well-formed: the five XML built-ins and numeric references decode, the common
  HTML names (`&nbsp;` `&mdash;` `&rsquo;` …) decode, and anything else —
  including a bare `&` in `AT&T` — survives as literal text rather than failing
  the document. Inside CDATA and comments nothing is rewritten.

  Nodes have **no prototype**, so an element named `constructor` or `toString`
  is data like any other; use `Object.keys(node)` rather than
  `node.hasOwnProperty(...)`.

  Three more things worth knowing before you match on a value. **Values are
  always strings** — `<count>3</count>` gives `"3"`, not `3` (fast-xml-parser
  would coerce it; we do not), so compare with `Number(x)` or `===  "3"`.
  **Namespace prefixes are kept verbatim and never resolved**, so `xmlns`
  declarations are ignored and you match `d.rss.channel['media:content']` by
  the literal prefix the feed used — which is what feed vocabularies
  (`media:`, `dc:`, `content:`) expect anyway. And a **repeated attribute
  keeps the last occurrence** (`<r a="1" a="2"/>` → `@_a: "2"`), silently;
  that input is malformed XML, and this matches fast-xml-parser.

  Failures throw: a `SyntaxError` naming the defect ("close tag does not match
  the open tag", "document ended mid-element (truncated?)") or an
  `InternalError: out of memory` when the document is too big for the heap.
  The tree costs roughly **3× the document**, so the 8MB default heap
  (`js_heap_mb`) takes documents up to ~3MB; nesting is capped at 128 levels.
  Past that, fetch a narrower feed — the OOM message quotes the live limit.
- `getConfig(key)` — this extension's own `config[]` value (registry key
  `<name>.<key>`), string or `null`, read-only. The twin of `channel.getConfig`
  on the channel surface. Values are resolved before the sandbox is entered;
  secret keys are excluded — put `{{SECRET:NAME}}` in the config value and it
  arrives interpolated.

Keep handlers small and side-effect free where possible — network egress
still goes through the egress proxy and your host grants.

## Skills

A skill is a directory with a `SKILL.md` whose frontmatter has `name:` and
`description:`. Only the index (name + description) enters your context;
read the full SKILL.md with `file_read` when the task matches. Personal skills
in `agents/<you>/skills/` shadow extension skills of the same name.
