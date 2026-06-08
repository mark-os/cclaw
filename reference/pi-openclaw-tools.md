# Pi & OpenClaw — Base Tools Reference

## Pi Base Tools (7)

From `packages/coding-agent/src/core/tools/`. These are the open-source foundation.

### read
Read file contents with optional line range.
```
path:   string (required) — file path, relative or absolute
offset: number (optional) — start line, 1-indexed
limit:  number (optional) — max lines to read
```

### bash
Execute a shell command.
```
command: string (required) — bash command
timeout: number (optional) — seconds, kills on expiry
```

### write
Create or overwrite a file.
```
path:    string (required) — file path
content: string (required) — full file content
```

### edit
Targeted search-and-replace within a file. Multiple edits per call.
```
path:  string (required) — file path
edits: array (required) — list of replacements:
  oldText: string — exact text to find (must be unique in file)
  newText: string — replacement text
```
Edits must not overlap. Each `oldText` matched against original file, not incrementally.

### grep
Search file contents by regex.
```
pattern: string (required) — regex or literal
path:    string (optional) — directory or file to search (default: cwd)
glob:    string (optional) — filter files, e.g. '*.ts'
```

### find
Find files by glob pattern.
```
glob: string (required) — e.g. '*.ts', '**/*.json'
path: string (optional) — directory to search (default: cwd)
```

### ls
List directory contents.
```
path:  string (optional) — directory (default: cwd)
limit: number (optional) — max entries (default: 500)
```

---

## OpenClaw Additional Tools

OpenClaw replaces Pi's tool implementations with sandboxed versions and adds:

### exec
Replaces Pi's `bash`. Adds background execution, PTY support, sandboxing.
```
command:    string (required)
workdir:    string (optional)
env:        object (optional) — key-value env vars
yieldMs:    number (optional) — ms before backgrounding (default 10000)
background: boolean (optional) — run in background immediately
timeout:    number (optional) — seconds
pty:        boolean (optional) — use pseudo-terminal
elevated:   boolean (optional) — run on host with elevated perms
host:       string (optional) — auto|sandbox|gateway|node
```

### process
Manage background/long-running processes.
```
action:    string (required) — list|poll|log|write|send-keys|submit|paste|kill|clear|remove
sessionId: string (optional) — target process
data:      string (optional) — data for write
keys:      string[] (optional) — key tokens for send-keys
text:      string (optional) — text for paste
offset:    number (optional) — log offset
limit:     number (optional) — log length
timeout:   number (optional) — poll wait ms (max 30000)
```

### apply_patch
Apply unified-diff-style patches.
```
input: string (required) — patch content in *** Begin Patch/End Patch format
```

### web_search
Search the web.
```
query:   string (required)
count:   number (optional) — result count
country: string (optional) — 2-letter country code
```

### web_fetch
Fetch a URL and extract content.
```
url:         string (required) — HTTP(S) URL
extractMode: string (optional) — markdown|text (default: markdown)
maxChars:    number (optional) — truncation limit (min 100)
```

### Other OpenClaw tools
- `memory_search`, `memory_get` — persistent memory
- `browser`, `canvas` — UI/visual tools
- `cron` — scheduled tasks
- `image`, `image_generate`, `video_generate` — media
- `sessions_list`, `sessions_send`, `sessions_spawn`, `subagents` — multi-agent

---

## OpenClaw Sandboxing

OpenClaw runs tools inside Docker containers (`openclaw-sandbox:bookworm-slim`).

**Sandbox defaults:**
- Workspace mounted at `/workspace`
- Idle timeout: 24h, max age: 7 days
- Container prefix: `openclaw-sbx-`

**Tools allowed in sandbox:**
`exec`, `process`, `read`, `write`, `edit`, `apply_patch`, `image`, `sessions_*`, `subagents`

**Tools denied in sandbox:**
`browser`, `canvas`, `cron`, `gateway`, channel tools

**Elevated exec** bypasses sandboxing — runs on host via gateway. Requires explicit policy approval.

**File access:** All `read`/`write`/`edit` calls are workspace-root-guarded. Paths outside the workspace root are rejected unless explicitly allowed via `additionalRoots` config (e.g., skill read paths).
