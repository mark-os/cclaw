## Shell & Scripts

`shell_exec` runs commands in a sandboxed shell. No network. Writable only to your workspace.

Available: standard unix tools (grep, sed, awk, jq, find, sort, cat, head, tail, wc, cp, mv, mkdir, rm, chmod, diff, tar, gzip, git) + `mjs`.

### mjs — JavaScript with fetch

`/usr/local/lib/cclaw/mjs` is a JS evaluator. Its only special capability: `fetch()` works (routed through your allowed endpoints). The shell itself has no network.

Usage:
- `/usr/local/lib/cclaw/mjs -e 'code'` — inline JS
- `/usr/local/lib/cclaw/mjs script.js` — run file from workspace

fetch() respects your allowed_hosts. Denied hosts return 403.

### Composing pipelines

mjs reads stdin and writes stdout like any unix tool:

```bash
# Fetch API data, filter with jq
/usr/local/lib/cclaw/mjs -e 'let r = await fetch("https://api.example.com/items"); print(r.body)' | jq '.[] | .name' | grep error

# Transform a file through JS
cat data.csv | /usr/local/lib/cclaw/mjs -e 'let lines = stdin.split("\n"); print(lines.filter(l => l.includes("WARN")).join("\n"))' > warnings.txt

# Fetch and save
/usr/local/lib/cclaw/mjs -e 'let r = await fetch("https://api.example.com/config"); print(r.body)' > config.json
```

### When to use what

| Need | Use |
|------|-----|
| Read/write a file | `file_read` / `file_write` |
| Run a command, see output | `shell_exec` |
| HTTP request to allowed endpoint | `shell_exec` with `mjs` + `fetch()` |
| Transform data with code | `shell_exec`: pipe through `mjs` |
| Complex multi-step with network | `shell_exec`: mjs script in workspace |

Do NOT attempt to invoke `cclaw` from shell. It is not on PATH and will fail.
