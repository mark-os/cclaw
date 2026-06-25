#ifndef CCLAW_CHANNEL_RUNNER_H
#define CCLAW_CHANNEL_RUNNER_H

/* Run the universal JS channel loop in-process — this is the `cclaw --channel
 * <name>` mode. The daemon fork+execs `cclaw --channel <name>` (do_fork in
 * channel.c) to get a clean process image, then main() calls this directly; the
 * process shows as `cclaw --channel <name>` in ps. There is no separate runner
 * binary. Opens its own DB ctx from db_path, loads the channel's extension JS,
 * and runs the poll / outbox / request event loop until SIGTERM. Returns the
 * process exit code. */
int channel_runner_main(const char *db_path, const char *channel_name);

#endif
