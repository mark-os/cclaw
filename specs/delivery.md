# Delivery — one vocabulary for session boundaries, both edges

Status: milestone 1 shipped 2026-08-04. Milestone 2 (`session_send`/ping,
reply-edge timeouts) is deferred; `target_kind='session'` is reserved for it.

Every outbound edge of a session is a `delivery_edges` row with a policy
from one shared vocabulary: the **standing parent edge** every child gets at
creation, the **channel edge** a session gets when it is bound to a chat, and
the **one-shot `tool_call` edge** a blocking launch adds on top. Parent and tool_call edges carry their policy, frozen. A channel
edge carries none (`policy` is NULL — a CHECK enforces it): its policy is the
`delivery_mode` of the chat the session belongs to, read at every boundary
(`EDGE_POLICY` in db.h, the one definition every reader uses). The row keeps
only the cursor. `advance_deliver_boundary()` (advance.c)
evaluates a session's edges at each of its turn boundaries; `'iteration'`
edges also ship mid-turn via `advance_deliver_iteration()`.

## The five policies (uniform on every edge)

| Policy | Ships | When |
|--------|-------|------|
| `iteration` | every content-bearing assistant message, incl. tool_use-stop commentary | as each lands |
| `digest` | assistant prose since the edge cursor, `---`-joined (no tool summaries — `check_session` is the pull surface) | turn end |
| `turn` | final assistant message (`get_response_text`) | turn end — the old standard |
| `quiescent` | final assistant message | turn end **iff** the subtree is quiescent — the default |
| `explicit` | nothing auto-ships | — |

