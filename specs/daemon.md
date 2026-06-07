# Daemon + Fork Architecture

## Roles

| Component | Responsibility | ⊥ Does |
|-----------|---------------|--------|
| Daemon | fork agents, reap children, dispatch on exit code, deliver responses, enforce limits, write inboxes | LLM calls, tool exec, context build |
| Agent process | drain inbox, run LLM loop, write entries to DB, exit w/ code | fork other agents, deliver to channels, write coordination tables |
| Channel processes | emit events to cclaw.db + wake daemon | run agent logic, write entries directly |
| CLI | in-process agent (no daemon), opens cclaw.db directly | multi-session concurrency, spawn_agent, approvals |

## Exit Code Protocol

| Code | Meaning | Daemon Action |
|------|---------|---------------|
| 0 | Turn complete, deliver response | Read last assistant entry from DB → deliver to channel |
| 1 | Turn complete with error | Log error, mark session idle |
| 2 | Spawn sub-agent requested | Read last tool_call from DB → fork child |
| 3 | Approval requested | Read last tool_call from DB → notify admin |
| 4 | Config change requested | Read last tool_call from DB → validate + apply |
| 127 | exec failed | Log error |
| 128+N | Killed by signal N | Log crash, synthesize error |

Agent sets own session state before exit:
- Exit 0/1: state → "idle"
- Exit 2/3: state → "waiting" (agent resumes when daemon delivers result to inbox)

## Turn Lifecycle

```
1. Message arrives (Telegram, cron, webhook, sub-agent completion, CLI)
2. Daemon: inbox_insert(session_id, source, payload) in cclaw.db
3. Daemon: write(signal_pipe, &session_id, sizeof(int64))
4. Daemon: epoll wakes on signal_pipe readable
5. Daemon: read session_id, check session state == idle?
6. If idle: fork_agent(agent_name, session_id)
7. Agent process:
   a. Read CCLAW_* env vars (config from daemon)
   b. Open CCLAW_DB (cclaw.db)
   c. setrlimit(RLIMIT_AS, RLIMIT_CPU, RLIMIT_NOFILE)
   e. session state → "running"
   f. inbox_consume_into_entries(session_id) — atomic, sets last_route
   g. context_build(session_id)
   h. agent_loop() until stop_reason == "stop" or sentinel
   i. session state → "idle" (or "waiting" for exit 2/3)
   j. _exit(code)
8. Daemon: SIGCHLD → waitpid(WNOHANG) → reap
9. Daemon: dispatch on exit code:
   - 0: read last_route + last assistant entry → deliver
   - 2: read last tool_call → fork sub-agent
   - 3: read last tool_call → notify admin
   - 4: read last tool_call → validate + apply config
   - 1/signal: log error
10. Daemon: inbox_peek(session_id) → items pending? → fork again
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
    // Read config from cclaw.db agent_config table
    AgentConfig cfg = agent_config_load(daemon_db, agent_name);

    // Create pipe for stderr capture
    int log_pipe[2];
    pipe(log_pipe);

    pid_t pid = fork();
    if (pid == 0) {
        // Child — set env vars
        setenv("CCLAW_AGENT_NAME", agent_name, 1);
        setenv("CCLAW_DB", db_path, 1);
        setenv("CCLAW_WORKSPACE", cfg.workspace, 1);
        setenv("CCLAW_MODEL", cfg.model, 1);
        setenv("CCLAW_MAX_ITERATIONS", cfg.max_iterations_str, 1);
        setenv("CCLAW_ALLOWED_HOSTS", cfg.allowed_hosts_csv, 1);
        setenv("CCLAW_TOOLS", cfg.tools_csv, 1);
        setenv("CCLAW_SHELL_TIMEOUT", cfg.shell_timeout_str, 1);
        setenv(cfg.provider.api_key_env, decrypted_key, 1);  // e.g. OPENROUTER_API_KEY

        // Redirect stderr to log pipe (parent drains → syslog)
        dup2(log_pipe[1], STDERR_FILENO);
        close(log_pipe[0]); close(log_pipe[1]);

        close(signal_pipe[0]); close(signal_pipe[1]); close(epfd);
        prctl(PR_SET_PDEATHSIG, SIGTERM);  // V34

        // Agent reads config from env, opens CCLAW_DB
        apply_rlimits();
        run_agent_turn(session_id);
        // run_agent_turn calls _exit(code)
    }
    close(log_pipe[1]);  // daemon reads child stderr from log_pipe[0] → syslog
    add_to_running(pid, agent_name, session_id);
}
```

## Process I/O Model (stdout vs stderr)

Agent processes use two distinct output channels:

