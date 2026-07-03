#ifndef CCLAW_DOCTOR_H
#define CCLAW_DOCTOR_H

/* One-shot diagnostic bundle. Prints redacted system info to stdout.
 * Runs before normal DB/config path — a broken DB is itself a finding.
 * Always returns 0 (the report is the product). */
int doctor_main(void);

#endif
