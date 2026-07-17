#ifndef CCLAW_WAKE_H
#define CCLAW_WAKE_H

#include <stdint.h>

/* Message written to the wake pipe by threads/externals. */
typedef struct {
    int64_t session_id;
} WakeMsg;

/* Internal wake pipe (threads → epoll loop). */
int wake_init(void);
int wake_fd(void);
int wake_session(int64_t session_id);
void wake_close(void);

int wake_fifo_open(const char *db_path);
void wake_fifo_close(int fd, const char *db_path);
int wake_external(const char *db_path);

#endif
