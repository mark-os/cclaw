# Pi Compaction Model

## When It Triggers

Pi's `shouldCompact()` is simple:

```
contextTokens > contextWindow - reserveTokens
```

Where `reserveTokens` defaults to 16384. So compaction fires when the context
is within 16K tokens of the model's limit.

It's checked **after** each LLM response (when usage.totalTokens is known from
the provider response). Not every turn — only when the response reports token
usage that exceeds the threshold.

## What It Does

1. `prepareCompaction()` — selects which entries to summarize (everything before
   a "keep recent" window of ~20K tokens from the tail)
2. Serializes the to-be-compacted messages into a text conversation
3. Extracts file operations (reads/writes) from tool calls in that range
4. Calls the LLM with a summarization prompt asking for a concise summary
5. Stores the result as a `compaction` entry in the session tree:
   `{"type":"compaction","summary":"...","first_kept_id":"...","tokensBefore":N}`
6. Future `getBranch()` calls see the compaction entry and use its summary
   instead of loading the original messages

## Settings (defaults)

```typescript
{
  enabled: true,
  reserveTokens: 16384,    // headroom before triggering
  keepRecentTokens: 20000, // recent context preserved verbatim
}
```

## Key Design Points

- Compaction is a **separate LLM call** (uses same model/auth as the agent)
- The summary replaces old messages — they're still in the DB but not loaded
- File operations (which files were read/written) are tracked in compaction
  metadata so the agent retains awareness of what it touched
- Compaction entries are part of the session tree (have parent_id like any entry)
- Multiple compactions can stack — each one summarizes everything before it
- The harness is "idle" during compaction (no concurrent agent turns)

## OpenClaw Additions

OpenClaw adds more triggers on top of Pi's basic model:
- Context overflow error from provider → emergency compaction
- Timeout with high token usage (>65% of window) → preemptive compaction
- Post-compaction loop guard (detect if compaction itself causes loops)
- Tool result truncation as a lighter alternative before full compaction

## CClaw Implications

For CClaw, the simplest version:
1. After each LLM response, check if `usage.input` > `context_window - reserve`
2. If yes, run a separate LLM call to summarize old messages
3. Store as a compaction entry in the entries table
4. `context_build` recognizes compaction entries and uses summary instead of
   loading the original messages before that point

The trigger is natural — it only fires when the provider tells you you're close
to the limit. No per-turn overhead otherwise.
