# Daemon + Inbox + Process Architecture

Formal specification. Derived from `notes/daemon-inbox-architecture.md` (design notes).

## Roles

| Component | Responsibility | ⊥ Does |
|-----------|---------------|--------|
| Daemon | fork agents, reap children, deliver responses, enforce limits | LLM calls, tool exec, context build |
| Agent process | drain inbox, run LLM loop, write entries, exit | fork other agents, deliver to channels |
| Channel threads | inbox_insert + signal daemon | run agent logic, write entries directly |
| CLI | in-process agent (no daemon), same loop code | multi-session concurrency |

## Chat Turn Lifecycle

```
1. Message arrives (Telegram, cron, webhook, sub-agent completion, CLI)
2. Inserter: inbox_insert(session_id, source, payload)
3. Inserter: write(signal_pipe, &session_id, sizeof(int64))
4. Daemon: epoll wakes on signal_pipe readable
5. Daemon: read session_id, check running_agents map → session idle?
6. If idle: fork_agent(session_id)
7. Agent process:
   a. landlock_restrict(agent.workspace)
   b. setrlimit(RLIMIT_AS, RLIMIT_CPU, RLIMIT_NOFILE)
   c. session_try_acquire(session_id, getpid())
   d. inbox_consume_into_entries(session_id)  — atomic, sets last_route
   e. context_build(session_id)
   f. agent_loop() until stop_reason == "stop"
   g. session_release(session_id)
   h. _exit(0)
8. Daemon: SIGCHLD → waitpid(WNOHANG) → reap
9. Daemon: read last_route from session → deliver final assistant message to channel
10. Daemon: inbox_peek(session_id) → items pending? → fork again immediately
```

## Daemon Main Loop

```c
// Initialization
int signal_pipe[2];  // inserters write session_id here
pipe2(signal_pipe, O_NONBLOCK | O_CLOEXEC);

int sigchld_pipe[2]; // SIGCHLD handler writes 1 byte
pipe2(sigchld_pipe, O_NONBLOCK | O_CLOEXEC);
// Install SIGCHLD handler that writes to sigchld_pipe[1]

int epfd = epoll_create1(EPOLL_CLOEXEC);
epoll_ctl(epfd, EPOLL_CTL_ADD, signal_pipe[0], &(struct epoll_event){.events=EPOLLIN});
epoll_ctl(epfd, EPOLL_CTL_ADD, sigchld_pipe[0], &(struct epoll_event){.events=EPOLLIN});

while (!shutdown) {
    struct epoll_event events[8];
    int n = epoll_wait(epfd, events, 8, -1);  // blocks indefinitely

    for (int i = 0; i < n; i++) {
        if (events[i].data.fd == sigchld_pipe[0]) {
            // Drain sigchld notifications
            char buf[64]; read(sigchld_pipe[0], buf, sizeof(buf));
            // Reap all finished children
            int status; pid_t pid;
            while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                int64_t sid = lookup_session_for_pid(pid);
                remove_from_running(pid);
                deliver_response(sid);
                if (inbox_has_pending(sid))
                    fork_agent(sid);
            }
        }
        else if (events[i].data.fd == signal_pipe[0]) {
            // Drain all signaled session_ids
            int64_t sid;
            while (read(signal_pipe[0], &sid, sizeof(sid)) == sizeof(sid)) {
                if (!session_has_active_agent(sid))
                    fork_agent(sid);
            }
        }
    }
}
```

## Signal Pipe Protocol

- Pipe created at daemon startup: `signal_pipe[2]`
- Write end passed to channel threads (Telegram, cron, civetweb handlers)
- Inserters write `int64_t session_id` after `inbox_insert`
- Daemon reads all available session_ids, deduplicates, forks idle ones
- Non-blocking reads — drain until EAGAIN

## Agent Process Lifecycle

```c
void fork_agent(int64_t session_id) {
    pid_t pid = fork();
    if (pid == 0) {
        // Child — agent process
        close(signal_pipe[0]);  // don't need read end
        close(signal_pipe[1]);  // don't signal self
        close(epfd);

        AgentConfig cfg = load_agent_config(session_id);
        apply_landlock(cfg.workspace);
        apply_rlimits(cfg.rlimits);

        if (!session_try_acquire(session_id, getpid()))
            _exit(1);  // someone else has it (shouldn't happen)

        inbox_consume_into_entries(session_id);  // atomic drain
        agent_run(session_id);                   // LLM loop until stop
        session_release(session_id);
        _exit(0);
    }
    // Parent — daemon
    add_to_running(pid, session_id);
}
```

## Landlock Policy (per agent)

```
Writable:  agents/<name>/workspace/   (or configured workspace path)
Readable:  /usr/lib, /usr/share, /etc/ssl, /etc/resolv.conf
           agents/<name>/              (config, skills, notes)
           cclaw.db                    (SQLite — needed for WAL)
Denied:    everything else (other agents' workspaces, /home, /tmp unless workspace)
```

Applied via `landlock_create_ruleset` + `landlock_add_rule` + `landlock_restrict_self`.
Fallback: if kernel < 5.13 or landlock not available, log warning, continue without.

## Resource Limits (per agent)

| Limit | Default | Rationale |
|-------|---------|-----------|
| RLIMIT_AS | 256 MB | prevent runaway malloc (LLM response parsing, tool output) |
| RLIMIT_CPU | 300s | kill stuck agents (infinite tool loops) |
| RLIMIT_NOFILE | 64 | agent needs: DB fd, curl sockets, workspace files |

Configurable per-agent in `agents/<name>/agent.json`:
```json
{ "rlimits": { "as_mb": 256, "cpu_sec": 300, "nofile": 64 } }
```

