---
name: extending-cclaw
description: How to author CClaw extensions — manifest format, tool/hook/skill/channel components, the draft → promote → publish → attach lifecycle, and QuickJS handler contracts.
---

# Extending CClaw

You can add new tools, hooks, skills, channels, and even agents to CClaw by
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
   by other agents. No approval needed — you must own the extension, and
   attaching never grants authority by itself.
4. **Attach** — rows in `agent_extensions` link agents to extensions; attached
   extensions load at agent startup (tools registered, skills indexed).

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
  "hooks":   [ { "event": "session_start", "handler": "hooks/hello.qjs" } ],
  "skills":  [ "skills/my-skill" ],
  "scripts": [ { "name": "cleanup", "handler": "scripts/cleanup.qjs" } ],
  "config":  [ { "key": "api_base", "default": "https://…", "description": "…",
                 "secret": false, "required": false } ],
  "channel": { "type": "…", "handler": "channel.qjs" },
  "agents":  [ { "name": "Scout", "sandbox_profile": "restricted", "…": "…" } ]
}
```

All handler paths are bundle-relative; no `..`, no absolute paths. `config[]`
keys register as `<name>.<key>` in the config registry. `agents[]` entries use
the agent-definition schema (see the cclaw-agents skill); an entry whose name
already exists is skipped, never overwritten.

## Handler contract (QuickJS)

Tool handlers are ES5-ish QuickJS scripts run in a sandboxed engine. A tool
handler file defines `function handle(args)` and returns a string (the tool
result). No Node APIs, no `require`; use the provided host bridges:

- `http_request(url[, {method, body, headers:{Name:Value}, markdownify}])` —
  synchronous HTTP, returns `{status, body, error}` (`body` is always a
  string, `""` on transport failure). `markdownify: true` converts an HTML
  body to markdown for readability — it is a rendering aid, not a security
  boundary (untrusted-content wrapping happens at storage time regardless).
- `fs.readFile` / `fs.writeFile` / `fs.readdir` / `fs.stat` / `fs.cwd` —
  workspace-scoped file access.

Keep handlers small and side-effect free where possible — network egress
still goes through the credential proxy and host allow-list.

## Skills

A skill is a directory with a `SKILL.md` whose frontmatter has `name:` and
`description:`. Only the index (name + description) enters your context;
read the full SKILL.md with `file_read` when the task matches. Personal skills
in `agents/<you>/skills/` shadow extension skills of the same name.
