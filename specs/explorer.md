# The dashboard — admin panel at `/dashboard`, data explorer at `/admin`, API at `/admin/api/`

Everything in `cclaw.db` for the operator: the tables, what each column means,
how rows link to each other in both directions — and, where a thing is shown,
the named actions that change it. It is a JSON API plus one static page that
draws it; the API is also the first cut of what a mobile app will use. Code:
`src/explore.c` (reading, HTTP), `src/explore_act.c` (actions),
`src/dashboard.c` (the token), `templates/explorer.html`, and the admin panel
`templates/dashboard.html` (below, "The admin panel"); tests in
`test/test_explore.c`, `test/test_explore_act.c`, `test/test_web.c`.

The server-rendered page that used to live at `/admin` (`dashboard.c`'s HTML
and its form endpoint `POST /admin/act`) is gone; `/admin/explore`, the
explorer's address while both existed, redirects to `/admin`.

## Rules

- **Reading is generic.** One list page and one row page serve every table.
  Nothing in the API knows a table by name except four short hand lists: the
  home page's order, links the schema does not declare, labels for numeric
  codes, and values that are hidden. Three tables (agents, providers,
  channels) get a few extra sections on their row page, built from the same
  API calls any client could make.
- **Writing is specific.** There is no "edit any cell": each edit is a named
  action (below) that runs the same checks as the CLI verb or agent path it
  stands beside, and anything the event loop must carry out goes through a
  queue row and `wake_session(0)` (approval decisions). This first explorer
  has no actions on conversation history — sessions, entries, tool calls, LLM
  responses, inbox, outbox, change_log — and no delete for agents or
  sessions. That is its scope, not a rule: session manipulation will arrive
  as named actions like any other write.
- **SQL is the serializer.** Every response is the single text column of one
  statement built with JSON1 (`json_object`, `json_group_array`, …); C only
  chooses which schema names appear in that statement.
- **Request names never become SQL.** A table name must be a row of
  `pragma_table_list` (schema `main`, type `table`, not `sqlite_*`; the FTS
  index and its shadow tables are not served); a column name must be one of
  that table's `pragma_table_info` names, compared exactly. The spelling that
  goes into the query is the schema's own, quoted as an identifier. Keys,
  filter values and cursors are always bound parameters. An unknown table is
  404, an unknown column 400.
- **Secret values never leave the server** (see below).
- **Reads never write.** Plain SELECTs in autocommit, never `BEGIN`. The web
  thread's own handle is used under `web_db_lock()`; the lock is held for the
  database work only — the response is copied out and written after it is
  released, so a slow client cannot hold up an approval click. Lists are paged
  and long values previewed to keep each request short. Every path but
  `/admin/api/act` accepts only `GET` (anything else is 405); `act` accepts
  only `POST`.

## Access

The admin token (`web_admin_token`). `cclaw dashboard` prints two links with
it, `/dashboard` first and `/admin` second; an admin who sends `/dashboard`
in a direct message to an agent gets the first one (see
[channels.md](channels.md)). Either page accepts `?token=` once and answers
with a 303 to its own clean address that moves the token into the
`cclaw_admin` cookie (`Path=/`, HttpOnly, SameSite=Strict) — so signing in on
one page signs in the other and the API. The API accepts the cookie only. Without it every path is 403: the page explains how to get in,
the API answers `{"error": "not signed in: …"}`.

The page is served with `Content-Security-Policy: default-src 'none';
script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self';
…`, `Cache-Control: no-store` and `X-Content-Type-Options: nosniff`. It makes
no external requests (system fonts, no images), so it works on a LAN box with
no internet.

## API

All responses are JSON objects; errors are `{"error": "…"}` with 400, 403,
404 or 500.

### `GET /admin/api/meta`

Everything needed to draw any table:

