#define _GNU_SOURCE
#include "install.h"
#include "templates.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Run argv[0] with args, no shell — every arg here is a fixed literal or a
 * path we built ourselves, but fork+execvp is the Unix-native way regardless
 * (see AGENTS.md: trust the OS, don't hand-roll it). Returns the child's exit
 * code, or -1 if it couldn't be started. */
static int run_argv(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static char *self_exe_path(void) {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n < 0) return NULL;
    buf[n] = '\0';
    return strdup(buf);
}

/* Write content to path only if it doesn't already exist — never clobber a
 * user's edited env file or unit. Returns 1 if written, 0 if kept existing,
 * -1 on error. */
static int write_file_if_absent(const char *path, const char *content, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, mode);
    if (fd < 0) {
        if (errno == EEXIST) return 0;
        return -1;
    }
    size_t len = strlen(content);
    ssize_t written = write(fd, content, len);
    close(fd);
    return (written == (ssize_t)len) ? 1 : -1;
}

/* Write content to path unconditionally (used for the unit file — reinstalls
 * should refresh it, unlike the env file which holds user edits). */
static int write_file(const char *path, const char *content, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;
    size_t len = strlen(content);
    ssize_t written = write(fd, content, len);
    close(fd);
    return (written == (ssize_t)len) ? 0 : -1;
}

/* ── User-mode install (default) ────────────────────────────────── */

/* PATH_MAX-sized buffers make gcc's -Wformat-truncation flag these snprintfs
 * as possibly truncating, even though the inputs (getenv("HOME"), our own
 * fixed suffixes) never approach that length. Same guard as main.c's
 * extract_builtin_extensions; clang doesn't have this warning at all. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif
static int install_user(void) {
    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        fprintf(stderr, "error: HOME not set — cannot resolve ~/.cclaw or ~/.config/systemd/user\n");
        return 1;
    }

    char *exe = self_exe_path();
    if (!exe) {
        fprintf(stderr, "error: readlink(/proc/self/exe) failed: %s\n", strerror(errno));
        return 1;
    }

    char cclaw_dir[PATH_MAX], env_path[PATH_MAX];
    char unit_dir[PATH_MAX], unit_path[PATH_MAX];
    snprintf(cclaw_dir, sizeof(cclaw_dir), "%s/.cclaw", home);
    snprintf(env_path, sizeof(env_path), "%s/env", cclaw_dir);
    snprintf(unit_dir, sizeof(unit_dir), "%s/.config/systemd/user", home);
    snprintf(unit_path, sizeof(unit_path), "%s/cclaw.service", unit_dir);

    if (util_mkdir_p(cclaw_dir) != 0) {
        fprintf(stderr, "error: mkdir %s failed: %s\n", cclaw_dir, strerror(errno));
        free(exe);
        return 1;
    }
    if (util_mkdir_p(unit_dir) != 0) {
        fprintf(stderr, "error: mkdir %s failed: %s\n", unit_dir, strerror(errno));
        free(exe);
        return 1;
    }

    int env_rc = write_file_if_absent(env_path, TPL_CCLAW_ENV_EXAMPLE, 0600);
    if (env_rc < 0) {
        fprintf(stderr, "error: writing %s failed: %s\n", env_path, strerror(errno));
        free(exe);
        return 1;
    }
    printf(env_rc ? "  wrote  %s\n" : "  kept   %s (already exists)\n", env_path);

    /* Unit content is generated, not templated from cclaw.service: the user
     * unit's ExecStart/EnvironmentFile/WantedBy differ from the root unit
     * (self-resolved binary path, %h-relative env, default.target instead of
     * multi-user.target since there's no root daemon to depend on). */
    char unit[2048];
    snprintf(unit, sizeof(unit),
        "[Unit]\n"
        "Description=CClaw autonomous AI agent daemon (user)\n"
        "After=network.target\n"
        "\n"
        "[Service]\n"
        "Type=simple\n"
        "ExecStart=%s --daemon\n"
        "EnvironmentFile=%%h/.cclaw/env\n"
        "Restart=on-failure\n"
        "RestartSec=5\n"
        "StandardOutput=journal\n"
        "StandardError=journal\n"
        "\n"
        "[Install]\n"
        "WantedBy=default.target\n",
        exe);
    free(exe);

    if (write_file(unit_path, unit, 0644) != 0) {
        fprintf(stderr, "error: writing %s failed: %s\n", unit_path, strerror(errno));
        return 1;
    }
    printf("  wrote  %s\n", unit_path);

    run_argv((char *const[]){"systemctl", "--user", "daemon-reload", NULL});
    int enable_rc = run_argv((char *const[]){"systemctl", "--user", "enable", "--now", "cclaw", NULL});

    /* Without linger, the user unit dies when the last session logs out.
     * Best-effort: warn, don't fail the install over it. */
    char *user = getenv("USER");
    if (!user) { struct passwd *pw = getpwuid(getuid()); user = pw ? pw->pw_name : NULL; }
    if (user) {
        if (run_argv((char *const[]){"loginctl", "enable-linger", (char *)user, NULL}) != 0)
            fprintf(stderr, "warning: loginctl enable-linger failed — the daemon will stop when you log out "
                            "(ask your admin, or ignore this on a single-user desktop)\n");
    }

    printf("\n");
    if (enable_rc == 0)
        printf("Installed and started: systemctl --user status cclaw\n");
    else
        printf("Unit written but `systemctl --user enable --now cclaw` failed (exit %d).\n"
               "Check manually: systemctl --user status cclaw\n", enable_rc);
    if (!env_rc)
        printf("Note: %s already existed — if the key probe fails, edit it and run:\n"
               "  systemctl --user restart cclaw\n", env_path);
    else
        printf("Next: edit %s (set your API key), then: systemctl --user restart cclaw\n"
               "Diagnose with: cclaw --doctor\n", env_path);
    return 0;
}

