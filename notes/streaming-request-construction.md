# Streaming Request Construction

## Idea

Instead of loading full session into memory → building JSON → sending to curl,
stream the LLM request JSON directly from SQLite rows into curl's upload via
`CURLOPT_READFUNCTION`. One entry at a time, no full session in memory.

## Motivation

- Blocked parents (waiting on sub-agent) hold full session + request buffer (~50-100MB)
- With streaming construction, blocked parent holds only stack + sqlite handle + curl handle (~10-20MB)
- Long sessions with large context windows amplify the savings
- Reduces per-turn arena requirements (512KB → 64-128KB possible)

## Design

### Two-Pass Approach

**Pass 1 — Planning (cheap):**
```sql
SELECT id, json_extract(data, '$.type') as type,
       json_extract(data, '$.role') as role,
       length(data) as size
FROM entries WHERE session_id = ? AND id IN (branch_ids)
ORDER BY ...
```
Decide token budget, cut point (V7, V8). Output: ordered list of entry IDs to include.

**Pass 2 — Streaming (zero-copy-ish):**
Curl READFUNCTION pulls from a state machine:
1. Emit preamble: `{"model":"...","messages":[`
2. Step sqlite cursor over selected entry IDs
3. For each: emit `{"role":"...","content":` + entry.data content + `},`
4. Emit `]` + tools array (if any) + closing `}`

### Curl Integration

```c
typedef struct {
    sqlite3_stmt *cursor;   // steps through selected entries
    int phase;              // PREAMBLE, ENTRIES, TOOLS, DONE
    const char *chunk;      // current partial write buffer
    size_t chunk_offset;    // how far into current chunk
} RequestStreamer;

// CURLOPT_READFUNCTION
size_t stream_request_read(char *buf, size_t size, size_t nmemb, void *ud);
```

Use `CURLOPT_POST` + `CURLOPT_READFUNCTION` + `Transfer-Encoding: chunked`
(or precompute Content-Length from pass 1 sizes if provider requires it).

### Entry Data Format Advantage

Entry `data` is already JSON. For most entries, streaming is near-passthrough:
- System/user messages: `data` = `{"type":"message","role":"user","content":"..."}`
  → extract role + content, wrap in OpenAI format
- Assistant messages: `data` has content array, tool_calls — needs light reshaping
- Tool results: map to OpenAI `tool` role format

The reshaping is per-entry, bounded, no accumulation.

### Retry Handling

On 429/5xx retry: reset cursor, re-stream from beginning. SQLite cursor reopen is ~free.

### Compatibility

- Does NOT conflict with §C "no streaming" (that's response SSE streaming)
- This is request upload streaming — fully synchronous from agent's perspective
- curl still blocks until full response received

## Tradeoffs

| Pro | Con |
|-----|-----|
| ~5x memory reduction for blocked parents | Adds state machine complexity to request builder |
| Smaller arena requirements | Two-pass (plan + stream) vs one-pass (load + serialize) |
| Scales to very long sessions | Retry requires re-stream (cheap but not free) |
| Entry data already JSON (minimal transform) | Some providers may want Content-Length (need precompute) |

## Prerequisites

- Context window manager (T12) must output an ordered ID list (already does conceptually)
- Entry data format stable (§D — already locked)
- curl READFUNCTION familiarity

## When to Implement

After core agent loop is proven and blocking sub-agents are working. This is an
optimization — correctness first, then measure actual memory pressure under load.
If blocked parents at 50-100MB are fine on 2GB with V3 limits, defer indefinitely.
