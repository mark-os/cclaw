# Daemon + Fork Architecture

## Roles

| Component | Responsibility | ⊥ Does |
|-----------|---------------|--------|
| Daemon | fork agents, reap children, deliver responses, enforce limits | LLM calls, tool exec, context build |
| Agent process | drain inbox, run LLM loop, write entries, exit | fork other agents, deliver to channels |
| Channel threads | inbox_insert + signal daemon | run agent logic, write entries directly |
| CLI | in-process agent (no daemon), same loop code | multi-session concurrency |

## Turn Lifecycle

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
   c. session state → "running"
   d. inbox_consume_into_entries(session_id)  — atomic, sets last_route
   e. context_build(session_id)
   f. agent_loop() until stop_reason == "stop"
   g. session state → "idle"
   h. _exit(0)
8. Daemon: SIGCHLD → waitpid(WNOHANG) → reap
9. Daemon: read last_route from session → deliver final assistant message to channel
10. Daemon: inbox_peek(session_id) → items pending? → fork again immediately
```

## Main Loop

```c
int signal_pipe[2];   // inserters write session_id here
int sigchld_pipe[2];  // SIGCHLD handler writes 1 byte

int epfd = epoll_create1(EPOLL_CLOEXEC);
epoll_ctl(epfd, EPOLL_CTL_ADD, signal_pipe[0], EPOLLIN);
epoll_ctl(epfd, EPOLL_CTL_ADD, sigchld_pipe[0], EPOLLIN);

while (!shutdown) {
    struct epoll_event events[8];
    int n = epoll_wait(epfd, events, 8, -1);

    for (int i = 0; i < n; i++) {
        if (fd == sigchld_pipe[0]) {
            // Reap all finished children
            while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                sid = lookup_session_for_pid(pid);
                remove_from_running(pid);
                deliver_response(sid);
                if (inbox_has_pending(sid))
                    fork_agent(sid);
            }
        }
        else if (fd == signal_pipe[0]) {
            // Drain all signaled session_ids
            while (read(signal_pipe[0], &sid, sizeof(sid)) == sizeof(sid)) {
                if (!session_has_active_agent(sid))
                    fork_agent(sid);
            }
        }
    }
}
```

## Agent Process

```c
void fork_agent(int64_t session_id) {
    pid_t pid = fork();
    if (pid == 0) {
        // Child
        close(signal_pipe[0]); close(signal_pipe[1]); close(epfd);
        prctl(PR_SET_PDEATHSIG, SIGTERM);  // V34

        AgentConfig cfg = load_agent_config(session_id);
        apply_landlock(cfg.workspace);
        apply_rlimits(cfg.rlimits);

        inbox_consume_into_entries(session_id);
        agent_run(session_id);
        _exit(0);
    }
    add_to_running(pid, session_id);
}
```

## Landlock Policy

```
Writable:  agent workspace + /tmp/cclaw-<session_id>/
Readable:  /usr/lib, /usr/share, /etc/ssl, /etc/resolv.conf, cclaw.db
Denied:    everything else
```

Fallback: if kernel lacks landlock (Pogoplug), log warning, continue without (V22).

## Resource Limits

| Limit | Default | Rationale |
|-------|---------|-----------|
| RLIMIT_AS | 256 MB | prevent runaway malloc |
| RLIMIT_CPU | 300s | kill stuck agents |
| RLIMIT_NOFILE | 64 | DB fd + curl sockets + workspace files |

## Blocking Sub-Agents

1. Parent LLM returns tool_call: `spawn_agent(blocking=true, ...)`
2. Parent writes assistant entry (with tool_call) + spawn request
3. Parent sets session state → "waiting", exits
4. Daemon reaps parent, sees "waiting" → ⊥ deliver, ⊥ re-fork
5. Daemon forks sub-agent (own session, own sandbox)
6. Sub-agent completes, writes result to parent inbox
7. Sub-agent exits → daemon reaps
8. Daemon transitions parent "waiting" → "idle", forks parent
9. Parent drains inbox: sub-agent result → tool_result entry
10. Parent context_build sees tool_call → tool_result → resumes LLM loop

**State machine:**
```
idle ──[fork]──→ running ──[exit]──→ idle
                     │
                     └──[spawn blocking]──→ waiting ──[sub-agent done]──→ idle
```

**Cost:** Zero idle processes. Parent memory fully reclaimed while waiting.

## CLI Exception

CLI runs agent in-process. No daemon needed for 1:1 interactive use.
Same logical flow (inbox_insert → consume → agent_run → print response).
If daemon is running on same DB, CLI uses separate sessions — no contention.

## Route Pinning

`last_route TEXT` on sessions — updated during inbox consumption from newest item's `source`.

Format: `telegram:<chat_id>`, `cli`, `cron:<job>`, `subagent:<sid>`

Daemon reads after reaping child to decide delivery target.
