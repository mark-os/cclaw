CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Werror -g
VENDOR_CFLAGS = -std=c11 -Wall -Wextra -g
LDFLAGS = -lcurl -lpthread -ldl -lm

BUILD = build
SRC = $(wildcard src/*.c)
SRC_OBJS = $(patsubst src/%.c,$(BUILD)/%.o,$(SRC))
VENDOR_OBJS = $(BUILD)/cJSON.o $(BUILD)/sqlite3.o

.PHONY: all test clean

all: $(BUILD)/cclaw

$(BUILD):
	mkdir -p $(BUILD)

# Vendor objects (no -Werror, upstream code)
$(BUILD)/cJSON.o: vendor/cJSON/cJSON.c | $(BUILD)
	$(CC) $(VENDOR_CFLAGS) -c $< -o $@

$(BUILD)/sqlite3.o: vendor/sqlite3/sqlite3.c | $(BUILD)
	$(CC) $(VENDOR_CFLAGS) -c $< -o $@

# Source objects
$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -Ivendor/cJSON -Ivendor/sqlite3 -c $< -o $@

# Main binary
$(BUILD)/cclaw: $(SRC_OBJS) $(VENDOR_OBJS)
	$(CC) $(CFLAGS) $^ $(LDFLAGS) -o $@

# Tests
$(BUILD)/test_vendor: test/test_vendor.c $(VENDOR_OBJS) | $(BUILD)
	$(CC) $(CFLAGS) -Ivendor/cJSON -Ivendor/sqlite3 $< $(VENDOR_OBJS) $(LDFLAGS) -o $@

$(BUILD)/test_arena: test/test_arena.c src/arena.c | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude $^ -o $@

$(BUILD)/test_types: test/test_types.c src/types.c src/arena.c | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude $^ -o $@

$(BUILD)/test_http: test/test_http.c src/http.c | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude $< src/http.c $(LDFLAGS) -o $@

$(BUILD)/test_llm_request: test/test_llm_request.c src/llm.c src/http.c src/types.c src/arena.c $(BUILD)/cJSON.o | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -Ivendor/cJSON $< src/llm.c src/http.c src/types.c src/arena.c $(BUILD)/cJSON.o $(LDFLAGS) -o $@

$(BUILD)/test_llm_response: test/test_llm_response.c src/llm.c src/http.c src/types.c src/arena.c $(BUILD)/cJSON.o | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -Ivendor/cJSON $< src/llm.c src/http.c src/types.c src/arena.c $(BUILD)/cJSON.o $(LDFLAGS) -o $@

$(BUILD)/test_llm_e2e: test/test_llm_e2e.c src/llm.c src/http.c src/types.c src/arena.c $(BUILD)/cJSON.o | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -Ivendor/cJSON $< src/llm.c src/http.c src/types.c src/arena.c $(BUILD)/cJSON.o $(LDFLAGS) -o $@

$(BUILD)/test_cli: test/test_cli.c src/cli.c | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude $^ -o $@

$(BUILD)/test_agent_turn: test/test_agent_turn.c src/agent.c src/types.c src/arena.c | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude $^ -o $@

$(BUILD)/test_agent_tools: test/test_agent_tools.c src/agent.c src/types.c src/arena.c | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude $^ -o $@

$(BUILD)/test_tool_shell: test/test_tool_shell.c src/tool_shell.c src/arena.c $(BUILD)/cJSON.o | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -Ivendor/cJSON $< src/tool_shell.c src/arena.c $(BUILD)/cJSON.o -o $@

$(BUILD)/test_tool_file: test/test_tool_file.c src/tool_file.c src/arena.c $(BUILD)/cJSON.o | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -Ivendor/cJSON $< src/tool_file.c src/arena.c $(BUILD)/cJSON.o -o $@

$(BUILD)/test_config: test/test_config.c src/config.c src/arena.c $(BUILD)/cJSON.o | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -Ivendor/cJSON $< src/config.c src/arena.c $(BUILD)/cJSON.o -o $@

$(BUILD)/test_db: test/test_db.c src/db.c src/types.c src/arena.c $(BUILD)/cJSON.o $(BUILD)/sqlite3.o | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -Ivendor/cJSON -Ivendor/sqlite3 $< src/db.c src/types.c src/arena.c $(BUILD)/cJSON.o $(BUILD)/sqlite3.o $(LDFLAGS) -o $@

$(BUILD)/test_telegram: test/test_telegram.c src/telegram.c src/http.c src/arena.c $(BUILD)/cJSON.o | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -Ivendor/cJSON $< src/telegram.c src/http.c src/arena.c $(BUILD)/cJSON.o $(LDFLAGS) -o $@

$(BUILD)/test_main: test/test_main.c $(BUILD)/cclaw | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude $< -o $@

$(BUILD)/test_dispatch: test/test_dispatch.c src/dispatch.c | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude $< src/dispatch.c $(LDFLAGS) -o $@

$(BUILD)/test_repl_tool: test/test_repl_tool.c src/agent.c src/cli.c src/tool_shell.c src/tool_file.c src/types.c src/arena.c $(BUILD)/cJSON.o | $(BUILD)
	$(CC) $(CFLAGS) -Iinclude -Ivendor/cJSON $< src/agent.c src/cli.c src/tool_shell.c src/tool_file.c src/types.c src/arena.c $(BUILD)/cJSON.o -o $@

test: $(BUILD)/test_vendor $(BUILD)/test_arena $(BUILD)/test_types $(BUILD)/test_http $(BUILD)/test_llm_request $(BUILD)/test_llm_response $(BUILD)/test_llm_e2e $(BUILD)/test_cli $(BUILD)/test_agent_turn $(BUILD)/test_agent_tools $(BUILD)/test_tool_shell $(BUILD)/test_tool_file $(BUILD)/test_config $(BUILD)/test_db $(BUILD)/test_telegram $(BUILD)/test_dispatch $(BUILD)/test_repl_tool $(BUILD)/test_main
	./$(BUILD)/test_vendor
	./$(BUILD)/test_arena
	./$(BUILD)/test_types
	./$(BUILD)/test_http
	./$(BUILD)/test_llm_request
	./$(BUILD)/test_llm_response
	./$(BUILD)/test_llm_e2e
	./$(BUILD)/test_cli
	./$(BUILD)/test_agent_turn
	./$(BUILD)/test_agent_tools
	./$(BUILD)/test_tool_shell
	./$(BUILD)/test_tool_file
	./$(BUILD)/test_config
	./$(BUILD)/test_db
	./$(BUILD)/test_telegram
	./$(BUILD)/test_dispatch
	./$(BUILD)/test_repl_tool --test
	./$(BUILD)/test_main

clean:
	rm -rf $(BUILD)