| fd | Purpose | Content | Destination (daemon) | Destination (CLI) |
|----|---------|---------|---------------------|-------------------|
| stdout | Streaming tokens | SSE-parsed LLM output (CLI streaming mode) | /dev/null (daemon delivers via channel post-exit) | Inherited — displays directly to terminal |
| stderr | Structured logs | `LOG_*` macro output: `HH:MM:SS.mmm [LEVEL] message` | pipe → parent drains → syslog | pipe → parent drains → tee to terminal |

**Key design points:**
- Agent code writes logs exclusively via `LOG_*` macros (stderr). Never `printf` to stdout.
- Daemon mode: parent drains child stderr pipe, forwards to syslog (`LOG_DAEMON` facility; `sd_journal_send` if journald available).
- CLI mode: parent drains child stderr pipe, tees to terminal (if `--verbose` or log_level ≥ debug). stdout inherited for streaming display.
- Log level controls what agent writes to stderr. Display level (CLI `--verbose`) controls what parent echoes.

## Env-Var Config Injection

Daemon reads `agent_config` table from cclaw.db at fork time. Injects as env vars:

| Env Var | Source | Notes |
|---------|--------|-------|
| `CCLAW_AGENT_NAME` | agent identity | |
| `CCLAW_DB` | path to cclaw.db | |
| `CCLAW_WORKSPACE` | agent_config.workspace | |
| `CCLAW_MODEL` | agent_config.model | |
| `CCLAW_MAX_ITERATIONS` | agent_config.max_iterations | |
| `CCLAW_ALLOWED_HOSTS` | agent_config.allowed_hosts | comma-separated |
| `CCLAW_TOOLS` | agent_config.tools | comma-separated |
| `CCLAW_SHELL_TIMEOUT` | agent_config.shell_timeout | seconds |
| `CCLAW_LOG_LEVEL` | cclaw.db kv `log_level` | trace\|debug\|info\|error |
| Provider API key (e.g. `OPENROUTER_API_KEY`) | cclaw.db kv (decrypted) | injected as native env var |
| `CCLAW_DAEMON_DB` | cclaw.db path | only if daemon_db_read=1 |

Agent reads env vars at startup. ⊥ opens config files. ⊥ opens cclaw.db for config (reads `CCLAW_DB` for data access).

## Resource Limits

| Limit | Default | Rationale |
|-------|---------|-----------|
| RLIMIT_AS | 256 MB | prevent runaway malloc |
| RLIMIT_CPU | 300s | kill stuck agents |
| RLIMIT_NOFILE | 64 | DB fd + curl sockets + workspace files |

## Blocking Sub-Agents (Exit Code 2)

1. Parent LLM returns tool_call: `spawn_agent(blocking=true, ...)`
2. Parent writes assistant entry (with tool_call) to DB
3. Parent sets session state → "waiting", exits with code 2
4. Daemon reaps, reads tool_call from DB
5. Daemon forks sub-agent (own session in cclaw.db, scoped by agent_name)
6. Sub-agent completes → exits 0
7. Daemon reaps sub-agent, reads result from DB
8. Daemon writes tool_result to parent's inbox in cclaw.db
9. Daemon transitions parent state "waiting" → "idle", forks parent
10. Parent drains inbox: sub-agent result → tool_result entry → resumes LLM loop

**State machine:**
```
idle ──[fork]──→ running ──[exit 0]──→ idle
                     │
                     └──[exit 2/3]──→ waiting ──[result in inbox]──→ idle
```

**Cost:** Zero idle processes. Parent memory fully reclaimed while waiting.

## Approval Flow (Exit Code 3)

1. Agent calls `approval_request(type, payload)`
2. Agent writes tool_call to DB, sets state → "waiting", exits with code 3
3. Daemon reaps, reads tool_call from cclaw.db
4. Daemon sends inline keyboard to admin via Telegram
5. Admin approves/denies → daemon writes result to agent inbox → state idle → signal
6. Agent re-forks, drains inbox, receives approval result as tool_result

## CLI Exception

CLI runs agent in-process. No daemon needed for 1:1 interactive use.
Opens cclaw.db directly. Config from env vars or defaults.
Same logical flow (inbox_insert → consume → agent_run → print response).
Lacks: spawn_agent, cron, approval_request (no daemon to handle exit codes).
Keeps: shell, file, js, web_fetch, db_query, memory tools.

## Route Pinning

`last_route TEXT` on sessions — updated during inbox consumption from newest item's `source`.

Format: `telegram:<chat_id>`, `cli`, `cron:<job>`, `subagent:<sid>`

Daemon reads from DB after reaping child to decide delivery target.

## Daemon Startup Recovery

1. Scan sessions table in cclaw.db for state != "idle"
2. "running" → children already dead (daemon restarted) → reset to "idle"
3. "waiting" w/ result in inbox → reset to "idle" (will fork on next signal)
4. "waiting" w/o result → write error tool_result to inbox, reset to "idle"
