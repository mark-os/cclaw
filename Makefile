CC      ?= cc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -Iinclude -Ivendor/cJSON -Ivendor/sqlite3 -Ivendor/civetweb -Ivendor/mquickjs
LDFLAGS := -lcurl -lm -lpthread -ldl

BUILD   := build
SRC     := $(wildcard src/*.c)
OBJ     := $(patsubst src/%.c,$(BUILD)/%.o,$(SRC))

VENDOR_SRC := vendor/cJSON/cJSON.c vendor/sqlite3/sqlite3.c
VENDOR_OBJ := $(BUILD)/cJSON.o $(BUILD)/sqlite3.o

TEST_SRC := $(wildcard test/test_*.c)
TEST_BIN := $(patsubst test/%.c,$(BUILD)/%,$(TEST_SRC))

.PHONY: all clean test

all: $(BUILD)/cclaw

$(BUILD)/cclaw: $(OBJ) $(VENDOR_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/cJSON.o: vendor/cJSON/cJSON.c | $(BUILD)
	$(CC) $(CFLAGS) -Wno-unused-parameter -c -o $@ $<

$(BUILD)/sqlite3.o: vendor/sqlite3/sqlite3.c | $(BUILD)
	$(CC) -std=c11 -O2 -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_JSON1 -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

test: $(TEST_BIN)
	@for t in $(TEST_BIN); do echo "--- $$t ---"; ./$$t || exit 1; done

$(BUILD)/test_%: test/test_%.c $(VENDOR_OBJ) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(VENDOR_OBJ) $(LDFLAGS)

clean:
	rm -rf $(BUILD)