/* ── --system install (root) ───────────────────────────────────── */

static int install_system(void) {
    if (geteuid() != 0) {
        fprintf(stderr, "error: --system requires root (writes /usr/local/bin, /etc/cclaw, /etc/systemd/system)\n");
        return 1;
    }

    char *exe = self_exe_path();
    if (!exe) {
        fprintf(stderr, "error: readlink(/proc/self/exe) failed: %s\n", strerror(errno));
        return 1;
    }

    /* The daemon runs as the dedicated 'cclaw' user (User= in the unit,
     * setpriv in the init script) — it never needs root at runtime, and the
     * parent process is the one parsing untrusted input. */
    if (!getpwnam("cclaw")) {
        int urc = run_argv((char *const[]){"useradd", "--system", "-m",
                                           "-d", "/home/cclaw",
                                           "-s", "/usr/sbin/nologin",
                                           "-U", "cclaw", NULL});
        if (urc != 0) {
            fprintf(stderr, "error: useradd cclaw failed (exit %d)\n", urc);
            free(exe);
            return 1;
        }
        printf("  created user cclaw (home /home/cclaw)\n");
    }

    util_mkdir_p("/usr/local/bin");
    util_mkdir_p("/etc/cclaw");

    if (util_copy_file(exe, "/usr/local/bin/cclaw", 0755) != 0) {
        fprintf(stderr, "error: copying %s to /usr/local/bin/cclaw failed: %s\n", exe, strerror(errno));
        free(exe);
        return 1;
    }
    free(exe);
    printf("  wrote  /usr/local/bin/cclaw\n");

    int env_rc = write_file_if_absent("/etc/cclaw/env", TPL_CCLAW_ENV_EXAMPLE, 0600);
    if (env_rc < 0) {
        fprintf(stderr, "error: writing /etc/cclaw/env failed: %s\n", strerror(errno));
        return 1;
    }
    printf(env_rc ? "  wrote  /etc/cclaw/env\n" : "  kept   /etc/cclaw/env (already exists)\n");

    /* systemd if the host runs it, SysV init otherwise (e.g. Pogoplug's
     * Debian armel). /run/systemd/system existing is the documented way to
     * detect a running systemd (sd_booted(3)). */
    if (access("/run/systemd/system", F_OK) == 0) {
        if (write_file("/etc/systemd/system/cclaw.service", TPL_CCLAW_SERVICE, 0644) != 0) {
            fprintf(stderr, "error: writing /etc/systemd/system/cclaw.service failed: %s\n", strerror(errno));
            return 1;
        }
        printf("  wrote  /etc/systemd/system/cclaw.service\n");

        run_argv((char *const[]){"systemctl", "daemon-reload", NULL});

        printf("\nInstalled. Edit /etc/cclaw/env then: systemctl enable --now cclaw\n");
    } else {
        if (write_file("/etc/init.d/cclaw", TPL_CCLAW_INIT, 0755) != 0) {
            fprintf(stderr, "error: writing /etc/init.d/cclaw failed: %s\n", strerror(errno));
            return 1;
        }
        printf("  wrote  /etc/init.d/cclaw\n");

        if (run_argv((char *const[]){"update-rc.d", "cclaw", "defaults", NULL}) != 0)
            fprintf(stderr, "warning: update-rc.d failed — enable manually for your init system\n");

        printf("\nInstalled. Edit /etc/cclaw/env then: /etc/init.d/cclaw start\n");
    }
    return 0;
}

