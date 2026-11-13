#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "test_util.h"
#include "tool_memory.h"
#include "tools.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Memory tools: blocks (memory_block_create/update) hold notes
 * (memory_note_add/edit/delete), and a note is addressed by its row id. */

typedef struct { sqlite3 *db; ToolRegistry reg; ToolMemoryCtx ctx; } Fx;

static void fx_open(Fx *f, const char *agent) {
    f->db = test_db_open(":memory:");
    assert(f->db);
    test_seed_agent(f->db, "bot");
    test_seed_agent(f->db, "other");
    tools_init(&f->reg);
    f->ctx = (ToolMemoryCtx){.db = f->db, .agent_name = agent};
    tool_memory_register(&f->reg, &f->ctx);
}

static void fx_close(Fx *f) { tools_free(&f->reg); db_close(f->db); }

/* Run a tool; returns the result (caller frees), *err = its is_error. */
static char *call(Fx *f, const char *tool, const char *args, int *err) {
    ToolEntry *e = tools_lookup(&f->reg, tool);
    assert(e);
    int ie = 0;
    char *r = e->handler(args, e->user_data, &ie);
    assert(r);
    if (err) *err = ie;
    return r;
}

static void ok(Fx *f, const char *tool, const char *args) {
    int err = 0;
    char *r = call(f, tool, args, &err);
    if (err) { printf("unexpected error from %s: %s\n", tool, r); assert(!err); }
    free(r);
}

static int64_t block_id(sqlite3 *db, const char *agent, const char *label) {
    MemoryBlock *mb = memory_block_get(db, agent, label);
    assert(mb);
    int64_t id = mb->id;
    memory_block_free(mb);
    return id;
}

/* Notes of a block; caller frees with memory_notes_free. */
static MemoryNote *notes(sqlite3 *db, const char *agent, const char *label, int *n) {
    return memory_notes_list(db, block_id(db, agent, label), n);
}

static void test_create_and_add(void) {
    Fx f; fx_open(&f, "bot");
    ok(&f, "memory_block_create", "{\"label\":\"facts\",\"description\":\"user facts\"}");

    int err = 0;
    char *r = call(&f, "memory_note_add", "{\"block\":\"facts\",\"text\":\"likes coffee\"}", &err);
    assert(!err);
    int n = 0;
    MemoryNote *list = notes(f.db, "bot", "facts", &n);
    assert(n == 1 && strcmp(list[0].text, "likes coffee") == 0);
    /* The result names the new id and echoes the block with [id] handles —
     * what the model needs to address the note next. */
    char want[64];
    snprintf(want, sizeof(want), "added note [%lld]", (long long)list[0].id);
    assert(strstr(r, want));
    /* "[id date] text": the date is when the text last changed (today). */
    char day[16];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(day, sizeof(day), "%Y-%m-%d", &tm);
    snprintf(want, sizeof(want), "[%lld %s] likes coffee", (long long)list[0].id, day);
    assert(strstr(r, want));
    free(r);
    memory_notes_free(list, n);

    /* A new block is 'context' placement unless asked otherwise. */
    MemoryBlock *mb = memory_block_get(f.db, "bot", "facts");
    assert(strcmp(mb->placement, "context") == 0);
    assert(strcmp(mb->description, "user facts") == 0);
    memory_block_free(mb);

    /* Duplicate label fails. */
    r = call(&f, "memory_block_create", "{\"label\":\"facts\",\"description\":\"dup\"}", &err);
    assert(err && strstr(r, "error"));
    free(r);
    fx_close(&f);
    printf("PASS: test_create_and_add\n");
}