```json
{"tables": [{"name": "memory_blocks", "doc": "Two words, used everywhere: …",
   "count": 5, "key": ["id"],
   "columns": [{"name": "label", "type": "TEXT", "pk": 0, "notnull": 1,
                "doc": "the name agents address the block by",
                "class": null, "hidden": null, "display": null}, …],
   "links_out": [{"from": "memory_blocks", "from_columns": ["created_by"],
                  "to": "agents", "to_columns": ["name"], "declared": true, "when": null}],
   "links_in": [{"from": "agent_memory_blocks", "from_columns": ["block_id"],
                 "to": "memory_blocks", "to_columns": ["id"], "declared": true, "when": null}, …]},
  …],
 "home": [{"title": "Agents", "tables": ["agents"], "doc": "Who does the work: …"}, …],
 "codes": {"entries.role": {"0": "system", "1": "user", …}, …},
 "attention": {"approvals_pending": 1, "sessions_active": 0, "chats_unrouted": 3, "memory_shared": 2}}
```

- `doc` — the comments in `templates/schema.sql` (the copy embedded in this
  binary): the comment block right above a `CREATE TABLE` explains the table;
  a column's trailing comment, continued on deeper-indented comment lines,
  explains the column; comment lines at the column indent explain the column
  that follows. Wrapped lines are joined; indented lines stay list items.
  A column with no comment has `null`. Keeping these comments good is how the
  explorer gets better.
- `class` — `setting` / `measurement` / `secret` from the change log's column
  list (`CHANGE_LOG_COLUMNS`, `src/change_log.c`); `null` where the list does
  not classify the column (every table it does not log).
- `hidden` — `always`, `some` (hidden in some rows), or `null`.
- `display` — `time` (unix seconds: columns named `at`, `*_at`, `*_until`),
  `bool` (a hand list of 0/1 columns), `code` (a numeric code in `codes`), or
  `null`.
- `key` — how a row is addressed: the primary key columns in key order, or
  `["rowid"]` when the table has none or its key is hidden (`shares`).
- Links: every `pragma_foreign_key_list` entry (`declared: true`), plus a
  hand list (`declared: false`) of links the schema leaves undeclared
  (plan v0.8 decision 19): the conversation tables (`entries`, `tool_calls`,
  `llm_responses`, `inbox`, `approvals`, `channel_outbox`,
  `media_jobs`, `llm_jobs`, `cron_jobs`) → `sessions`; `entries.parent_id`
  and `original_parent_id` → `entries`; `tool_calls.entry_id` /
  `result_entry_id` → `entries`;
  `sessions.parent_session_id` → `sessions`; `sessions.owner_instance` →
  `processes`; `channel_outbox` / `channel_events.channel_name` → `channels`;
  `secret_hosts.secret_name`, `providers.api_key_env` → `secrets`;
  `models_cache.provider_name` → `providers`; and `grants.value` → `tools`
  `when {"kind": "tool"}`, → `secrets` `when {"kind": "secret"}`.
- `codes` — `entries.role` (`Role`, types.h), `entries.stop_reason`
  (`StopReason`), `llm_jobs.job_type` (0 turn, 1 compaction —
  llm_worker.c), `channel_outbox.deliver_mode` (0 formatted, 1 plain).

### `GET /admin/api/rows?table=T[&filter=F][&limit=N][&after=C]`

One page of a table.

- Order: a single integer key (or rowid) newest first; any other key
  ascending.
- `limit` 1–200, default 50.
- `after` — the previous page's `next`, a JSON array of key values (keyset
  paging; no OFFSET). `next` is `null` on the last page.
- `filter` — a JSON object of `column: value` (equality), `column: null` (IS
  NULL) or `column: [values]` (IN). This is how "the models of this provider"
  is fetched. Hidden columns cannot be filtered on (400), so a filter cannot
  be used to probe a hidden value.
- Text longer than 200 characters comes back as `{"preview": "…", "length":
  N}`; JSONB as its JSON text; any other blob as `{"blob": bytes}`; a hidden
  value as `{"hidden": true}`. `full=1` lifts the 200-character limit to the
  row page's; the pages that show or edit whole values (memory notes, chat
  guidance) ask for it, the generic list does not.

