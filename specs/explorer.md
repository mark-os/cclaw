# Data explorer — `/admin/explore` and `/admin/api/`

A read-only view of everything in `cclaw.db` for the operator: the tables,
what each column means, and how rows link to each other in both directions.
It is a JSON API plus one static page that draws it; the API is also the
first cut of what a mobile app will use. Code: `src/explore.c`,
`templates/explorer.html`, tests in `test/test_explore.c`.

The old server-rendered page at `/admin` (`src/dashboard.c`) is unchanged and
still serves; it goes away when the explorer gains its actions (below).

## Rules

- **Reading is generic.** One list page and one row page serve every table.
  Nothing in the API knows a table by name except four short hand lists: the
  home page's order, links the schema does not declare, labels for numeric
  codes, and values that are hidden. Three tables (agents, providers,
  channels) get a few extra sections on their row page, built from the same
  API calls any client could make.
- **Writing will be specific.** There is no "edit any cell". When editing
  arrives (step 4b), each edit is a named action that runs the same checks as
  the CLI, and anything the event loop must carry out goes through a queue
  row and `wake_session(0)`, as approval decisions do today. Conversation
  history is never editable.
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
- **Read-only.** Plain SELECTs in autocommit, never `BEGIN`. The web thread's
  own handle is used under `web_db_lock()`; the lock is held for the
  database work only — the response is copied out and written after it is
  released, so a slow client cannot hold up an approval click on `/admin`.
  Lists are paged and long values previewed to keep each request short.
  Only `GET` is accepted (anything else is 405).

## Access

The same admin token as `/admin` (`web_admin_token`, printed as a link by
`cclaw dashboard`). The page accepts `?token=` once and answers with a 303 to
`/admin/explore` that moves the token into the `cclaw_admin` cookie
(HttpOnly, SameSite=Strict), exactly like `/admin`. The API accepts the
cookie only. Without it every path is 403: the page explains how to get in,
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
  `llm_responses`, `inbox`, `approvals`, `channel_outbox`, `hook_directives`,
  `media_jobs`, `llm_jobs`, `cron_jobs`) → `sessions`; `entries.parent_id`
  and `original_parent_id` → `entries`; `tool_calls.entry_id` /
  `result_entry_id` and `hook_directives.entry_id` → `entries`;
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
  value as `{"hidden": true}`.

```json
{"table": "entries", "key": ["id"], "limit": 2,
 "rows": [{"key": [330], "values": {"id": 330, "session_id": 1, "role": 2,
           "content": "error: network error [resp #41]", …}}, …],
 "next": [322]}
```

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

## The page — `/admin/explore`

`templates/explorer.html`, embedded at build time. Hash routes:

| Route | Page |
|---|---|
| `#/` | Home: the main data with one-line descriptions and counts, then "All N tables" |
| `#/tables` | Every table, with its count and the first sentence of its comment |
| `#/t/<table>[?f=<filter JSON>]` | List: key and a few identifying columns, long text previewed, filter chips, "Load more" |
| `#/r/<table>/<key parts…>` | Row: fields grouped "What you configured" / "What cclaw observed" where the column class is known (else one group), each with its explanation; links from this row; rows that point here, with counts, opening the filtered list |

Agents, providers and channels add sections above the fields: an agent's
model lists by purpose in order, grants by kind with their source, sessions
with their chat and whether that chat still sends to them, memory, extensions
and schedules; a provider's key (stored secret or environment), registered
models and how many models the provider offers; a channel's routed and
unrouted chats with their reply mode. Anything else uses the generic pages —
the rule is to improve those rather than add a fourth shaped view.

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