static void test_edit_by_id(void) {
    Fx f; fx_open(&f, "bot");
    ok(&f, "memory_block_create", "{\"label\":\"bio\",\"description\":\"about\"}");
    ok(&f, "memory_note_add", "{\"block\":\"bio\",\"text\":\"age 30\"}");
    ok(&f, "memory_note_add", "{\"block\":\"bio\",\"text\":\"city NYC\"}");

    int n = 0;
    MemoryNote *list = notes(f.db, "bot", "bio", &n);
    assert(n == 2);
    char args[256];
    snprintf(args, sizeof(args),
             "{\"block\":\"bio\",\"notes\":[{\"id\":%lld,\"text\":\"age 31\"},"
             "{\"id\":%lld,\"text\":\"city LA\"}]}",
             (long long)list[0].id, (long long)list[1].id);
    int64_t id0 = list[0].id;
    memory_notes_free(list, n);

    int err = 0;
    char *r = call(&f, "memory_note_edit", args, &err);
    assert(!err && strstr(r, "edited 2 notes"));
    free(r);
    list = notes(f.db, "bot", "bio", &n);
    assert(n == 2 && list[0].id == id0);          /* ids unchanged by an edit */
    assert(strcmp(list[0].text, "age 31") == 0);
    assert(strcmp(list[1].text, "city LA") == 0);
    memory_notes_free(list, n);

    /* An id that is not in this block is named, and with nothing edited the
     * call is an error — never a quiet success. */
    r = call(&f, "memory_note_edit",
             "{\"block\":\"bio\",\"notes\":[{\"id\":99999,\"text\":\"x\"}]}", &err);
    assert(err && strstr(r, "no note [99999]"));
    free(r);
    fx_close(&f);
    printf("PASS: test_edit_by_id\n");
}

/* The reason for ids: a delete must not change what any other handle means.
 * Under the old 1..N numbering, deleting #1 turned "#3" into a different
 * note; here every surviving id still names the same text. */
static void test_delete_keeps_ids(void) {
    Fx f; fx_open(&f, "bot");
    ok(&f, "memory_block_create", "{\"label\":\"log\",\"description\":\"events\"}");
    ok(&f, "memory_note_add", "{\"block\":\"log\",\"text\":\"one\"}");
    ok(&f, "memory_note_add", "{\"block\":\"log\",\"text\":\"two\"}");
    ok(&f, "memory_note_add", "{\"block\":\"log\",\"text\":\"three\"}");

    int n = 0;
    MemoryNote *list = notes(f.db, "bot", "log", &n);
    assert(n == 3);
    int64_t a = list[0].id, b = list[1].id, c = list[2].id;
    memory_notes_free(list, n);

    char args[160];
    snprintf(args, sizeof(args), "{\"block\":\"log\",\"ids\":[%lld]}", (long long)a);
    int err = 0;
    char *r = call(&f, "memory_note_delete", args, &err);
    assert(!err && strstr(r, "deleted 1 note"));
    free(r);

    /* Edit the note that WAS third, by the id it always had. */
    snprintf(args, sizeof(args),
             "{\"block\":\"log\",\"notes\":[{\"id\":%lld,\"text\":\"THREE\"}]}", (long long)c);
    ok(&f, "memory_note_edit", args);

    list = notes(f.db, "bot", "log", &n);
    assert(n == 2);
    assert(list[0].id == b && strcmp(list[0].text, "two") == 0);
    assert(list[1].id == c && strcmp(list[1].text, "THREE") == 0);
    memory_notes_free(list, n);

    /* A deleted id is never handed out again. */
    ok(&f, "memory_note_add", "{\"block\":\"log\",\"text\":\"four\"}");
    list = notes(f.db, "bot", "log", &n);
    assert(n == 3 && list[2].id > c && list[2].id != a);
    memory_notes_free(list, n);

    /* Deleting it twice reports the id and fails. */
    snprintf(args, sizeof(args), "{\"block\":\"log\",\"ids\":[%lld]}", (long long)a);
    r = call(&f, "memory_note_delete", args, &err);
    assert(err && strstr(r, "deleted 0 notes; no note ["));
    free(r);
    fx_close(&f);
    printf("PASS: test_delete_keeps_ids\n");
}

