# Extension bundles

Extension bundles that ship with the source. Most are **not** embedded in the
binary: a bundle becomes real only when it is copied into an agent workspace
and promoted, exactly as an agent-authored one is. Keeping them here means a
bundle survives a database rebuild and gets reviewed like the rest of the tree,
without growing the binary for every deployment that does not want it.

The exception is a bundle listed in `SHIPPED_BUNDLES` in the Makefile —
today only [compaction](compaction/README.md). Its files (not its README) are
compiled in, and every start installs them into the shared store with no
owner, like the built-in channels in `templates/` (`channel_telegram.qjs` and
friends). Editing such a bundle here changes the next build.

## gemini-search

A `web_search` tool: asks Gemini a question with Google Search grounding
enabled and returns the answer plus the sources it actually used.

Two details worth knowing before changing it. **Grounding quota is granted
per-model and is much narrower than plain generation quota** — as of 2026-08 a
free-tier key serves grounded requests only on `gemini-2.5-flash`, while newer
flash models happily answer ungrounded and return 429 the moment
`google_search` is attached. Hence `models` is a preference list that falls
through on 429; reorder it when the free tier widens. And **a grounded call is
slow** — 48s measured from a Pogoplug, because the search runs and then a model
reads the results — so the handler passes `timeout: 90` rather than relying on
the 30s default.

Install (as the operator):

```sh
cp -r extensions/gemini-search ~/.cclaw/extensions/
cclaw secret set GEMINI_API_KEY                 # reads one line from stdin
cclaw secret-bind GEMINI_API_KEY generativelanguage.googleapis.com
```

then register the extension, tool, and `gemini-search.models` config, and grant
`web_search` to the agent that should have it. The tool declares
`hosts: ["generativelanguage.googleapis.com"]`, which *replaces* the agent's
host grants for its own calls — so granting it adds search without opening
general egress.

## turn-review

A `Stop` hook: after a turn ends, a utility model picks the tool calls
later turns no longer need. Dead-end calls are dropped, long results are cut
to the lines that mattered, and one short note says what the removed material
established. The model only selects — the script builds each excerpt from the
original text, and the originals stay in the database. It is the reference
example for [hooks](../specs/hooks.md) and for `LLM(…, {purpose: "utility"})`;
attached by hand, never a default. Install, model routing, settings and how to
see what it did are in [turn-review/README.md](turn-review/README.md).

## compaction

The one bundle on by default: a `ContextThreshold` and `CacheExpiring` hook.
When a session grows past its trim threshold, the oldest part of it — whole
earlier turns first, then the oldest steps of the turn still running — is
replaced by a summary the agent's own model writes, with the actions already
taken listed under it by the script. When an idle top-level session's prompt
cache is about to lapse, it is summarised down to half its prune target the
same way. Without it the runtime's trim drops the oldest part with a one-line
note when the threshold is passed, and an idle session is left alone. It is
the reference example for both events; switching it off for an agent, or
replacing it, is in [compaction/README.md](compaction/README.md).
