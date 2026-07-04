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

/* draft|broken → validated (on a passing --check). Returns 0 on success,
 * -1 if the channel doesn't exist or isn't in an allowed source state. */
int channel_mark_validated(sqlite3 *db, const char *name);

/* validated → active (on --activate). Returns 0 on success, -1 if the
 * channel doesn't exist or isn't 'validated'. */
int channel_activate(sqlite3 *db, const char *name);

/* Current lifecycle status ("draft"/"validated"/"active"/"broken"), or NULL
 * if the channel doesn't exist. Caller frees. */
char *channel_get_status(sqlite3 *db, const char *name);

/* Reap — returns 1 if pid belonged to a channel, 0 otherwise */
int channel_reap(pid_t pid, sqlite3 *db);

/* Tick — restart channels whose backoff expired, reset healthy ones */
void channel_tick(sqlite3 *db);

/* Channel events — process pending channel_events rows */
void channel_consume_events(sqlite3 *db);

/* Earliest next_restart_at across pending channels (0 = none pending) */
time_t channel_next_deadline(void);

#endif
