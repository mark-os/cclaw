#ifndef CCLAW_CHANNEL_HARNESS_H
#define CCLAW_CHANNEL_HARNESS_H

/* `cclaw --channel <name> --harness <scenario.json>` — offline fixture-replay
 * test harness. Runs the channel's real JS against a scratch DB, matching
 * cclaw.send() traffic against scenario fixtures instead of the network.
 * Prints a transcript of seam traffic + expectation results; returns 0 only
 * if every expectation in the scenario passed. */
int channel_harness_run(const char *db_path, const char *channel_name, const char *scenario_path);

#endif
