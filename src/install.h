#ifndef CCLAW_INSTALL_H
#define CCLAW_INSTALL_H

/* `cclaw install [--system]` — onboarding's missing middle step (configure →
 * install → long-lived daemon). Replaces `make install`. Runs before normal
 * DB/config path, same as --doctor. Default: user systemd unit + ~/.cclaw/env.
 * --system: root install (/usr/local/bin, /etc/cclaw, /etc/systemd/system). */
int install_main(int argc, char *argv[]);

/* `cclaw uninstall [--system]` — stop+disable the unit and remove the unit
 * file. Leaves ~/.cclaw (or /etc/cclaw + the installed binary) untouched. */
int uninstall_main(int argc, char *argv[]);

#endif
