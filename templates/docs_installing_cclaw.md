---
name: installing-cclaw
description: How to install CClaw as a long-running service — `cclaw install` on this machine, putting the right release binary on another machine, moving this instance to a new home, or starting a second separate one. Read before helping anyone with install, "keep running after I close the terminal", a new device, or reaching the dashboard.
---

# Installing CClaw

## Your part in this

What you can run yourself depends on where the command has to happen.

- **On this machine: `install_service`.** Your shell, if you have one, is a
  sandbox: it cannot reach the service manager and its `$HOME` is your
  workspace, so `cclaw install` typed there would install nothing real.
  Instead call `install_service`: the person approves, cclaw itself runs
  `cclaw install` for their user, and you get its output back. It works from
  a terminal session on a machine with systemd; anywhere else the person runs
  the command (below).
- **On another machine: you can, over ssh, with approvals.** Ask with
  `request_config` for `shell_exec` and a host grant for the target's address
  (a home address like `192.168.1.20` is a private address and needs its own
  grant). ssh from the sandbox needs
  `-o ProxyCommand="$CCLAW_CONNECT %h %p"`. The person sets up the login — a
  key pair you generate in your workspace whose public half they add on the
  target is the clean way. Then you can download the binary there and run
  `cclaw install` there yourself.
- **Moving this instance: the person copies the database and key.** You cannot
  read either, so you cannot send them anywhere.

Whoever runs a step:

- Give one step at a time and ask for the output before the next.
- Never ask for a key, token or password to be pasted to you. Keys go into a
  terminal prompt or a file the person edits.
- Finish with proof, not a claim: `cclaw --doctor` reads clean, and a message
  sent through a channel arrives.

**Know where it is being installed.** Before any install, be clear which
machine the daemon will live on and where the person will sit when they open
its admin page. On the machine they are using, nothing more is needed. On
another machine on their home or work network — a Pi, a router, a server in a
closet — the admin page is unreachable by default, because it answers only on
the machine itself. So an install on another machine is not finished until
admin access is settled: either the page is opened to that network, or it
stays closed and one particular machine reaches it through an ssh forward.
Raise this yourself as part of the install; do not wait to be asked. The
choices and what each exposes are under "Reaching the dashboard".

First find out which of four things they want:

| They say | Section |
|---|---|
| "keep running when I close this terminal" | Install on this machine |
| "put it on my router / Pi / other box" and keep everything | Move this instance |
| "run another one over there" | A second, separate instance |
| "how do I open the dashboard" | Reaching the dashboard |

## Install on this machine

From a terminal session, call `install_service` and let the person approve — it runs the command below for them. What follows is what that command does, and what the person types when the tool is refused (no systemd, or you are already a daemon).

`cclaw install` sets up a service for the current user. It:

- writes `~/.config/systemd/user/cclaw.service`, which starts **the binary at
  the path it is run from** — so move the binary somewhere permanent first
  (`~/.local/bin/cclaw` is a good home; a Downloads folder is not);
- writes `~/.cclaw/env` if there is none (every line is a comment until edited);
- enables and starts the service, and turns on linger so it survives logout.

```
mkdir -p ~/.local/bin && mv ./cclaw ~/.local/bin/cclaw
~/.local/bin/cclaw install
systemctl --user status cclaw
```

The API key: a key typed at cclaw's first-run prompt, or stored with
`cclaw secret set OPENROUTER_API_KEY`, is in the database and the service
finds it — nothing to edit. A key in `~/.cclaw/env` also works and wins over
the stored one, so a wrong line there hides a good stored key.

It needs systemd with user services. Where there is none (a router, an old
init system, a container) use the system install below, or run
`cclaw --daemon` under whatever supervisor the machine has. The supervisor
must restart cclaw when it exits non-zero: `cclaw update` relies on that.

`cclaw install --system` (as root) is the appliance form. It creates a `cclaw`
user, copies the binary to `/usr/local/bin/cclaw`, writes `/etc/cclaw/env`,
and installs a systemd unit or, without systemd, an `/etc/init.d/cclaw`
script. The daemon runs as the `cclaw` user, never as root. **Its home is
`/home/cclaw/.cclaw` — a different database from the one you are talking in.**
After a `--system` install the person either starts fresh there or moves this
instance into it (see Move).

`cclaw uninstall [--system]` stops and removes the service. It leaves the
data alone.

## The right binary for another machine