```json
{"table": "entries", "key": ["id"], "limit": 2,
 "rows": [{"key": [330], "values": {"id": 330, "session_id": 1, "role": 2,
           "content": "error: network error [resp #41]", …}}, …],
 "next": [322]}
```

`q` — a case-insensitive substring of the table's text key columns (hidden
ones excluded); 400 for a table with none. This is how the page searches a
provider's offered models (`models_cache`, hundreds of rows):
`rows?table=models_cache&filter={"provider_name":"x"}&q=vision`.

### `GET /admin/api/approval?id=N`

`{"id", "state", "resolve", "park_reason", "tool_name", "summary"}` — the
approval card an approver sees on a channel (`approval_format_summary`), as
text, never the raw `args_json`.

### `GET /admin/api/row?table=T&key=K`

One row by key (`K` is a JSON array in `meta`'s key order — text, integer
and composite keys alike; an integer part may be a number or a numeric
string). Values are complete up to 1,000,000 characters (longer ones are a
preview with the full length). Each outgoing link says whether its target
exists (a `-1` sentinel or a dangling name shows `exists: false`); each
incoming link carries a ready `filter` for `rows` and the count of referring
rows (counting stops past 1000: `count_capped`).

```json
{"table": "agent_models", "key": ["agent_name", "purpose", "pos"],
 "row": {"key": ["researcher", "chat", 0],
         "values": {"agent_name": "researcher", "purpose": "chat",
                    "model_id": "anthropic/claude-sonnet-5-5", "pos": 0,
                    "reasoning_effort": "high"}},
 "links_out": [{"from": "agent_models", "from_columns": ["model_id"], "to": "models",
                "to_columns": ["id"], "declared": true, "when": null,
                "values": ["anthropic/claude-sonnet-5-5"], "exists": true}, …],
 "links_in": []}
```

A conditional link that does not apply to the row (a `host` grant has no
tool) is `null` in `links_out`.

## What is hidden

| Where | Hidden | Why |
|---|---|---|
| `secrets.value` | always (`COL_SECRET` in the change log list) | the ciphertext is still the secret, with the key on the same disk |
| `config.value`, `config.default_value` | where `secret <> 0` or `key = 'web_admin_token'` | secret config keys resolve elsewhere (value should be NULL anyway); the admin token is the dashboard's own credential |
| `shares.token` | always; shares are addressed by rowid | a bearer capability for the shared file |

Hidden means: replaced by `{"hidden": true}` in every response, never
filterable, never part of a key or a cursor, and any link whose columns
include it is left out. Decided not hidden:

- `entries.content`, `llm_responses.body` / `request_body`, `inbox` /
  `channel_outbox` / `channel_events` / `media_jobs` payloads — conversation
  data that the secret scanner has already processed; the operator may read
  it.
- `approvals.args_json` — holds the call as the model wrote it, with
  `{{SECRET:name}}` placeholders. Interpolation happens after parking
  (`dispatch.c`), and the tools that write their own approval
  (`create_agent`, `update_agent`, `request_config`, `cron_set`,
  `extension_promote`, `install_update`) are not interpolated tools, so no
  resolved value is stored there.
- `channel_state` — the shipped channels store a bot id, user and emoji maps
  (Discord), an update offset (Telegram) and a cursor (WhatsApp); no
  credentials. A channel extension that stored a credential there would be
  shown; credentials belong in secrets.
- `change_log` — its triggers never record a secret value.
- `.cclaw_key` is not in the database.

`test/test_explore.c` seeds a marker into every hidden place, walks every
table through `meta`, every page of `rows` and `row` for every row, and fails
if the marker appears in any response.

## Actions — `POST /admin/api/act`

One endpoint; the body is a JSON object naming the action and its fields:
`{"action": "grant_add", "agent": "Researcher", "kind": "host", "value":
"api.github.com"}`. A field the action does not take is refused (a typo
must not silently change nothing). The answer is `{"ok": true, "message":
"<what changed, in a sentence>", ...}` or `{"error": "<what was wrong and how
to fix it>"}` with 400 (bad input), 403, 404 (no such thing), 409 (the state
does not allow it: already there, still in use, needs a confirmation), 502
(the provider did not answer) or 503 (database busy, secret key not loaded).

