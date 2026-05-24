CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -Iinclude -Ivendor/cJSON -Ivendor/sqlite3 -Ivendor/civetweb -Ivendor/mquickjs
LDFLAGS := -lcurl -lm -lpthread -ldl

BUILD   := build
SRC     := $(wildcard src/*.c)
OBJ     := $(patsubst src/%.c,$(BUILD)/%.o,$(SRC))

VENDOR_SRC := vendor/cJSON/cJSON.c vendor/sqlite3/sqlite3.c vendor/civetweb/civetweb.c \
              vendor/mquickjs/mquickjs.c vendor/mquickjs/cutils.c vendor/mquickjs/dtoa.c \
              vendor/mquickjs/libm.c vendor/mquickjs/mquickjs_stdlib.c
VENDOR_OBJ := $(BUILD)/cJSON.o $(BUILD)/sqlite3.o $(BUILD)/civetweb.o \
              $(BUILD)/mquickjs.o $(BUILD)/mqjs_cutils.o $(BUILD)/mqjs_dtoa.o \
              $(BUILD)/mqjs_libm.o $(BUILD)/mqjs_stdlib.o

INTEG_SRC := $(wildcard test/test_integration_*.c)
TEST_SRC  := $(filter-out $(INTEG_SRC),$(wildcard test/test_*.c))
TEST_BIN  := $(patsubst test/%.c,$(BUILD)/%,$(TEST_SRC))
INTEG_BIN := $(patsubst test/%.c,$(BUILD)/%,$(INTEG_SRC))

.PHONY: all clean test test-integration

all: $(BUILD)/cclaw

$(BUILD)/cclaw: $(OBJ) $(VENDOR_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/cJSON.o: vendor/cJSON/cJSON.c | $(BUILD)
	$(CC) $(CFLAGS) -Wno-unused-parameter -c -o $@ $<

$(BUILD)/sqlite3.o: vendor/sqlite3/sqlite3.c | $(BUILD)
	$(CC) -std=c11 -O2 -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_JSON1 -c -o $@ $<

$(BUILD)/civetweb.o: vendor/civetweb/civetweb.c | $(BUILD)
	$(CC) -std=c11 -O2 -DNO_SSL -DNO_CGI -DUSE_IPV6 -DNO_CACHING -Ivendor/civetweb -Wno-unused-parameter -c -o $@ $<

MQJS_CFLAGS := -std=c11 -O2 -Ivendor/mquickjs -Wno-unused-parameter -Wno-sign-compare -Wno-unused-variable -Wno-unused-but-set-variable

$(BUILD)/mquickjs.o: vendor/mquickjs/mquickjs.c vendor/mquickjs/mquickjs_atom.h | $(BUILD)
	$(CC) $(MQJS_CFLAGS) -c -o $@ $<

$(BUILD)/mqjs_cutils.o: vendor/mquickjs/cutils.c | $(BUILD)
	$(CC) $(MQJS_CFLAGS) -c -o $@ $<

$(BUILD)/mqjs_dtoa.o: vendor/mquickjs/dtoa.c | $(BUILD)
	$(CC) $(MQJS_CFLAGS) -c -o $@ $<

$(BUILD)/mqjs_libm.o: vendor/mquickjs/libm.c | $(BUILD)
	$(CC) $(MQJS_CFLAGS) -c -o $@ $<

$(BUILD)/mqjs_stdlib.o: vendor/mquickjs/mquickjs_stdlib.c | $(BUILD)
	$(CC) $(MQJS_CFLAGS) -c -o $@ $<

vendor/mquickjs/mquickjs_atom.h: vendor/mquickjs/gen_atoms.c | $(BUILD)
	$(CC) -o $(BUILD)/gen_atoms $<
	./$(BUILD)/gen_atoms > $@

vendor/mquickjs/mquickjs_stdlib.c: vendor/mquickjs/gen_stdlib.c vendor/mquickjs/mquickjs_build.c vendor/mquickjs/cutils.c vendor/mquickjs/mqjs_host.c | $(BUILD)
	$(CC) -Ivendor/mquickjs -o $(BUILD)/gen_stdlib vendor/mquickjs/gen_stdlib.c vendor/mquickjs/mquickjs_build.c vendor/mquickjs/cutils.c -lm
	printf '#define _POSIX_C_SOURCE 199309L\n#include <stdlib.h>\n#include <string.h>\n#include <stdio.h>\n#include <math.h>\n#include <time.h>\n#include "mquickjs_priv.h"\n\n' > $@
	cat vendor/mquickjs/mqjs_host.c >> $@
	./$(BUILD)/gen_stdlib -m64 | sed '1,/^#include "mquickjs_priv.h"/d' >> $@

$(BUILD):
	mkdir -p $(BUILD)

test: $(TEST_BIN)
	@for t in $(TEST_BIN); do echo "--- $$t ---"; ./$$t || exit 1; done

test-integration: $(INTEG_BIN)
	@for t in $(INTEG_BIN); do echo "--- $$t ---"; ./$$t || exit 1; done

test-all: test test-integration

LIB_OBJ := $(filter-out $(BUILD)/main.o,$(OBJ))
$(BUILD)/libcclaw.a: $(LIB_OBJ) $(VENDOR_OBJ) | $(BUILD)
	$(AR) rcs $@ $^

$(BUILD)/test_%: test/test_%.c $(BUILD)/libcclaw.a | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(BUILD)/libcclaw.a $(LDFLAGS)

clean:
	rm -rf $(BUILD)
