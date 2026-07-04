CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -Isrc -Ivendor/sqlite3 -Ivendor/civetweb -Ivendor/quickjs -Ivendor/monocypher -Ivendor/jsmn
LDFLAGS := -lcurl -lm -lpthread -ldl

VERSION_COMMIT := $(shell git -C $(dir $(firstword $(MAKEFILE_LIST))) rev-parse --short HEAD 2>/dev/null || echo unknown)
BUILD_DATE     := $(shell date -u +%Y-%m-%d)
CFLAGS += -DVERSION_COMMIT='"$(VERSION_COMMIT)"' -DBUILD_DATE='"$(BUILD_DATE)"'
# debug passes its extra flags here — re-passing CFLAGS="$(CFLAGS)" through the
# recursive $(MAKE) would strip the nested quotes off the -D defines above.
CFLAGS += $(EXTRA_CFLAGS)

BUILDDIR := build

# Build-mode guard: build/ holds objects compiled with one CC/EXTRA_CFLAGS
# combo (e.g. plain `cc` vs. clang+sanitizers from `debug`/`test-asan`).
# Linking objects from two different combos fails with cryptic undefined
# __asan_*/__ubsan_* references. `debug` and `test-asan` clean first, but any
# other invocation (a bare `make test`, or `make build/test_foo` for a single
# binary) does not — so if the mode changed since the last build, wipe build/
# before anything compiles. Runs at parse time (not as a recipe prerequisite)
# so it can't lose an ordering race with rules that also touch build/.
BUILD_TAG := $(CC)|$(EXTRA_CFLAGS)
BUILD_TAGFILE := $(BUILDDIR)/.buildtag
ifneq ($(wildcard $(BUILD_TAGFILE)),)
ifneq ($(shell cat $(BUILD_TAGFILE)),$(BUILD_TAG))
$(info build mode changed — cleaning $(BUILDDIR)/)
$(shell rm -rf $(BUILDDIR))
endif
endif
$(shell mkdir -p $(BUILDDIR) && echo "$(BUILD_TAG)" > $(BUILD_TAGFILE))