**Forgery.** The cookie alone is not enough for another site's page to act.
SameSite=Strict keeps it off cross-site requests in current browsers, and on
top of that an action must carry `Content-Type: application/json` and
`X-Cclaw-Action: 1` — a cross-site HTML form can send neither, and a
cross-site `fetch()` that sets them is preflighted, which this server never
approves (it sends no CORS headers). When the browser states `Sec-Fetch-Site`
it must be `same-origin`; when it states `Origin`, its host must be the
request's `Host`. Refusals are 403 with the reason. The body is capped at
1 MB and zeroed after use (it may carry a secret value or a key).

**Writes.** Each action runs under `web_db_lock()` on the web server's handle
(actor `dashboard`), in one `BEGIN IMMEDIATE` where it writes more than one
row, with the handle's context set to `act:<action>` — so every change_log
row an action causes says which action did it. (The change log covers the
tables `CHANGE_LOG_COLUMNS` lists; memory, channels and secret hosts are not
among them.) Network actions release the lock around the provider call.

| Action | Fields | Checks (refusals) |
|---|---|---|
| `grant_add` | agent, kind (`tool` `host` `read_path` `write_path` `secret`), value, approval_mode?, expires_at? | agent exists; tool exists in `tools`; host is a host rule, not a URL; a path is absolute and not cclaw.db, its -wal/-shm, `.cclaw_key` or a directory holding them (`grant_path_hits_db`); a secret name is valid and not a `system` secret (as `cclaw secret grant`); mode only on tools; expiry in the future; an identical live grant is 409. Source `operator`. A tool's mode, when not given, is `always` for `agent_approval_tools`, else `silent`; re-granting a tool with another mode changes only the mode |
| `grant_remove` | agent, kind, value | the grant exists |
| `agent_copy` | from, name, description?, attach_memory? | the operator's `clone_from` through `agent_definition_apply` (creator NULL: no creator caps; everything else as for any new agent — baseline extensions, heartbeat schedule, workspace under `<db dir>/agents/<name>/workspace`, made on the web thread since nothing in it belongs to the event loop). Copies prompt and limits, every model list with effort, every grant (source `agent_create`), extension attachments. Memory is not part of a clone; `attach_memory` shares each of the source's blocks **read-only**. Name PascalCase and new |
| `models_set` | agent, purpose (`chat` `image` `audio` `utility`), models: [{model_id, reasoning_effort?}] in order | one transaction replacing that list; chat may not be empty; no duplicates; models exist; effort only on chat and utility, one of the five levels; image/audio only for models whose capabilities include it (the trigger is the backstop, its refusal turned into a sentence); an unchanged list writes nothing |
| `session_new_on_chat` | agent, channel, chat_id, confirm_handover? | the chat is a `channel_chats` row; `channel_pin_new_session`. A chat that talks to **another agent's** session is 409 with `{"confirm": {"kind": "handover", "from", "to"}}` until `confirm_handover` equals the receiving agent's name — handing a chat to an agent is an authority change, so it takes a typed name, not one click. The previous session is left behind and keeps naming the chat |
| `session_move_to_chat` | session_id, channel, chat_id, confirm_handover? | `channel_pin_session`: the session's old chat goes on with a fresh session for the same agent (decision 4); the target chat's previous session is left behind; same handover rule; sub-agent sessions refused |
| `chat_settings` | channel, chat_id, delivery_mode? (`auto` `explicit`), tool_filter? (list or `"unrestricted"`), guidance? | tools exist; an empty list is refused — "unrestricted" is its own choice; `""` guidance clears |
| `chat_unroute` | channel, chat_id | `cclaw route rm`: session_id and notified_at cleared, the row stays |
| `channel_defaults` | channel, default_agent? (name or null), default_tool_filter? | agent exists; tools exist |
| `provider_add` | name, base_url, endpoint_type (`openai` `gemini` `anthropic`), api_key? | name starts with a letter; http(s) URL; the key's secret name `<NAME>_API_KEY` is not taken. The key is checked **before anything is written** by listing the provider's models (`models_cache_fetch`, authenticated as that wire format authenticates, with the lock released, waiting up to max(30 s, `llm_probe_timeout`)); if that fails nothing is saved. Then one transaction: the key as a `system` secret, the provider row, its catalog |
| `provider_edit` | name, base_url?, endpoint_type?, request_extra? (JSON object or empty), api_key? | a change to endpoint, format or key is checked the same way first and nothing is saved if it fails; the key is written through `admin_set_key` |
| `provider_refresh_models` | name | `models_cache_fetch` + `models_cache_ingest`; on failure the previous list stays (502) |
| `provider_remove` | name | refused while it has models; its cached list goes, its key secret stays (the answer says so) |
| `model_import` | provider, model, context_window?, max_output_tokens?, capabilities? | id `<provider>/<model>`, not already registered; capabilities lowercase words |
| `model_edit` | id, context_window?, max_output_tokens?, capabilities?, effort_map?, enabled? | effort map shape (format and levels as in schema.sql); a capability cannot be taken away while the model is on a list for it; disabling the only enabled chat model of an agent is allowed with a warning naming the agents |
| `model_remove` | id | refused while on any list, naming them |
| `memory_block_create` | label, description?, placement?, char_limit?, read_only?, agent? | label rule; per-agent label uniqueness; `created_by` NULL; with `agent`, attached read-write |
| `memory_block_edit` | block_id, description?, placement?, char_limit?, read_only? | |
| `memory_block_delete` | block_id | the answer names the agents it was detached from |
| `memory_attach` | block_id, agent, read_only? (default true) | label clash refused with the clashing block named; also switches an existing attachment |
| `memory_detach` | block_id, agent | |
| `memory_note_add` / `memory_note_edit` / `memory_note_delete` | block_id, note_id, text | the note belongs to that block; empty text refused; over the limit is allowed (the operator is not bound) with a warning |
| `approval_decide` | approval_id, decision (`once` `always` `deny`) | pending; not already decided; `once` refused for an applied request (`resolve = apply`), `always` for a sensitive target — as the event loop would. Queued with `approval_decision_queue(…, "dashboard")` + `wake_session(0)`; the loop applies it |
| `approval_grant_from_history` | approval_id | a denied `request_config`; `admin_grant_from_history(…, "dashboard")` records a new approved row the grants point at |
| `secret_add` | name, value, scope?, replace? | name rule; an existing name needs `replace`, and keeps its scope; the value is written encrypted and never returned |
| `secret_hosts_set` | name, hosts | replaces the set; host rules; not for system secrets |
| `secret_remove` | name | refused while a provider uses it as its key; `db_secret_rm` (value, hosts, grants) |

