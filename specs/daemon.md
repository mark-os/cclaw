# Daemon + Fork Architecture

## Roles

| Component | Responsibility | ⊥ Does |
|-----------|---------------|--------|
| Daemon | fork agents, reap children, dispatch on exit code, deliver responses, enforce limits, write agent inboxes, spawn log collector | LLM calls, tool exec, context build |
| Agent process | drain inbox, run LLM loop, write entries to own DB, exit w/ code | fork other agents, deliver to channels, write daemon.db |
| Log collector | receive stdout/stderr from all processes via fd passing, write journal.db | anything else |
| Channel threads | inbox_insert (to agent DB) + signal daemon | run agent logic, write entries directly |
| CLI | in-process agent (no daemon), opens agent DB directly | multi-session concurrency, spawn_agent, approvals |

## Exit Code Protocol

| Code | Meaning | Daemon Action |
|------|---------|---------------|
| 0 | Turn complete, deliver response | Read last assistant entry from agent DB → deliver to channel |
| 1 | Turn complete with error | Log error, mark session idle |
| 2 | Spawn sub-agent requested | Read last tool_call from agent DB → fork child |
| 3 | Approval requested | Read last tool_call from agent DB → notify admin |
| 4 | Config change requested | Read last tool_call from agent DB → validate + apply |
| 127 | exec failed | Log error |
| 128+N | Killed by signal N | Log crash, synthesize error |

Agent sets own session state before exit:
- Exit 0/1: state → "idle"
- Exit 2/3: state → "waiting" (agent resumes when daemon delivers result to inbox)

## Turn Lifecycle

```
1. Message arrives (Telegram, cron, webhook, sub-agent completion, CLI)
2. Daemon: daemon_inbox_insert(agent_name, session_id, source, payload)
   → opens agent's DB, inserts inbox row, closes
3. Daemon: write(signal_pipe, &session_id, sizeof(int64))
4. Daemon: epoll wakes on signal_pipe readable
5. Daemon: read session_id, open agent DB, check session state == idle?
6. If idle: fork_agent(agent_name, session_id)
7. Agent process:
   a. Read CCLAW_* env vars (config from daemon)
   b. Open CCLAW_AGENT_DB
   c. landlock_apply() from env-parsed policy
   d. setrlimit(RLIMIT_AS, RLIMIT_CPU, RLIMIT_NOFILE)
   e. session state → "running"
   f. inbox_consume_into_entries(session_id) — atomic, sets last_route
   g. context_build(session_id)
   h. agent_loop() until stop_reason == "stop" or sentinel
   i. session state → "idle" (or "waiting" for exit 2/3)
   j. _exit(code)
8. Daemon: SIGCHLD → waitpid(WNOHANG) → reap
9. Daemon: dispatch on exit code:
   - 0: read last_route + last assistant entry from agent DB → deliver
   - 2: read last tool_call → insert spawn_queue → fork sub-agent
   - 3: read last tool_call → insert approvals → notify admin
   - 4: read last tool_call → validate + apply config
   - 1/signal: log error
10. Daemon: inbox_peek(agent DB, session_id) → items pending? → fork again
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
            while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                AgentInfo *info = lookup_agent_for_pid(pid);
                int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
                dispatch_exit_code(info->agent_name, info->session_id, code);
                remove_from_running(pid);
            }
        }
        else if (fd == signal_pipe[0]) {
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
void fork_agent(const char *agent_name, int64_t session_id) {
    // Read config from daemon.db agent_config table
    AgentConfig cfg = agent_config_load(daemon_db, agent_name);

    // Create pipe for log collector
    int log_pipe[2];
    pipe(log_pipe);
    send_fd_to_collector(collector_sock, log_pipe[0], agent_name, session_id);

    pid_t pid = fork();
    if (pid == 0) {
        // Child — set env vars
        setenv("CCLAW_AGENT_NAME", agent_name, 1);
        setenv("CCLAW_AGENT_DB", cfg.db_path, 1);
        setenv("CCLAW_WORKSPACE", cfg.workspace, 1);
        setenv("CCLAW_MODEL", cfg.model, 1);
        setenv("CCLAW_MAX_ITERATIONS", cfg.max_iterations_str, 1);
        setenv("CCLAW_ALLOWED_HOSTS", cfg.allowed_hosts_csv, 1);
        setenv("CCLAW_TOOLS", cfg.tools_csv, 1);
        setenv("CCLAW_SHELL_TIMEOUT", cfg.shell_timeout_str, 1);
        setenv("CCLAW_INJECTED_API_KEY", decrypted_key, 1);
        if (cfg.daemon_db_read)
            setenv("CCLAW_DAEMON_DB", daemon_db_path, 1);

        // Redirect stdout/stderr to log pipe
        dup2(log_pipe[1], STDOUT_FILENO);
        dup2(log_pipe[1], STDERR_FILENO);
        close(log_pipe[0]); close(log_pipe[1]);

        close(signal_pipe[0]); close(signal_pipe[1]); close(epfd);
        prctl(PR_SET_PDEATHSIG, SIGTERM);  // V34

        // Agent reads config from env, opens own DB
        apply_landlock_from_env();
        apply_rlimits();
        run_agent_turn(session_id);
        // run_agent_turn calls _exit(code)
    }
    close(log_pipe[1]);  // daemon doesn't write to agent's pipe
    add_to_running(pid, agent_name, session_id);
}
```