TEMPLATES := $(wildcard templates/*)
SRC      := $(filter-out src/preload_net.c src/net_shim.c,$(wildcard src/*.c))
OBJ      := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(SRC))
DEP      := $(OBJ:.o=.d)

VENDOR_SRC := vendor/sqlite3/sqlite3.c vendor/civetweb/civetweb.c \
              vendor/quickjs/quickjs.c vendor/quickjs/cutils.c \
              vendor/quickjs/dtoa.c vendor/quickjs/libunicode.c \
              vendor/quickjs/libregexp.c \
              vendor/monocypher/monocypher.c
VENDOR_OBJ := $(BUILDDIR)/sqlite3.o $(BUILDDIR)/civetweb.o \
              $(BUILDDIR)/quickjs.o $(BUILDDIR)/qjs_cutils.o \
              $(BUILDDIR)/qjs_dtoa.o $(BUILDDIR)/qjs_libunicode.o \
              $(BUILDDIR)/qjs_libregexp.o \
              $(BUILDDIR)/monocypher.o \
              $(BUILDDIR)/templates.o

INTEG_SRC := $(wildcard test/test_integration_*.c)
E2E_SRC   := $(wildcard test/test_e2e_*.c)
TEST_SRC  := $(filter-out $(INTEG_SRC) $(E2E_SRC),$(wildcard test/test_*.c))
TEST_BIN  := $(patsubst test/%.c,$(BUILDDIR)/%,$(TEST_SRC))
INTEG_BIN := $(patsubst test/%.c,$(BUILDDIR)/%,$(INTEG_SRC))
E2E_BIN   := $(patsubst test/%.c,$(BUILDDIR)/%,$(E2E_SRC))

.PHONY: all clean test smoke test-integration test-e2e test-all check-gen install debug test-asan

# Curated fast unit subset — no network, no fork. Target: a few seconds.
SMOKE := test_db test_config test_advance_session test_llm_payload test_tools \
         test_session_state test_tool_file test_context_plan test_secret_scan \
         test_agent_setup test_processes test_recovery_scoping test_sandbox_profile \
         test_sensitive test_secret_bind \
         test_approval_block_window test_approval_postwindow test_tool_check_approval
SMOKE_BIN := $(patsubst %,$(BUILDDIR)/%,$(SMOKE))

all: $(BUILDDIR)/cclaw $(BUILDDIR)/libcclaw.a $(BUILDDIR)/libcclaw_net.so $(BUILDDIR)/net_shim compile_commands.json

# Development build: debug symbols, no optimization, sanitizers (clang preferred for better traces)
debug: clean
	$(MAKE) all CC=clang \
	            EXTRA_CFLAGS="-O0 -g3 -fno-omit-frame-pointer -fsanitize=address,undefined" \
	            LDFLAGS="$(LDFLAGS) -fsanitize=address,undefined"

# Full unit suite under ASan/UBSan — library, main binary, and every test
# binary all instrumented (a plain `make test` after `make debug` link-fails
# on mixed objects; cleaning first makes that impossible). The standard
# pre-commit sanitizer check.
test-asan: clean
	$(MAKE) test CC=clang \
	            EXTRA_CFLAGS="-O0 -g3 -fno-omit-frame-pointer -fsanitize=address,undefined" \
	            LDFLAGS="$(LDFLAGS) -fsanitize=address,undefined"

$(BUILDDIR)/cclaw: $(OBJ) $(VENDOR_OBJ) | $(BUILDDIR)/
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILDDIR)/templates.h $(BUILDDIR)/templates.c: $(TEMPLATES) scripts/gen_templates.sh | $(BUILDDIR)/
	./scripts/gen_templates.sh $(BUILDDIR)/templates.h templates

$(BUILDDIR)/templates.o: $(BUILDDIR)/templates.c $(BUILDDIR)/templates.h | $(BUILDDIR)/
	$(CC) -std=c11 -O2 -I$(BUILDDIR) -c -o $@ $<

$(BUILDDIR)/%.o: src/%.c $(BUILDDIR)/templates.h | $(BUILDDIR)/
	$(CC) $(CFLAGS) -I$(BUILDDIR) -MMD -MP -c -o $@ $<

$(BUILDDIR)/sandbox.o: $(BUILDDIR)/preload_blob.h $(BUILDDIR)/net_shim_blob.h

$(BUILDDIR)/sqlite3.o: vendor/sqlite3/sqlite3.c | $(BUILDDIR)/
	$(CC) -std=c11 -O2 -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_JSON1 -c -o $@ $<

$(BUILDDIR)/civetweb.o: vendor/civetweb/civetweb.c | $(BUILDDIR)/
	$(CC) -std=c11 -O2 -DNO_SSL -DNO_CGI -DUSE_IPV6 -DNO_CACHING -Ivendor/civetweb -Wno-unused-parameter -c -o $@ $<

$(BUILDDIR)/monocypher.o: vendor/monocypher/monocypher.c | $(BUILDDIR)/
	$(CC) -std=c11 -O2 -c -o $@ $<

# QuickJS compilation flags — suppress vendor warnings
QJS_CFLAGS := -std=gnu11 -O2 -Ivendor/quickjs -DCONFIG_VERSION=\"2026-06-04\" \
              -D_GNU_SOURCE \
              -Wno-unused-parameter -Wno-sign-compare -Wno-unused-variable \
              -Wno-unused-but-set-variable -Wno-implicit-fallthrough

$(BUILDDIR)/quickjs.o: vendor/quickjs/quickjs.c | $(BUILDDIR)/
	$(CC) $(QJS_CFLAGS) -c -o $@ $<

$(BUILDDIR)/qjs_cutils.o: vendor/quickjs/cutils.c | $(BUILDDIR)/
	$(CC) $(QJS_CFLAGS) -c -o $@ $<

$(BUILDDIR)/qjs_libunicode.o: vendor/quickjs/libunicode.c | $(BUILDDIR)/
	$(CC) $(QJS_CFLAGS) -c -o $@ $<

$(BUILDDIR)/qjs_libregexp.o: vendor/quickjs/libregexp.c | $(BUILDDIR)/
	$(CC) $(QJS_CFLAGS) -c -o $@ $<

$(BUILDDIR)/qjs_dtoa.o: vendor/quickjs/dtoa.c | $(BUILDDIR)/
	$(CC) $(QJS_CFLAGS) -c -o $@ $<

$(BUILDDIR)/libcclaw_net.so: src/preload_net.c | $(BUILDDIR)/
	$(CC) -std=c11 -Wall -Wextra -Werror -shared -fPIC -o $@ $< -ldl

$(BUILDDIR)/preload_blob.h: $(BUILDDIR)/libcclaw_net.so
	printf 'static const unsigned char preload_net_blob[] = {\n' > $@
	xxd -i < $< >> $@
	printf '};\nstatic const unsigned int preload_net_blob_len = sizeof(preload_net_blob);\n' >> $@

# net_shim: loopback HTTP CONNECT proxy for static binaries (link-isolated —
# its own TU, libc only, no policy/resolver/secret symbols). Embedded as a blob
# and written into the sandbox /tmp at runtime, like the preload .so.
$(BUILDDIR)/net_shim: src/net_shim.c | $(BUILDDIR)/
	$(CC) -std=c11 -Wall -Wextra -Werror -o $@ $<

$(BUILDDIR)/net_shim_blob.h: $(BUILDDIR)/net_shim
	printf 'static const unsigned char net_shim_blob[] = {\n' > $@
	xxd -i < $< >> $@
	printf '};\nstatic const unsigned int net_shim_blob_len = sizeof(net_shim_blob);\n' >> $@

# `make install` is retired — `cclaw install` (src/install.c) replaces it:
# a user systemd unit by default, `--system` for the old root-install flow.

$(BUILDDIR)/:
	mkdir -p $(BUILDDIR)

# Each test binary's output goes to a /tmp file, never to make's stdout.
# This keeps `make test | tail/grep/head` safe: leaked mock-server children
# hold the file fd, not the pipe, so readers always see EOF; and only make's
# own one-liners can take a SIGPIPE.
# test_tool_js / test_js_http_fetch fork build/cclaw via CCLAW_QJS_EXE, so the
# binary must be fresh — depend on it so a stale build/cclaw can't slip in.
test: $(TEST_BIN) $(BUILDDIR)/cclaw
	@fail=0; for t in $(TEST_BIN); do \
		out="/tmp/cclaw_$$(basename $$t).txt"; \
		timeout 20 ./$$t > $$out 2>&1; rc=$$?; \
		if [ $$rc -eq 0 ]; then echo "PASS $$t (→ $$out)"; \
		elif [ $$rc -eq 124 ]; then echo "TIMEOUT $$t (killed after 20s → $$out)"; fail=1; \
		else echo "FAIL $$t (exit $$rc → $$out)"; tail -20 $$out | sed 's/^/  | /'; fail=1; fi; \
	done; \
	if [ $$fail -eq 0 ]; then echo "All unit tests passed ($(words $(TEST_BIN)) suites)"; fi; \
	exit $$fail

# Fast smoke subset — same per-binary capture/report loop as `test`.
smoke: $(SMOKE_BIN)
	@fail=0; for t in $(SMOKE_BIN); do \
		out="/tmp/cclaw_$$(basename $$t).txt"; \
		timeout 20 ./$$t > $$out 2>&1; rc=$$?; \
		if [ $$rc -eq 0 ]; then echo "PASS $$t (→ $$out)"; \
		elif [ $$rc -eq 124 ]; then echo "TIMEOUT $$t (killed after 20s → $$out)"; fail=1; \
		else echo "FAIL $$t (exit $$rc → $$out)"; tail -20 $$out | sed 's/^/  | /'; fail=1; fi; \
	done; \
	if [ $$fail -eq 0 ]; then echo "Smoke passed ($(words $(SMOKE_BIN)) suites)"; fi; \
	exit $$fail

test-integration: $(INTEG_BIN) $(BUILDDIR)/cclaw
	@fail=0; for t in $(INTEG_BIN); do \
		out="/tmp/cclaw_$$(basename $$t).txt"; \
		timeout 45 ./$$t > $$out 2>&1; rc=$$?; \
		lines=$$(wc -l < $$out); \
		if [ $$rc -eq 0 ]; then echo "PASS $$t ($$lines lines → $$out)"; \
		elif [ $$rc -eq 124 ]; then echo "TIMEOUT $$t (killed after 45s, $$lines lines → $$out)"; fail=1; \
		else echo "FAIL $$t (exit $$rc, $$lines lines → $$out)"; tail -20 $$out | sed 's/^/  | /'; fail=1; fi; \
	done; exit $$fail

test-e2e: $(E2E_BIN)
	@fail=0; for t in $(E2E_BIN); do \
		out="/tmp/cclaw_$$(basename $$t).txt"; \
		timeout 120 ./$$t > $$out 2>&1; rc=$$?; \
		if [ $$rc -eq 0 ]; then echo "PASS $$t (→ $$out)"; \
		elif [ $$rc -eq 124 ]; then echo "TIMEOUT $$t (killed after 120s → $$out)"; fail=1; \
		else echo "FAIL $$t (exit $$rc → $$out)"; tail -20 $$out | sed 's/^/  | /'; fail=1; fi; \
	done; exit $$fail

# Verify the checked-in secret-scan headers match the generator output.
# Dev-only (needs python3) — skipped silently when python3 is absent, so
# embedded builds never grow a python dependency.
check-gen:
	@command -v python3 >/dev/null 2>&1 || { echo "check-gen: skipped (no python3)"; exit 0; }; \
	cp src/secret_scan_ac.h /tmp/cclaw_checkgen_ac.h; \
	cp src/secret_scan_rules.h /tmp/cclaw_checkgen_rules.h; \
	python3 scripts/gen_secret_scan.py >/dev/null 2>&1; \
	if cmp -s src/secret_scan_ac.h /tmp/cclaw_checkgen_ac.h && \
	   cmp -s src/secret_scan_rules.h /tmp/cclaw_checkgen_rules.h; then \
		echo "check-gen: secret-scan headers in sync"; \
	else \
		cp /tmp/cclaw_checkgen_ac.h src/secret_scan_ac.h; \
		cp /tmp/cclaw_checkgen_rules.h src/secret_scan_rules.h; \
		echo "check-gen: STALE — rerun 'python3 scripts/gen_secret_scan.py' and commit"; \
		exit 1; \
	fi

test-all: test test-integration check-gen

LIB_OBJ := $(filter-out $(BUILDDIR)/main.o,$(OBJ))
$(BUILDDIR)/libcclaw.a: $(LIB_OBJ) $(VENDOR_OBJ) | $(BUILDDIR)/
	rm -f $@
	$(AR) rcs $@ $^

$(BUILDDIR)/mock_server.o: test/mock_server.c | $(BUILDDIR)/
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/libtest.a: $(BUILDDIR)/mock_server.o
	rm -f $@
	$(AR) rcs $@ $^

$(BUILDDIR)/test_%: test/test_%.c $(BUILDDIR)/libcclaw.a $(BUILDDIR)/libtest.a | $(BUILDDIR)/
	$(CC) $(CFLAGS) -I$(BUILDDIR) -o $@ $< $(BUILDDIR)/libtest.a $(BUILDDIR)/libcclaw.a $(LDFLAGS)

compile_commands.json: $(BUILDDIR)/templates.h
	@echo "[" > $@
	@# CFLAGS has -D...='"x"' — quoting meant for clangd to re-tokenize later,
	@# same as a real shell would. So the stored JSON must keep those quotes,
	@# only escaped for JSON (" -> \"), NOT resolved now. $(subst) works on
	@# raw text (no shell involved), so it can't have the collision below.
	@# It emits \\\" (4 chars) per quote because the printf line still wraps
	@# this in "..." — the shell's own \" unescape then collapses it to the
	@# \" (2 chars) that's actually valid JSON.
	@first=1; for f in $(SRC); do \
		[ $$first -eq 0 ] && printf ",\n" >> $@; first=0; \
		printf '  {"directory":"%s","file":"%s","command":"%s %s -I%s -c %s"}' \
			"$(CURDIR)" "$$f" "$(CC)" "$(subst ",\\\",$(CFLAGS))" "$(BUILDDIR)" "$$f" >> $@; \
	done
	@printf "\n]\n" >> $@
	@echo "Generated $@ ($(words $(SRC)) entries)"

clean:
	rm -rf $(BUILDDIR)

-include $(DEP)
