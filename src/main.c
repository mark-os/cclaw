#include <stdio.h>
#include <string.h>
#include "cclaw.h"
#include "config.h"
#include "cli.h"

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

    if (!cli_mode) {
        /* Daemon mode not yet implemented */
        printf("cclaw %s\n", CCLAW_VERSION);
        printf("usage: cclaw --cli [config.json]\n");
        return 0;
    }

    Config *cfg = config_load(config_path);
    if (!cfg) {
        fprintf(stderr, "error: failed to load config\n");
        return 1;
    }

    if (debug_mode) cfg->debug = 1;

    int rc = cli_run(cfg);
    config_free(cfg);
    return rc == 0 ? 0 : 1;
}