## Route Pinning

Sessions track `last_route TEXT` — updated during inbox consumption.

Format: `<channel>:<identifier>`
- `telegram:<chat_id>`
- `cli`
- `cron:<job_name>`
- `webhook:<source_id>`
- `subagent:<child_session_id>`

Daemon reads `last_route` after reaping child to decide delivery target.

Edge case: user sends Telegram msg, then types in CLI during active turn.
CLI message queues in inbox, consumed next turn, `last_route` flips to CLI.
Response delivered to CLI. Correct — respond where user last spoke.

## Sub-Agent Spawning (Daemon-Mediated)

```
1. Parent agent calls spawn_agent tool
2. Tool: INSERT INTO spawn_queue (parent_session_id, agent_name, task, mode)
3. Tool: write(signal_pipe, &SPAWN_SENTINEL)  — or dedicated spawn_pipe
4. Daemon: reads spawn request, forks sub-agent process
5. Sub-agent: runs in own session, own landlock, own rlimits
6. Sub-agent exits → daemon reaps
7. Sub-agent already wrote result to parent inbox (last step before exit)
8. Daemon: inbox_has_pending(parent_sid) && parent idle → fork parent
```

### Blocking Sub-Agents

Parent exits after posting spawn request. No idle processes, uniform lifecycle.

**Mechanism:**
1. Parent LLM returns tool_call: `spawn_agent(blocking=true, ...)`
2. Parent writes assistant entry (with tool_call, no tool_result yet — turn incomplete)
3. Parent writes spawn request to dispatch queue
4. Parent sets session state → "waiting"
5. Parent exits (exit code 0 — intentional, not a crash)
6. Daemon reaps parent, sees state == "waiting" → does NOT deliver response, does NOT re-fork
7. Daemon picks up spawn request, forks sub-agent
8. Sub-agent runs in own session, completes, writes result to parent inbox:
   `{source: "subagent:<child_session_id>", payload: {tool_call_id: "xyz", content: "..."}}`
9. Sub-agent exits → daemon reaps
10. Daemon transitions parent state "waiting" → "idle"
11. Daemon sees parent inbox has items → forks parent
12. Parent drains inbox:
    - Sub-agent result → written as tool_result entry (matched by tool_call_id)
    - Any other messages (Telegram, etc.) → queued as user entries for next turn
13. Parent context_build: sees assistant(tool_call) → tool_result → resumes LLM loop
14. LLM continues until stop_reason == "stop"
15. Parent exits normally, daemon delivers response

**State machine:**
```
idle ──[daemon forks]──→ running ──[agent exits normally]──→ idle
                              │
                              └──[spawn_agent blocking]──→ waiting
                                                              │
                              ┌──[sub-agent completes]────────┘
                              ▼
                            idle ──[daemon forks parent]──→ running ...
```

**Distinguishing crash from intentional exit:**
- Agent exits while state == "running" + no spawn request → normal completion or crash
- Agent exits while state == "waiting" → intentional (blocking sub-agent)
- V17: incomplete turn + state == "waiting" + last tool_call is spawn_agent → don't synthesize failure
- V17: incomplete turn + state != "waiting" → crash, synthesize failure notice

**Messages arriving while parent is waiting:**
They queue in inbox. When parent re-forks and drains inbox, it gets sub-agent result
(becomes tool_result) + any user messages (become entries for subsequent turns).
The sub-agent result is identified by having a `tool_call_id` field in its payload.

**Cost:** Zero. No idle processes. Parent memory fully reclaimed while waiting.
Sub-agent uses its own memory allocation. System-wide footprint = only active agents.

## CLI Exception

CLI runs agent in-process. No daemon needed for 1:1 interactive use.

```
1. User types message
2. CLI: inbox_insert(session_id, "cli", message)
3. CLI: session_try_acquire(session_id, getpid())
   - If fails: print "queued — session is busy" (daemon has it)
4. CLI: inbox_consume_into_entries(session_id)
5. CLI: agent_run(session_id)
6. CLI: print final assistant message
7. CLI: session_release(session_id)
```

Same logical flow as daemon-forked agent, just in-process.

## Daemon + CLI Coexistence

If daemon is running and user launches CLI on same session:
- CLI does inbox_insert (message saved regardless)
- CLI tries session_try_acquire → fails (daemon's agent has it)
- CLI prints "queued — session is busy, message will be processed next turn"
- Daemon's agent finishes → daemon checks inbox → sees CLI message → forks again
- Response delivered to `last_route` which is now "cli" — but CLI already exited?

**Resolution:** CLI in daemon mode should be a persistent process that:
1. Inserts to inbox + signals daemon
2. Watches for response (poll entries table or subscribe to a response pipe)
3. Prints response when it arrives

Or simpler: CLI always runs in-process (no daemon involvement for CLI sessions).
Separate sessions for CLI vs Telegram. No contention.

## Open Decisions

1. **Spawn pipe vs signal pipe reuse**: sub-agent spawn requests could use the same signal pipe (daemon checks spawn_queue on every wake) or a dedicated pipe. Same pipe is simpler.

2. **Response delivery mechanism**: daemon reads final assistant entry from DB and calls channel-specific send function (Telegram API, etc.). Daemon needs channel send code but not agent logic.

3. **Landlock + curl**: curl needs network access. Landlock v1-v4 don't restrict network. Network restriction would need seccomp or network namespaces. For now, landlock = filesystem only. Network restriction is §F (future).

4. **Multi-message turns**: agent may produce multiple assistant messages (between tool calls). Daemon delivers only the final one (last assistant entry in the turn). Intermediate messages are internal reasoning.
