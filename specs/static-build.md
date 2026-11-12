# Static release builds

Two ways to build cclaw, on purpose:

| | Source build (`make`) | Release build (`scripts/build_static.sh <target>`) |
|---|---|---|
| libc | host's, dynamic | musl, static |
| libcurl + TLS | system `libcurl.so` | pinned curl + mbedTLS, linked in |
| Runs on | the machine that built it | any Linux kernel of that architecture |
| Used by | developers, `make test`, CI `test`/`compat` jobs | CI `release-build`, GitHub releases, `cclaw update` |

A release binary asks the host for nothing but kernel syscalls and a CA
bundle. There is no glibc floor and no "which libcurl does this distro ship":
websockets, TLS behaviour and the HTTP stack are the same on every host.

## What the script does

`scripts/build_static.sh <x64|arm64|i586|armv5te> [make-target...]`, on an
x86_64 host with the distro GCC for the target:

1. Builds **musl** from source and installs its `musl-gcc` wrapper (the target
   GCC plus a specs file swapping in musl's headers and libc).
2. Builds **mbedTLS** and a minimal **curl** (http, https, ws, wss, file) with
   that wrapper.
3. Runs the ordinary Makefile with `-static`. Extra arguments are make
   targets, so `build_static.sh x64 test-integration` builds and runs the
   suite as static binaries against the static `build/cclaw`.
4. Fails unless `build/cclaw` has no `PT_INTERP`/`PT_DYNAMIC`.

Versions and SHA-256 sums are pinned at the top of the script. Dependencies
are built once into `build-deps/<target>/` (gitignored). **A curl or mbedTLS
security advisory now means bumping the pin and cutting a release** — that is
the cost of not depending on the host's copy.

## Things that differ from the source build

- **CA bundle.** A bundled libcurl has no trustworthy compiled-in CA path, so
  curl is configured without one and `util_ca_bundle()` resolves it at
  runtime: `$SSL_CERT_FILE`, else libcurl's default if it exists, else the
  well-known distro paths. `cclaw --doctor` `[tls]` shows the result. Nothing
  found is a logged error and failing HTTPS requests, not a startup exit.
- **Nothing in a release binary speaks the host's libc.** Until v0.6.0 one
  thing did: `libcclaw_net.so`, built with the build machine's glibc and
  `LD_PRELOAD`ed into the host's programs inside the sandbox. v0.6.0 shipped
  it needing glibc 2.38, and `ld.so` then refused to start every sandboxed
  shell command on Debian 12 and Amazon Linux 2023. It is gone; sandboxed
  programs reach the network through the proxy variables only
  ([shell-networking.md](shell-networking.md)), which also makes the sandboxed
  shell work the same on glibc and musl hosts.
- **`net_shim` is static** (`NET_SHIM_LDFLAGS=-static`): it runs inside the
  sandbox by itself and must not need a loader the host lacks.
- **Thread stacks.** musl's default thread stack is ~128K (glibc: 8M). All
  threads are created by `thread_spawn()` with `CCLAW_THREAD_STACK` (1M).
  Measured on x64: a full TLS turn completes on 48K stacks and overflows at
  16K.
- **Logging.** musl's `syslog()` truncates at 1024 bytes; the CLI's stderr tee
  is cclaw's own write and carries the whole line. The syslog copy is subject
  to the libc's limit.
- **No libatomic.** ARMv5TE has no 64-bit compare-and-swap, and the distro's
  ARM libatomic dispatches through IFUNC, which a static musl binary does not
  resolve. The only code that asked for it was QuickJS's `Atomics`, which is
  compiled out (`CONFIG_ATOMICS` in `vendor/quickjs/quickjs.c`) — no JS
  runtime in cclaw has a second thread. Narrower atomics (curl's spinlock,
  SQLite's barriers) come from libgcc's kernel-helper routines.
- **DNS** is musl's resolver: `/etc/resolv.conf` only, no NSS (no mDNS, no
  `nsswitch.conf` plugins).

## Targets

| Target | Compiler | Baseline |
|---|---|---|
| `x64` | `gcc` | x86-64 |
| `arm64` | `aarch64-linux-gnu-gcc` | ARMv8-A |
| `i586` | `i686-linux-gnu-gcc -march=i586` | Pentium (no CMOV/SSE) |
| `armv5te` | `arm-linux-gnueabi-gcc -march=armv5te` | ARM926, soft float |

Artifact names (`cclaw-linux-<target>`) are what `default_asset()` in
`src/update.c` asks a release for. CI smoke-tests each artifact under
qemu-user with the baseline CPU pinned where qemu can model it.