Letting an agent use a secret is `grant_add` with kind `secret`.

## The page — `/admin`

`templates/explorer.html`, embedded at build time. Hash routes:

| Route | Page |
|---|---|
| `#/` | Home: the main data with one-line descriptions and counts, then "All N tables" |
| `#/tables` | Every table, with its count and the first sentence of its comment |
| `#/t/<table>[?f=<filter JSON>]` | List: key and a few identifying columns, long text previewed, filter chips, "Load more" |
| `#/r/<table>/<key parts…>` | Row: fields grouped "What you configured" / "What cclaw observed" where the column class is known (else one group), each with its explanation; links from this row; rows that point here, with counts, opening the filtered list |
| `#/add-provider` | New provider: name, endpoint, wire format, key |

Agents, providers, channels, models, memory blocks, chats, approvals and
secrets add sections (and their actions) above the fields: an agent's
model lists by purpose in order, grants by kind with their source, sessions
with their chat and whether that chat still sends to them, memory, extensions
and schedules; a provider's key (stored secret or environment), registered
models and how many models the provider offers; a channel's routed and
unrouted chats with their reply mode and its defaults; a model's edit and
remove; a block's attachments and notes; a chat's route and settings; an
approval's card and decision; a secret's allowed hosts and the agents that may
use it. The providers, memory blocks and secrets lists offer adding one; the
approvals list shows the pending ones as cards. Anything else uses the generic
pages.

