#ifndef CCLAW_CHANNEL_H
#define CCLAW_CHANNEL_H

#include <sqlite3.h>
#include <sys/types.h>

#define CHANNEL_MAX 16
#define CHANNEL_MAX_RESTARTS 3

typedef struct {
    pid_t pid;
    char name[64];
    int restart_count;
} ChannelProc;

/* Lifecycle */
int channel_launch_all(sqlite3 *db);
void channel_shutdown_all(void);

/* Reaping — returns 1 if pid belonged to a channel, 0 otherwise */

/* Registration */
int channel_register(sqlite3 *db, const char *name, const char *binary_path);

/* Channel events — process pending channel_events rows */
void channel_consume_events(sqlite3 *db);

#endif