Releases are single static files; nothing else needs installing but a CA
certificate bundle. Ask for the output of `uname -m` on the target:

| `uname -m` | File |
|---|---|
| `x86_64` | `cclaw-linux-x64` |
| `aarch64`, `arm64` | `cclaw-linux-arm64` |
| `i586`, `i686` | `cclaw-linux-i586` |
| `armv5tel`, `armv6l`, `armv7l` | `cclaw-linux-armv5te` |

Anything else (MIPS, RISC-V) has no release binary and needs a source build.

```
curl -L -o cclaw https://github.com/mark-os/cclaw/releases/latest/download/<file>
chmod +x cclaw
./cclaw --version
./cclaw --doctor
```

`./cclaw --version` printing a version is the proof the file matches the
machine; "Exec format error" means the wrong file. Each release also carries
`checksums.b2` for `b2sum -c`. On a box with no `curl`, download on another
machine and copy the file over (`scp`, a USB stick).

`--doctor` says what the host supports. The line that matters most is the
sandbox: where it cannot be set up, tool calls are refused until the host is
fixed or the operator chooses the `host` profile knowingly.

## Move this instance to another machine

Same agent, same memory, same channels, new home. Everything that is "you"
is four things under `~/.cclaw/` (or wherever `CCLAW_DB_PATH` points):

| What | Why |
|---|---|
| `cclaw.db` | all state |
| `.cclaw_key` | decrypts the stored secrets — without it they are gone |
| `agents/` | workspaces, personal skills |
| `extensions/` | promoted extension files |

plus the `env` file if it holds anything.

One instance runs in one place: two daemons holding the same channel token
fight over every message. So the old one stops before the new one starts.

1. New machine: put the binary there (above). Use the same release as the old
   machine or a newer one — never older.
2. Old machine: stop the service (`systemctl --user stop cclaw`).
3. Old machine: `cclaw backup /tmp/cclaw-move.db` — a clean single-file copy.
4. Copy to the new machine, into `~/.cclaw/`: the snapshot **as `cclaw.db`**,
   `.cclaw_key` (keep it mode 600), `agents/`, `extensions/`, `env`.
5. New machine: `cclaw --doctor`, then `cclaw install`. If the old home was
   the machine they sit at and the new one is not, settle admin access now
   (Reaching the dashboard) — the setting travelled with the database and
   still says "this machine only".
6. Proof: send a message through the channel and see the reply come from the
   new machine (`cclaw --doctor` there shows a running daemon).
7. Old machine: `cclaw uninstall`. Keep its `~/.cclaw/` until the new home has
   run for a few days, then delete it — it holds a copy of the key.

Paths that named the old machine's home directory (in your notes, cron
prompts or scripts) are now wrong if the user name differs. Check and fix them
on your first turn in the new home.

## A second, separate instance

A fresh install on the other machine: its own database, its own key, its own
first conversation. Copy nothing. It starts with its admin page closed to the
network; settle that as part of the install (Reaching the dashboard). It needs its own channel bot (a second
Telegram or Discord bot) — sharing one token is the fight described above. An
agent definition cannot be exported from here to there yet; describe the agent
to the new instance and let it build its own.

## Reaching the dashboard

The daemon's web pages listen on `127.0.0.1:8080` by default: this machine
only. `cclaw dashboard` prints the sign-in link.

- A machine with a screen: open the link in its own browser. Nothing to change.
- Another machine on the home or work network. Two ways, and the person
  chooses:
  - **Allow admin from within the network.** Set `web_bind` to the box's own
    address on that network (`request_config`, config key `web_bind`; the
    operator approves) and restart the service. Any device on the network can
    then reach the page; the sign-in link is still required. Say plainly what
    that means before asking: the pages are plain HTTP, the link is the whole
    credential, and anyone on that network who sees it has the dashboard.
    Prefer the box's own address over `0.0.0.0`. Right for a home network the
    person trusts; think twice on a shared or work network.
  - **Allow admin from one machine only.** Leave `web_bind` alone and forward
    the port from the machine they administer from:
    `ssh -L 8080:127.0.0.1:8080 user@box`, then open the link on
    `localhost:8080` there. Nothing new listens on the network. There is no
    setting that admits one address and refuses the rest — the forward is how
    "only this machine" is done. Right for a work network, or when they
    already ssh into the box.
- From outside the home there is no built-in path yet. A channel is the way
  to reach you from anywhere; it needs no open port.
