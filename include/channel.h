#ifndef CCLAW_CHANNEL_H
#define CCLAW_CHANNEL_H

#include <sqlite3.h>
#include <sys/types.h>
#include <time.h>

#define CHANNEL_MAX 16
#define CHANNEL_MAX_RESTARTS 3
#define CHANNEL_FLAP_WINDOW 300   /* 3 crashes in 5 min = flapping */
#define CHANNEL_MAX_BACKOFF 60

typedef struct {
    pid_t pid;
    char name[64];
    int restart_count;
    time_t first_crash;       /* timestamp of first crash in current window */
    time_t next_restart_at;   /* 0 = not pending restart */
    time_t started_at;        /* when process was last launched */
} ChannelProc;

/* Lifecycle */
int channel_launch_all(sqlite3 *db);
void channel_shutdown_all(void);

/* Reap — returns 1 if pid belonged to a channel, 0 otherwise */
int channel_reap(pid_t pid, sqlite3 *db);

/* Tick — restart channels whose backoff expired, reset healthy ones */
void channel_tick(sqlite3 *db);

/* Channel events — process pending channel_events rows */
void channel_consume_events(sqlite3 *db);

#endif
