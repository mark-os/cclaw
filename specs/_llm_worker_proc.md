# LLM Worker Process (Fork+Exec Pattern)

If process isolation is ever needed (crash safety, memory caps), the worker
should be fork+exec'd — never bare-forked with threads.

## Why bare fork + threads deadlocks

After `fork()`, only the calling thread survives. glibc's malloc arenas,
OpenSSL's lock table, and NSS/DNS resolver all have internal mutexes that
may have been held by other threads at fork time. When the child creates new
threads and they touch any of these subsystems, they deadlock on locks that
will never be released.

## Correct pattern

```c
// Parent:
pipe(ping_pipe);   // parent writes, worker reads
pipe(result_pipe); // worker writes, parent reads

pid = fork();
if (pid == 0) {
    close(ping_pipe[1]);
    close(result_pipe[0]);
    // Move to stable fd numbers, clear CLOEXEC
    dup2(ping_pipe[0], 100); close(ping_pipe[0]);
    dup2(result_pipe[1], 101); close(result_pipe[1]);
    fcntl(100, F_SETFD, 0);
    fcntl(101, F_SETFD, 0);
    execl("/proc/self/exe", "cclaw", "llm-worker",
          "100", "101", db_path, threads_str, NULL);
    _exit(1);
}
```

## Worker process requirements

- `prctl(PR_SET_PDEATHSIG, SIGTERM)` — die if parent dies
- `setrlimit(RLIMIT_AS, 512MB)` — memory cap
- Set ping fd to `O_NONBLOCK` (drain loop reads until EAGAIN)
- Own sqlite3 connections per thread (no sharing)
- Own CURL handles per thread
- Signal completion by writing session_id to result pipe

## Communication

- **Ping pipe**: parent writes 1 byte after inserting into `llm_jobs`
- **Result pipe**: worker writes 8-byte session_id when done
- **Job queue**: `llm_jobs` table with `UPDATE...RETURNING` claim SQL
- **WAL visibility**: not an issue after exec (fresh sqlite3 connections)

## Parallelism

Elastic thread pool inside the exec'd worker (1–N threads). Single
dispatcher thread claims from DB, pushes to in-memory queue. Worker
threads pop and execute. Scales to concurrent agent sessions in daemon mode.