## Log Collector

Spawned once at daemon startup. Long-lived child process.

```
Daemon ←──socketpair──→ Log Collector
                              │
                              ├── epoll on received fds
                              ├── read lines from agent pipes
                              ├── batch INSERT to journal.db
                              └── flush every 100ms or 64 lines
```

- Daemon creates unix socketpair at startup, forks collector
- On each agent fork: daemon creates pipe, dups write-end to child stdout/stderr, sends read-end to collector via `SCM_RIGHTS` with metadata (agent_name, session_id, pid)
- Daemon's own stdout/stderr also piped to collector (source="daemon")
- Collector crash ⊥ kill agents (pipe write gets EPIPE, agent ignores)
- Collector is unsandboxed (needs journal.db write access)

## Env-Var Config Injection

Daemon reads `agent_config` table from daemon.db at fork time. Injects as env vars:

| Env Var | Source | Notes |
|---------|--------|-------|
| `CCLAW_AGENT_NAME` | agent identity | |
| `CCLAW_AGENT_DB` | path to agent.db | |
| `CCLAW_WORKSPACE` | agent_config.workspace | |
| `CCLAW_MODEL` | agent_config.model | |
| `CCLAW_MAX_ITERATIONS` | agent_config.max_iterations | |
| `CCLAW_ALLOWED_HOSTS` | agent_config.allowed_hosts | comma-separated |
| `CCLAW_TOOLS` | agent_config.tools | comma-separated |
| `CCLAW_SHELL_TIMEOUT` | agent_config.shell_timeout | seconds |
| `CCLAW_INJECTED_API_KEY` | daemon.db kv (decrypted) | provider API key |
| `CCLAW_DAEMON_DB` | daemon.db path | only if daemon_db_read=1 |

Agent reads env vars at startup. ⊥ opens config files. ⊥ opens daemon.db for config.

## Landlock Policy

Daemon reads from agent_config: workspace, read_access, landlock_net_ports.
Passes via env: `CCLAW_WORKSPACE`, `CCLAW_READ_ACCESS` (colon-separated), `CCLAW_NET_PORTS`.
Agent reads env, calls `landlock_apply()`.

```
Writable:  agents/<name>/ (DB + workspace) + /tmp/cclaw-<session_id>/
Readable:  /usr/lib, /usr/share, /etc/ssl, /etc/resolv.conf, daemon.db (if granted)
Network:   TCP connect to configured ports (ABI v4+, kernel 6.7+)
Denied:    everything else
```

Fallback: if kernel lacks landlock, log warning, continue without (V22).

## Resource Limits

| Limit | Default | Rationale |
|-------|---------|-----------|
| RLIMIT_AS | 256 MB | prevent runaway malloc |
| RLIMIT_CPU | 300s | kill stuck agents |
| RLIMIT_NOFILE | 64 | DB fd + curl sockets + workspace files |

## Blocking Sub-Agents (Exit Code 2)

1. Parent LLM returns tool_call: `spawn_agent(blocking=true, ...)`
2. Parent writes assistant entry (with tool_call) to own DB
3. Parent sets session state → "waiting", exits with code 2
4. Daemon reaps, reads tool_call from parent's agent DB
5. Daemon inserts spawn_queue row in daemon.db
6. Daemon forks sub-agent (own session, own agent DB, own sandbox)
7. Sub-agent completes → exits 0
8. Daemon reaps sub-agent, reads result from sub-agent's DB
9. Daemon writes tool_result to parent's inbox (in parent's agent DB)
10. Daemon transitions parent state "waiting" → "idle", forks parent
11. Parent drains inbox: sub-agent result → tool_result entry → resumes LLM loop

**State machine:**
```
idle ──[fork]──→ running ──[exit 0]──→ idle
                     │
                     └──[exit 2/3]──→ waiting ──[result in inbox]──→ idle
```

**Cost:** Zero idle processes. Parent memory fully reclaimed while waiting.

## Approval Flow (Exit Code 3)

1. Agent calls `approval_request(type, payload)`
2. Agent writes tool_call to own DB, sets state → "waiting", exits with code 3
3. Daemon reaps, reads tool_call, inserts into daemon.db approvals table
4. Daemon sends inline keyboard to admin via Telegram
5. Admin approves/denies → daemon writes result to agent inbox → state idle → signal
6. Agent re-forks, drains inbox, receives approval result as tool_result

## CLI Exception

CLI runs agent in-process. No daemon needed for 1:1 interactive use.
Opens agent DB directly. Config from env vars or defaults.
Same logical flow (inbox_insert → consume → agent_run → print response).
Lacks: spawn_agent, cron, approval_request (no daemon to handle exit codes).
Keeps: shell, file, js, web_fetch, db_query, memory tools.

## Route Pinning

`last_route TEXT` on sessions — updated during inbox consumption from newest item's `source`.

Format: `telegram:<chat_id>`, `cli`, `cron:<job>`, `subagent:<sid>`

Daemon reads from agent DB after reaping child to decide delivery target.

## Daemon Startup Recovery

1. Scan all agent DBs for sessions with state != "idle"
2. "running" → children already dead (daemon restarted) → reset to "idle"
3. "waiting" w/ result in inbox → reset to "idle" (will fork on next signal)
4. "waiting" w/o result → write error tool_result to inbox, reset to "idle"
