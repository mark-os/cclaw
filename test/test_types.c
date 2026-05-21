#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "types.h"

/* V14: entry has id + parent_id for tree structure */
static void test_entry_tree_fields(void) {
    Entry e = {0};
    e.id = 42;
    e.parent_id = 10;
    e.session_id = 1;
    assert(e.id == 42);
    assert(e.parent_id == 10);
    assert(e.session_id == 1);
}

static void test_entry_root_parent(void) {
    Entry e = {0};
    e.parent_id = -1;  /* root entry has no parent */
    assert(e.parent_id == -1);
}

static void test_session_leaf(void) {
    Session s = {0};
    s.id = 1;
    s.leaf_id = 99;
    assert(s.id == 1);
    assert(s.leaf_id == 99);
}

static void test_message_roles(void) {
    Message m = {0};
    m.role = ROLE_USER;
    m.content = "hello";
    assert(m.role == ROLE_USER);
    assert(strcmp(m.content, "hello") == 0);
}

static void test_config_defaults(void) {
    Config c = {0};
    assert(c.provider.base_url == NULL);
    assert(c.db_path == NULL);
    assert(c.web_port == 0);
}

static void test_struct_sizes(void) {
    assert(sizeof(Entry) > 0);
    assert(sizeof(Session) > 0);
    assert(sizeof(Message) > 0);
    assert(sizeof(Config) > 0);
    assert(sizeof(ToolCall) > 0);
    assert(sizeof(ToolResult) > 0);
}

int main(void) {
    test_entry_tree_fields();
    test_entry_root_parent();
    test_session_leaf();
    test_message_roles();
    test_config_defaults();
    test_struct_sizes();
    printf("test_types: all passed\n");
    return 0;
}