**No policy×edge legality matrix** — every policy is valid on every edge;
costs are documented, not legislated (`iteration` on a parent edge costs the
parent one LLM turn per message; `explicit` on a parent edge means the child
is mute until milestone 2's send tool). The one structural exception: a
**blocking launch** is a one-shot edge and must resolve with exactly one
payload, so an explicit `delivery` arg of `explicit`/`iteration` is refused
with feedback. An *inherited* `iteration` degrades both of the blocking
child's edges to the registry default instead — refusal is only for the
spelled-out ask.

## Quiescent, defined

`session_subtree_quiescent()` (db.c, one recursive CTE — derived, never
stored): over the whole subtree (recursive `parent_session_id`),

1. every session is `idle`,
2. no unconsumed inbox rows,
3. no `llm_jobs` rows,
4. no pending/running `tool_calls`.

Rule 2 is load-bearing: after a grandchild finishes, everyone is momentarily
idle while its result sits unconsumed in the middle session's inbox — that
state is *not* quiescent. Consequences accepted by design: a `quiescent`
delegation tree is silent until the whole thing settles (the parent sees
exactly one notice — the real answer), and channel auto-delivery coalesces (a
follow-up message already queued at turn end holds delivery until the
follow-up turn's answer covers both). **Errors bypass the hold**: a parked
parent must get an error result rather than hang on a subtree that never
settles.

## The cursor is the only delivery state

Each edge keeps the last-delivered entry id, stamped inside the delivery
transaction (replacing the old `sessions.parent_notified_at` stamp). Cursor
behind an idle session's assistant leaf = delivery owed — the predicate
`advance_sweep_undelivered()` (daemon tick) re-derives from: lost pushes
re-deliver, quiescent holds re-check their subtree (this is how a subtree
that settles *silently* — an `explicit` grandchild finishing — still
delivers), unfired one-shots fire. The sweep skips `explicit` edges (for a
channel edge: a chat whose mode is `explicit` now). A boundary that ships
nothing (empty digest, suppressed channel error, an `explicit` chat) still
advances the cursor — "evaluated" is recorded, or the sweep would re-pick it
forever, and a chat flipped from `explicit` to `auto` later would get the
answer of a turn that was never meant to post. Pushes are latency, the sweep is the guarantee, recovery
(`db_recover_stale_sessions`) handles the dead. **No delivery-edge timeouts
in milestone 1** — blocking launches have no clock, exactly the prior
doctrine.

## The channel edge is created at bind

A session that belongs to a chat has its channel edge from that moment, made
in the same transaction that stamps `sessions.channel_name`
(`delivery_edge_bind_channel`): every pin (`channel_pin_session` /
`channel_pin_new_session` — first contact, `/new`, `cclaw route add`, the
explorer, `request_config` routes, and the fresh session a pin gives the chat
it takes a session from) and a chat-bound cron `target:"new"` fire. The
cursor is the session's `leaf_id` at that moment, 0 when it has no entries:
nothing it already said is posted, and everything it says after is owed.
That is what closes the first-turn window — the state flips to `idle` before
`advance_deliver_boundary()` commits, and a crash between the two used to
leave a bound session with no edge, which the sweep (it joins
`delivery_edges`) could never see. A session pinned again keeps its edge and
cursor; one moved to another chat keeps its cursor (what was delivered
elsewhere is not re-sent). The boundary still ensures the edge, at cursor 0,
as a backstop for a session stamped some other way — that boundary's answer
ships.

## Blocking: one-shot + standing, same transaction

Every child has its standing parent edge; a blocking launch adds the one-shot
`tool_call` edge. When the one-shot fires (`oneshot_resolve`: ToolResult to
the parent's call, call → done, parent unparked, edge row deleted), the
standing edge's cursor advances **in the same transaction** — the same
content never ships twice, and a blocking child that keeps working after
unblock ships its later results as ordinary parent notices (the closed
multi-turn lost-final-answer gap). The one-shot CASes on the call still being
open: if recovery already answered it synthetically, the edge just dies and
the child's real answer rides the standing edge.

## Labels are derived, not stored

The parent-edge prefix comes from the boundary state at delivery time:
subtree quiescent → `Sub-agent completed:`; not quiescent (only reachable
under turn-boundary policies) → `Sub-agent update:`; error → `Sub-agent
failed:`. `source='agent_result'`, `source_ref=<child sid>` as before.

## Defaults — policy inherits down the tree

| Edge | Config surface | Default |
|------|----------------|---------|
| parent | `launch_agent` arg `delivery` overrides; otherwise the launcher's own outbound-edge policy, copied at launch and frozen (`tool_filter` precedent, `session_create_filtered`) | registry `agent_delivery_default` (`quiescent`) seeds roots and the children of `explicit` sessions |
| channel | `channel_chats.delivery_mode` of the session's chat, read live at every boundary — an edit applies to the next turn; `cclaw route add --mode`, `request_config` routes (`explicit`) | a new chat: `explicit` if its id is group-shaped, else `auto` (= `quiescent`); a cron `target:"new"` session reads the mode of the chat it reports to |

## Edge cases that are rules

- **A failed turn ships its error text to the operator only**: a chat that is
  not an admin's own gets a rate-limited generic line. The rule and
  its table are in
  [error-handling.md](error-handling.md#a-turn-that-ends-in-an-error). An
  LLM-error boundary evaluates with channel edges suppressed (no answer
  ships, cursor still advanced) and reports the error on the way; an
  `explicit` edge does the same for any failed turn.
- **Pause fail-notify**: the autonomy streak guard's trip resolves the paused
  session's one-shot reply edge with an error ("paused by autonomy guard") —
  a paused session never settles, and recovery never fires for a
  legitimately idle child. The standing edge is left alone; the real answer
  ships after a human revives the session. The trip's note is read through to
  the answered turn before it, which keeps the sweep away in the meantime.
- **`check_session` poll-consume** advances the standing edge's cursor to the
  child's leaf — collected-by-poll and pushed are the same fact.
- **Synthetic answers drop one-shots**: recovery and the D11 turn-start
  reconcile delete the one-shot edges of calls they close
  (`drop_oneshot_edges_for_call`).

## Migration (v42) — history

`delivery_edges` created; every existing child got a standing parent edge
frozen at `turn` — the contract it was launched under; the quiescent default
applies to new launches only. Non-NULL `parent_notified_at` → cursor at the
current leaf; NULL → 0 (still owed, sweep re-derives). In-flight blocking
children got their one-shot edges. The column was dropped. Channel edges
materialize lazily, so routed chats picked up `auto`→`quiescent` on their
next turn — the one deliberate behavior change.

## Migration (v61) — history

The channel edge stopped carrying a policy: every existing channel edge lost
its frozen copy, and the chat's `delivery_mode` (the old route template)
became the live value. Cursors: under v60 an `explicit` boundary never
advanced the cursor, so an edge that never posted sat far behind its leaf
and would have posted an old answer on the first flip to `auto`. The patch
moved the cursor of every channel edge frozen `explicit`, or whose chat is
`explicit` now, to its session's `leaf_id` — exactly what an explicit
boundary stamps today. Every other edge kept its cursor: if it was behind,
that delivery was owed under v60 too, and the sweep makes it. Where a session's frozen copy
differed from its route — the route was edited after the session's first
delivery — the route's value now applies; the patch logs each such chat. A
channel edge's `target_ref` was set to its session's channel.

## Migration (v69) — history

Channel edges stopped materializing lazily (see "The channel edge is created
at bind"). Every chat-bound session without a channel edge got one at its
current `leaf_id` (0 for none), so the upgrade re-posts no old answer —
teaching the sweep to look at edgeless sessions instead would have. The cost:
a reply already cut off by a crash in the old window before the upgrade stays
lost. Data only; the table's shape did not change.
