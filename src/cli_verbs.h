#ifndef CCLAW_CLI_VERBS_H
#define CCLAW_CLI_VERBS_H

/* Operator CLI verbs (cclaw sensitive/secret/route/channel/resp/backup/
 * dashboard). Each opens the real DB itself via a schema-guarded opener;
 * none touch the daemon/CLI globals in main.c. */

int sensitive_main(int argc, char *argv[]);
int secret_bind_main(int argc, char *argv[]);
int secret_main(int argc, char *argv[]);
int channel_cli_main(int argc, char *argv[]);
int route_main(int argc, char *argv[]);
int dashboard_main(void);
int backup_main(int argc, char *argv[]);
int resp_main(int argc, char *argv[]);

#endif