/* A note id from one block cannot change a note through another block. */
static void test_id_scoped_to_block(void) {
    Fx f; fx_open(&f, "bot");
    ok(&f, "memory_block_create", "{\"label\":\"a\",\"description\":\"A\"}");
    ok(&f, "memory_block_create", "{\"label\":\"b\",\"description\":\"B\"}");
    ok(&f, "memory_note_add", "{\"block\":\"a\",\"text\":\"in a\"}");
    int n = 0;
    MemoryNote *list = notes(f.db, "bot", "a", &n);
    int64_t ida = list[0].id;
    memory_notes_free(list, n);

    char args[160];
    int err = 0;
    snprintf(args, sizeof(args),
             "{\"block\":\"b\",\"notes\":[{\"id\":%lld,\"text\":\"hijack\"}]}", (long long)ida);
    char *r = call(&f, "memory_note_edit", args, &err);
    assert(err); free(r);
    snprintf(args, sizeof(args), "{\"block\":\"b\",\"ids\":[%lld]}", (long long)ida);
    r = call(&f, "memory_note_delete", args, &err);
    assert(err); free(r);

    list = notes(f.db, "bot", "a", &n);
    assert(n == 1 && strcmp(list[0].text, "in a") == 0);
    memory_notes_free(list, n);
    fx_close(&f);
    printf("PASS: test_id_scoped_to_block\n");
}

static void test_missing_fields(void) {
    Fx f; fx_open(&f, "bot");
    int err = 0;
    char *r = call(&f, "memory_block_create", "{}", &err);
    assert(err && strstr(r, "error")); free(r);
    r = call(&f, "memory_note_add", "{\"block\":\"x\"}", &err);
    assert(err && strstr(r, "error")); free(r);
    r = call(&f, "memory_note_add", "{\"block\":\"nope\",\"text\":\"y\"}", &err);
    assert(err && strstr(r, "not found")); free(r);
    r = call(&f, "memory_note_edit", "{\"block\":\"nope\"}", &err);
    assert(err && strstr(r, "'notes'")); free(r);
    r = call(&f, "memory_note_delete", "{\"block\":\"nope\",\"ids\":5}", &err);
    assert(err && strstr(r, "'ids'")); free(r);
    r = call(&f, "memory_block_create",
             "{\"label\":\"x\",\"description\":\"d\",\"placement\":\"elsewhere\"}", &err);
    assert(err && strstr(r, "placement")); free(r);
    fx_close(&f);
    printf("PASS: test_missing_fields\n");
}

static void test_block_update(void) {
    Fx f; fx_open(&f, "bot");
    ok(&f, "memory_block_create",
       "{\"label\":\"notes\",\"description\":\"scratch\",\"placement\":\"system\"}");
    ok(&f, "memory_block_update", "{\"block\":\"notes\",\"placement\":\"context\"}");
    MemoryBlock *mb = memory_block_get(f.db, "bot", "notes");
    assert(strcmp(mb->placement, "context") == 0);
    assert(strcmp(mb->description, "scratch") == 0);   /* untouched */
    memory_block_free(mb);

    ok(&f, "memory_block_update", "{\"block\":\"notes\",\"description\":\"running notes\"}");
    mb = memory_block_get(f.db, "bot", "notes");
    assert(strcmp(mb->description, "running notes") == 0);
    assert(strcmp(mb->placement, "context") == 0);
    memory_block_free(mb);

    int err = 0;
    char *r = call(&f, "memory_block_update", "{\"block\":\"notes\"}", &err);
    assert(err); free(r);
    r = call(&f, "memory_block_update", "{\"block\":\"notes\",\"placement\":\"x\"}", &err);
    assert(err); free(r);
    r = call(&f, "memory_block_update", "{\"block\":\"nope\",\"placement\":\"system\"}", &err);
    assert(err && strstr(r, "not found")); free(r);
    fx_close(&f);
    printf("PASS: test_block_update\n");
}

