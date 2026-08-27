# Vendored Dependencies

Refresh procedure (any lib): download the pinned release, replace the files
listed under the entry, rebuild with `make clean && make test`, update the
version + Last checked line here. Check the upstream changelog/advisories for
security fixes since the pinned version — that's the main reason to bump.

## SQLite 3.53.1

- Source: https://www.sqlite.org/2026/sqlite-amalgamation-3530100.zip
- Files: `sqlite3.c`, `sqlite3.h`, `shell.c`
- Docs: https://www.sqlite.org/
- License: Public domain
- Last checked: 2026-08-27

`shell.c` is the upstream CLI, exposed as the `cclaw sqlite3` verb so a
deployment target never needs an `sqlite3` package — and never reads our JSONB
columns through a pre-3.45 shell that renders them as binary garbage. It is
compiled with `-Dmain=sqlite3_shell_main` to coexist with our own `main()`, and
must stay on `-std=gnu11 -D_GNU_SOURCE` (see the Makefile rule for why). Take
it from the same zip as the amalgamation on every refresh — a shell built
against a different version than it links to is not a supported combination.

## Civetweb 1.16

- Repo: https://github.com/civetweb/civetweb
- Files: `civetweb.c`, `civetweb.h`, plus `*.inl` includes
- Built with: `-DNO_SSL -DNO_CGI -DUSE_IPV6 -DNO_CACHING`
- License: MIT
- Last checked: 2026-07-17 — **CVE-2025-55763** (URI-parser heap overflow,
  1.14–1.16, remote via crafted HTTP request, RCE potential). No 1.17
  release exists; the upstream fix commit c584455 (PR #1347) is backported
  into the vendored `civetweb.c` (see the CVE-marked block in the
  directory-slash redirect path). Re-check on next refresh.

## QuickJS (2026-06-04)

- Repo: https://github.com/bellard/quickjs
- Files: `quickjs.c`, `quickjs.h`, `quickjs-atom.h`, `quickjs-opcode.h`,
  `quickjs-libc.c`, `quickjs-libc.h`, `cutils.c`, `cutils.h`, `dtoa.c`, `dtoa.h`,
  `list.h`, `libunicode.c`, `libunicode.h`, `libunicode-table.h`,
  `libregexp.c`, `libregexp.h`, `libregexp-opcode.h`
- Built with: `-std=gnu11 -D_GNU_SOURCE -DCONFIG_VERSION`
- License: MIT
- Arena-based slab allocator for small blocks (16–512 bytes), ES2025 support
- Last checked: 2026-08-13 — no CVE against the bellard tree newer than this
  snapshot; the 2026 typed-array/atomics CVEs (CVE-2026-0822/1144/1145) are
  against the quickjs-ng fork. JS also only executes in namespace-sandboxed
  children, never the daemon process.
- **Local patch (not upstream): use-after-free in `build_backtrace`.** Same
  backport pattern as the civetweb entry above — carried here rather than
  waiting for a release. `build_backtrace` receives `error_obj` as a borrowed
  alias of `rt->current_exception` (the JS_CallInternal "add the backtrace
  later" path, which OOM errors take because they are thrown with
  `add_backtrace=FALSE`). If an allocation inside it fails — in practice the
  `JS_NewString` for the stack string, the largest allocation there and one
  that grows with frame count — the nested `JS_ThrowOutOfMemory` reaches
  `JS_Throw`, which frees `rt->current_exception`, and the following
  `JS_DefinePropertyValue` writes through the freed object. Reproduced under
  ASan by failing that one allocation: SEGV in `JS_DefineProperty` ←
  `build_backtrace`, matching an organic crash seen parsing a large feed at a
  4MB heap. Fix holds a reference across the function (`eobj`), so the value
  is `JS_DupValue`d on entry and freed at the `done:` label. Drop this patch
  if a release picks up an equivalent fix; re-apply on refresh otherwise.

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

## yxml (git master, generated amalgamation)

- Repo: https://g.blicky.net/yxml.git (homepage https://dev.yorhel.nl/yxml)
- Files: `yxml/yxml.c` (generated from yxml.c.in — fetch the pre-generated
  file from the git host's plain view, don't regenerate), `yxml/yxml.h`
- License: MIT
- Scope note: SAX XML parser behind the `XML.parse()` JS global
  (`src/qjs_xml.c`) — runs only in sandboxed children (js_eval tool child,
  channel runner), never fed untrusted input in the daemon process
- Last checked: 2026-08-12

## Secret-scanner rule data (not code)

- `gitleaks.toml` — auto-generated rule set from
  https://github.com/gitleaks/gitleaks (input to `scripts/gen_secret_scan.py`
  at build time; not parsed at runtime)
- `secrets_custom.toml` — CClaw-authored supplements (see file header); rule
  ids must also appear in `CURATED_IDS` in the generator