/* ── Uninstall ──────────────────────────────────────────────────── */

static int uninstall_user(void) {
    run_argv((char *const[]){"systemctl", "--user", "disable", "--now", "cclaw", NULL});
    const char *home = getenv("HOME");
    if (home && home[0]) {
        char unit_path[PATH_MAX];
        snprintf(unit_path, sizeof(unit_path), "%s/.config/systemd/user/cclaw.service", home);
        if (unlink(unit_path) == 0) printf("  removed %s\n", unit_path);
    }
    run_argv((char *const[]){"systemctl", "--user", "daemon-reload", NULL});
    printf("Uninstalled the user unit. ~/.cclaw (config, workspace, db) was left untouched.\n");
    return 0;
}

static int uninstall_system(void) {
    if (geteuid() != 0) {
        fprintf(stderr, "error: --system uninstall requires root\n");
        return 1;
    }
    if (access("/run/systemd/system", F_OK) == 0) {
        run_argv((char *const[]){"systemctl", "disable", "--now", "cclaw", NULL});
        if (unlink("/etc/systemd/system/cclaw.service") == 0)
            printf("  removed /etc/systemd/system/cclaw.service\n");
        run_argv((char *const[]){"systemctl", "daemon-reload", NULL});
    } else {
        run_argv((char *const[]){"/etc/init.d/cclaw", "stop", NULL});
        if (unlink("/etc/init.d/cclaw") == 0) {
            printf("  removed /etc/init.d/cclaw\n");
            run_argv((char *const[]){"update-rc.d", "cclaw", "remove", NULL});
        }
    }
    printf("Uninstalled the system unit. /etc/cclaw/env and /usr/local/bin/cclaw were left in place.\n");
    return 0;
}

/* ── Entry points ───────────────────────────────────────────────── */

int install_main(int argc, char *argv[]) {
    int system_mode = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--system") == 0) system_mode = 1;
        else { fprintf(stderr, "install: unknown option: %s\n", argv[i]); return 1; }
    }
    return system_mode ? install_system() : install_user();
}

int uninstall_main(int argc, char *argv[]) {
    int system_mode = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--system") == 0) system_mode = 1;
        else { fprintf(stderr, "uninstall: unknown option: %s\n", argv[i]); return 1; }
    }
    return system_mode ? uninstall_system() : uninstall_user();
}