static void test_read_only(void) {
    Fx f; fx_open(&f, "bot");
    ok(&f, "memory_block_create", "{\"label\":\"locked\",\"description\":\"immutable\"}");
    int64_t bid = block_id(f.db, "bot", "locked");
    int64_t nid = memory_note_add(f.db, bid, "fixed");
    assert(nid > 0);
    assert(memory_block_set_limits(f.db, bid, 5000, 1) == 0);

    char args[160];
    int err = 0;
    char *r = call(&f, "memory_note_add", "{\"block\":\"locked\",\"text\":\"more\"}", &err);
    assert(err && strstr(r, "read-only")); free(r);
    snprintf(args, sizeof(args),
             "{\"block\":\"locked\",\"notes\":[{\"id\":%lld,\"text\":\"changed\"}]}", (long long)nid);
    r = call(&f, "memory_note_edit", args, &err);
    assert(err && strstr(r, "read-only")); free(r);
    snprintf(args, sizeof(args), "{\"block\":\"locked\",\"ids\":[%lld]}", (long long)nid);
    r = call(&f, "memory_note_delete", args, &err);
    assert(err && strstr(r, "read-only")); free(r);
    r = call(&f, "memory_block_update", "{\"block\":\"locked\",\"placement\":\"system\"}", &err);
    assert(err && strstr(r, "read-only")); free(r);

    int n = 0;
    MemoryNote *list = memory_notes_list(f.db, bid, &n);
    assert(n == 1 && strcmp(list[0].text, "fixed") == 0);
    memory_notes_free(list, n);
    fx_close(&f);
    printf("PASS: test_read_only\n");
}

/* A shared block: the owner writes, a read-only holder sees it by the same
 * label and is refused, an agent it is not attached to cannot find it, and
 * the operator can flip the attachment to read-write. */
static void test_shared_block(void) {
    Fx f; fx_open(&f, "bot");
    ok(&f, "memory_block_create", "{\"label\":\"team\",\"description\":\"shared facts\"}");
    ok(&f, "memory_note_add", "{\"block\":\"team\",\"text\":\"standup at 9\"}");
    int64_t bid = block_id(f.db, "bot", "team");

    /* Not attached: invisible. */
    Fx g; g.db = f.db; tools_init(&g.reg);
    g.ctx = (ToolMemoryCtx){.db = f.db, .agent_name = "other"};
    tool_memory_register(&g.reg, &g.ctx);
    int err = 0;
    char *r = call(&g, "memory_note_add", "{\"block\":\"team\",\"text\":\"x\"}", &err);
    assert(err && strstr(r, "not found")); free(r);
    assert(memory_block_get(f.db, "other", "team") == NULL);

    /* Attached read-only: visible, not writable. */
    assert(memory_block_attach(f.db, "other", bid, 1) == 0);
    MemoryBlock *mb = memory_block_get(f.db, "other", "team");
    assert(mb && mb->id == bid && mb->read_only == 1);
    assert(strcmp(mb->agent_name, "bot") == 0);        /* owner unchanged */
    memory_block_free(mb);
    r = call(&g, "memory_note_add", "{\"block\":\"team\",\"text\":\"x\"}", &err);
    assert(err && strstr(r, "read-only")); free(r);
    /* …while the owner still writes. */
    ok(&f, "memory_note_add", "{\"block\":\"team\",\"text\":\"retro friday\"}");

    /* The holder cannot take a second block under the same label. */
    r = call(&g, "memory_block_create", "{\"label\":\"team\",\"description\":\"mine\"}", &err);
    assert(err); free(r);
    int64_t own = memory_block_create(f.db, NULL, "team", "operator's", 5000, NULL);
    assert(own > 0);
    assert(memory_block_attach(f.db, "other", own, 1) == -1);

    /* Operator makes the attachment read-write: now the write lands in the
     * one shared block both agents see. */
    assert(memory_block_attach(f.db, "other", bid, 0) == 0);
    ok(&g, "memory_note_add", "{\"block\":\"team\",\"text\":\"from other\"}");
    int n = 0;
    MemoryNote *list = memory_notes_list(f.db, bid, &n);
    assert(n == 3 && strcmp(list[2].text, "from other") == 0);
    memory_notes_free(list, n);

    /* Detach: gone for that agent, intact for the owner. */
    assert(memory_block_detach(f.db, "other", bid) == 0);
    assert(memory_block_get(f.db, "other", "team") == NULL);
    mb = memory_block_get(f.db, "bot", "team");
    assert(mb); memory_block_free(mb);

    /* Deleting a block takes its notes and attachments with it. */
    assert(memory_block_delete(f.db, bid) == 0);
    assert(memory_block_get(f.db, "bot", "team") == NULL);
    list = memory_notes_list(f.db, bid, &n);
    assert(n == 0 && list == NULL);

    tools_free(&g.reg);
    fx_close(&f);
    printf("PASS: test_shared_block\n");
}

