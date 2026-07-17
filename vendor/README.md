# Vendored Dependencies

Refresh procedure (any lib): download the pinned release, replace the files
listed under the entry, rebuild with `make clean && make test`, update the
version + Last checked line here. Check the upstream changelog/advisories for
security fixes since the pinned version — that's the main reason to bump.

## SQLite 3.53.1

- Source: https://www.sqlite.org/2026/sqlite-amalgamation-3530100.zip
- Docs: https://www.sqlite.org/
- License: Public domain
- Last checked: 2026-07-17

## Civetweb 1.16

- Repo: https://github.com/civetweb/civetweb
- Files: `civetweb.c`, `civetweb.h`, plus `*.inl` includes
- Built with: `-DNO_SSL -DNO_CGI -DUSE_IPV6 -DNO_CACHING`
- License: MIT
- Last checked: 2026-07-17 — **CVE-2025-55763** (URI-parser heap overflow,
  1.14–1.16, remote via crafted HTTP request, RCE potential; fixed in 1.17).
  Affects the vendored version: upgrade to 1.17 is queued.

## QuickJS (2026-06-04)

- Repo: https://github.com/bellard/quickjs
- Files: `quickjs.c`, `quickjs.h`, `quickjs-atom.h`, `quickjs-opcode.h`,
  `quickjs-libc.c`, `quickjs-libc.h`, `cutils.c`, `cutils.h`, `dtoa.c`, `dtoa.h`,
  `list.h`, `libunicode.c`, `libunicode.h`, `libunicode-table.h`,
  `libregexp.c`, `libregexp.h`, `libregexp-opcode.h`
- Built with: `-std=gnu11 -D_GNU_SOURCE -DCONFIG_VERSION`
- License: MIT
- Arena-based slab allocator for small blocks (16–512 bytes), ES2025 support
- Last checked: 2026-07-17 — no CVE against the bellard tree newer than this
  snapshot; the 2026 typed-array/atomics CVEs (CVE-2026-0822/1144/1145) are
  against the quickjs-ng fork. JS also only executes in namespace-sandboxed
  children, never the daemon process.

## Monocypher 4.0.2

- Source: https://monocypher.org
- Files: `monocypher.c`, `monocypher.h`
- License: BSD-2-Clause OR CC0-1.0
- Last checked: 2026-07-17

## jsmn (master snapshot, header-only — upstream frozen since ~2019)

- Repo: https://github.com/zserge/jsmn
- Files: `jsmn/jsmn.h`
- License: MIT
- Scope note: db-less corners only (channel-harness scenario reader) — do not
  reintroduce it on any path that has a db handle (AGENTS.md)
- Last checked: 2026-07-17

## Secret-scanner rule data (not code)

- `gitleaks.toml` — auto-generated rule set from
  https://github.com/gitleaks/gitleaks (input to `scripts/gen_secret_scan.py`
  at build time; not parsed at runtime)
- `secrets_custom.toml` — CClaw-authored supplements (see file header); rule
  ids must also appear in `CURATED_IDS` in the generator
