#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "cclaw.h"
#include "config.h"
#include "cli.h"
#include "telegram.h"
#include "db.h"

static volatile int daemon_running = 1;

static void sighandler(int sig) {
    (void)sig;
    daemon_running = 0;
}

int main(int argc, char *argv[]) {
    int cli_mode = 0;
    int debug_mode = 0;
    const char *config_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--cli") == 0) {
            cli_mode = 1;
        } else if (strcmp(argv[i], "--debug") == 0) {
            debug_mode = 1;
        } else if (argv[i][0] != '-') {
            config_path = argv[i];
        }
    }

    Config *cfg = config_load(config_path);
    if (!cfg) {
        fprintf(stderr, "error: failed to load config\n");
        return 1;
    }
    if (debug_mode) cfg->debug = 1;

    if (cli_mode) {
        int rc = cli_run(cfg);
        config_free(cfg);
        return rc == 0 ? 0 : 1;
    }

    /* Daemon mode: start Telegram poller */
    if (!cfg->telegram_token || cfg->telegram_token[0] == '\0') {
        fprintf(stderr, "error: CCLAW_TELEGRAM_TOKEN not set (required for daemon mode)\n");
        config_free(cfg);
        return 1;
    }

    sqlite3 *db = db_open(cfg->db_path);
    if (!db) {
        fprintf(stderr, "error: cannot open database '%s'\n", cfg->db_path);
        config_free(cfg);
        return 1;
    }

    printf("cclaw %s — daemon mode\n", CCLAW_VERSION);

    if (telegram_start(cfg, db) != 0) {
        fprintf(stderr, "error: failed to start telegram poller\n");
        db_close(db);
        config_free(cfg);
        return 1;
    }
    printf("telegram poller started\n");

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    while (daemon_running) {
        sleep(1);
    }

    printf("\nshutting down...\n");
    telegram_stop();
    db_close(db);
    config_free(cfg);
    return 0;
}