static void test_char_limit(void) {
    Fx f; fx_open(&f, "bot");
    int64_t bid = memory_block_create(f.db, "bot", "tiny", "small block", 5, NULL);
    assert(bid > 0 && memory_note_add(f.db, bid, "hi") > 0);   /* 2 chars */
    int err = 0;
    /* 4 more (total 6 > 5) is refused; 3 more fits exactly. The refusal
     * says what to do about it and shows the block to prune. */
    char *r = call(&f, "memory_note_add", "{\"block\":\"tiny\",\"text\":\"1234\"}", &err);
    assert(err && strstr(r, "block is full"));
    assert(strstr(r, "delete notes that are out of date"));
    assert(strstr(r, "] hi"));
    free(r);
    ok(&f, "memory_note_add", "{\"block\":\"tiny\",\"text\":\"123\"}");

    /* An edit is held to the same limit (it used to bypass it): growing a
     * note past the budget is refused and leaves the text alone… */
    int n = 0;
    MemoryNote *list = memory_notes_list(f.db, bid, &n);
    assert(n == 2);
    char args[200];
    snprintf(args, sizeof(args),
             "{\"block\":\"tiny\",\"notes\":[{\"id\":%lld,\"text\":\"hello\"}]}",
             (long long)list[0].id);
    r = call(&f, "memory_note_edit", args, &err);
    assert(err && strstr(r, "not changed") && strstr(r, "block is full"));
    free(r);
    /* …while an edit that does not lengthen it always goes through, so a
     * full block can be edited down. */
    snprintf(args, sizeof(args),
             "{\"block\":\"tiny\",\"notes\":[{\"id\":%lld,\"text\":\"yo\"}]}",
             (long long)list[0].id);
    ok(&f, "memory_note_edit", args);
    /* Even when the operator has since lowered the limit below what is
     * already stored. */
    assert(memory_block_set_limits(f.db, bid, 2, 0) == 0);
    snprintf(args, sizeof(args),
             "{\"block\":\"tiny\",\"notes\":[{\"id\":%lld,\"text\":\"y\"}]}",
             (long long)list[0].id);
    ok(&f, "memory_note_edit", args);
    memory_notes_free(list, n);
    list = memory_notes_list(f.db, bid, &n);
    assert(strcmp(list[0].text, "y") == 0 && strcmp(list[1].text, "123") == 0);
    memory_notes_free(list, n);
    fx_close(&f);
    printf("PASS: test_char_limit\n");
}

int main(void) {
    TEST_INIT();
    test_create_and_add();
    test_edit_by_id();
    test_delete_keeps_ids();
    test_id_scoped_to_block();
    test_missing_fields();
    test_block_update();
    test_read_only();
    test_shared_block();
    test_char_limit();
    printf("All tool_memory tests passed.\n");
    return 0;
}
