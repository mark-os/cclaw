## Shell & Scripts

`shell_exec` runs commands in a sandboxed shell. No network. Writable only to your workspace.

Available: standard unix tools (grep, sed, awk, jq, find, sort, cat, head, tail, wc, cp, mv, mkdir, rm, chmod, diff, tar, gzip, git).

### JavaScript and HTTP — use the `js_eval` tool, not the shell

The shell has no network and no JS interpreter. To run JavaScript or make an HTTP request,
use the `js_eval` tool. It runs modern JavaScript (no modules, no top-level await) in a
sandboxed engine where `http_request(url[, opts])` works, routed through your allowed endpoints.

```js
// js_eval tool, not shell_exec:
const r = http_request('https://api.example.com/items');  // synchronous; NOT a Promise
print(r.json().map((x) => x.name).join('\n'));
```

`http_request` respects your `allowed_hosts`; a denied host errors. To add a host, call
`request_config` with `{"action":"grant_host","host":"..."}`.

To move data between the shell and JS, write a file with `file_write` (or `shell_exec`) and
read it in `js_eval` via `fs.readFile(path)`, or vice versa.

### When to use what

| Need | Use |
|------|-----|
| Read/write a file | `file_read` / `file_write` |
| Run a command, see output | `shell_exec` |
| HTTP request to allowed endpoint | `js_eval` with `http_request()` |
| Transform data with code | `js_eval` (synchronous JS) |
| Pipe/filter text with unix tools | `shell_exec` (grep, sed, awk, jq, ...) |

Do NOT attempt to invoke `cclaw` from shell. It is not on PATH and will fail.
