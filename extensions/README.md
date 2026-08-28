# Extension bundles

Extension bundles that ship with the source but are **not** embedded in the
binary. Nothing here is compiled or loaded automatically: a bundle becomes real
only when it is copied into an agent workspace and promoted, exactly as an
agent-authored one is. Keeping them here means a bundle survives a database
rebuild and gets reviewed like the rest of the tree, without growing the binary
for every deployment that does not want it.

Built-in extensions are the other thing — those live in `templates/` and are
compiled in (`templates/channel_telegram.qjs` and friends).

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
