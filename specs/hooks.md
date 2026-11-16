# Hooks

Status: **not available in this build.**

Extension hooks used to be JavaScript functions run inside the trusted daemon,
on the event-loop thread, at four points of a turn (`preAdvance`,
`postAdvance`, `beforeToolCall`, `afterToolCall`). That runtime is removed,
together with everything only it could reach: the `hook_directives` table
(per-request inject and suppress), the entry pin that survived the context
cut, `set_data`, `transform_content`, `annotate`, and the in-process tool
gate. Extension JavaScript no longer runs inside the daemon.

Hooks will return as sandboxed scripts bound to events. That mechanism is not
specified here yet.

What remains today:

- The `hooks` table (`extension_name`, `event`, `path`, `enabled` — see
  [schema.md](schema.md#hooks)). It is empty: schema patch v62 deleted every
  row, because the events they named no longer exist.
- The manifest `hooks[]` section and its ingest
  (`src/extension_manifest.c`). No event is valid, so
  `extension_manifest_validate` rejects any manifest whose `hooks[]` is not
  empty (`check_hook_events`), saying that hook events are not available in
  this build. A registered hook that never fires would be a silent lie.
