#define _GNU_SOURCE
#include "landlock.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/landlock.h>

/* Syscall wrappers — glibc doesn't provide them yet */
static int ll_create_ruleset(struct landlock_ruleset_attr *attr, size_t size, __u32 flags) {
    return (int)syscall(__NR_landlock_create_ruleset, attr, size, flags);
}

static int ll_add_rule(int fd, enum landlock_rule_type type, void *attr, __u32 flags) {
    return (int)syscall(__NR_landlock_add_rule, fd, type, attr, flags);
}

static int ll_restrict_self(int fd, __u32 flags) {
    return (int)syscall(__NR_landlock_restrict_self, fd, flags);
}

/* All filesystem access rights we handle (deny by default) */
#define ACCESS_ALL ( \
    LANDLOCK_ACCESS_FS_EXECUTE | \
    LANDLOCK_ACCESS_FS_WRITE_FILE | \
    LANDLOCK_ACCESS_FS_READ_FILE | \
    LANDLOCK_ACCESS_FS_READ_DIR | \
    LANDLOCK_ACCESS_FS_REMOVE_DIR | \
    LANDLOCK_ACCESS_FS_REMOVE_FILE | \
    LANDLOCK_ACCESS_FS_MAKE_CHAR | \
    LANDLOCK_ACCESS_FS_MAKE_DIR | \
    LANDLOCK_ACCESS_FS_MAKE_REG | \
    LANDLOCK_ACCESS_FS_MAKE_SOCK | \
    LANDLOCK_ACCESS_FS_MAKE_FIFO | \
    LANDLOCK_ACCESS_FS_MAKE_BLOCK | \
    LANDLOCK_ACCESS_FS_MAKE_SYM)

#define ACCESS_READ (LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_EXECUTE)

#define ACCESS_RW (ACCESS_ALL)

static int add_path_rule(int ruleset_fd, const char *path, __u64 access) {
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) return -1;  /* path doesn't exist — skip silently */

    struct landlock_path_beneath_attr attr = {
        .allowed_access = access,
        .parent_fd = fd,
    };
    int rc = ll_add_rule(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &attr, 0);
    close(fd);
    return rc;
}

int landlock_apply(const char *workspace_path, const char *db_path) {
    /* Check if landlock is supported */
    int abi = ll_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 0) {
        fprintf(stderr, "[landlock] not available (kernel support missing): %s\n", strerror(errno));
        return -1;
    }

    struct landlock_ruleset_attr attr = { .handled_access_fs = ACCESS_ALL };
    int ruleset_fd = ll_create_ruleset(&attr, sizeof(attr), 0);
    if (ruleset_fd < 0) {
        fprintf(stderr, "[landlock] create_ruleset failed: %s\n", strerror(errno));
        return -1;
    }

    /* Workspace: full read-write */
    if (workspace_path && add_path_rule(ruleset_fd, workspace_path, ACCESS_RW) < 0) {
        fprintf(stderr, "[landlock] warning: cannot add workspace rule for %s: %s\n",
                workspace_path, strerror(errno));
    }

    /* DB file: read+write (WAL needs write access) */
    if (db_path && add_path_rule(ruleset_fd, db_path, LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE) < 0) {
        fprintf(stderr, "[landlock] warning: cannot add db rule for %s: %s\n",
                db_path, strerror(errno));
    }

    /* System read-only paths */
    static const char *readonly_paths[] = {
        "/usr",
        "/etc",
        "/lib",
        "/lib64",
        "/run",
        "/proc",
        "/dev/null",
        "/dev/urandom",
        "/tmp",
        NULL
    };
    for (int i = 0; readonly_paths[i]; i++) {
        add_path_rule(ruleset_fd, readonly_paths[i], ACCESS_READ);
    }

    /* Enforce — requires no_new_privs */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        fprintf(stderr, "[landlock] prctl(NO_NEW_PRIVS) failed: %s\n", strerror(errno));
        close(ruleset_fd);
        return -1;
    }

    if (ll_restrict_self(ruleset_fd, 0) < 0) {
        fprintf(stderr, "[landlock] restrict_self failed: %s\n", strerror(errno));
        close(ruleset_fd);
        return -1;
    }

    close(ruleset_fd);
    return 0;
}