Actions on the page: an editor opens below its button and closes with Escape
or Cancel; Enter submits. Anything destructive or authority-widening (remove,
delete, unroute, add a grant, hand a chat to another agent, make a shared block
writable, open a channel's door, always-allow) asks in an inline confirm that
says what will happen; handing a chat to another agent also needs that agent's
name typed. After an action the page is drawn again from the API with the
server's sentence on top; a refusal is shown next to the control that sent it.
Key and secret fields are password inputs, cleared on submit, never put in a
URL or kept in page state. Image/audio model pickers offer only capable models;
an imported model's capabilities are prefilled from the provider's modality —
only the input side of `in->out`, and only the exact tokens `image`/`audio`
(no modality, no media).

Values are drawn for people: times as local date and time with the raw value
beside them, codes as words, 0/1 as yes/no, JSON pretty-printed and
collapsible, long text collapsed, NULL as "none", hidden values as a marker.

Breadcrumbs follow the path taken (Home › Agents › researcher › team-notes,
or Home › Memory blocks › team-notes for the same block), kept in
`history.state`, so back/forward and reload keep them; a deep link opened
fresh gets the natural trail (Home › table › row). Every database value is
untrusted (agents and chat users write these tables) and reaches the page
only through `textContent` — the page contains no `innerHTML` (a unit test
checks the source).

## The admin panel — `/dashboard`

`templates/dashboard.html`: the task-shaped screens for the person running
the agents, in plain words, with no table or column names. It is a client of
the same API (`rows`, `row`, `approval`, and `act` for approval decisions
only); it adds no endpoint and edits nothing else — every section links to
the agent's row in the explorer ("change", "all data"). Same serving rules as
`/admin`: GET only, exact path, same token and cookie, same headers; no
`innerHTML` (the source test covers both pages). Hash routes:

| Route | Screen |
|---|---|
| `#/` | **Needs you**, **Agents**, **System** |
| `#/agent/<name>` | Thinks with, Can use, Can reach, Can see, Talks and runs |
| `#/session/<id>` | The conversation by turn, newest first, older turns on demand |

What each signal is read from — nothing is shown that the database does not
record:

- **Needs you** — pending approvals (a sentence built from `tool_name` and
  `args_json`, the decisions the explorer offers: Once / Always / Deny, or
  Approve / Deny for an applied request, no Always for a sensitive target;
  "Details" shows the channel card); a session whose leaf entry is an
  assistant error (`stop_reason` 4 — what `advance.c` itself tests) from the
  last three days; `sessions.compaction_fail_count` ≥ 3; an active channel
  whose `<ext>.enabled` is on (DB value or default) but has no `pid`; an
  unrouted chat the gate told the operator about (`notified_at`) that spoke in
  the last seven days; an outbox row `failed…` in the last 24 h; and
  `update.notified_tag` differing from `update.installed_tag`.
- **Agents** — "waiting on you" (a pending approval or an
  `awaiting_approval` session), "working" (any other non-idle session),
  "waiting to retry" (`rate_limited`), else "idle".
- **System** — the newest daemon row of `processes` (up since);
  `update.installed_tag` when set; each active channel: "connected" (running,
  `last_activity_at` within 300 s), "running" (no watched transport), "no
  contact" (older mark), "not running", or switched off; the enabled cron job
  with the smallest `next_run_at`.
- **Session** — who spoke from `entries.data.source` (`cli`, the chat's channel
  — "you" when `channel_chats.is_admin` — `cron`, …); a tool call with its
  result and, matched by `tool_call_id`, its approval's state and
  `decided_via`; what the turn sent to the session's chat from
  `channel_outbox` rows created between this turn's first entry and the
  next's (they are pruned after `outbox_retention_sec`). Long text arrives as
  a preview and is fetched whole by "show more"; a turn is drawn once all its
  entries have arrived (keyset pages of 200).
