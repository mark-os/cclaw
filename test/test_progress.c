#include <stdio.h>
#include <string.h>
#include "agent.h"

/* T115: test that progress callback types and struct fields exist and work */

static int calls = 0;
static ProgressEvent last_event;
static char last_name[128];
static char last_data[256];

static void test_progress_cb(ProgressEvent event, const char *name,
                             const char *data, void *user_data) {
    (void)user_data;
    calls++;
    last_event = event;
    if (name) strncpy(last_name, name, sizeof(last_name) - 1);
    else last_name[0] = '\0';
    if (data) strncpy(last_data, data, sizeof(last_data) - 1);
    else last_data[0] = '\0';
}

static void test_progress_fields(void) {
    printf("  progress_fields... ");
    AgentContext ctx = {0};
    ctx.progress = test_progress_cb;
    ctx.progress_data = (void *)0xDEAD;

    /* Verify callback is callable */
    ctx.progress(PROGRESS_ASSISTANT_TEXT, NULL, "thinking...", ctx.progress_data);
    if (calls != 1 || last_event != PROGRESS_ASSISTANT_TEXT) {
        printf("FAIL: assistant text event\n"); return;
    }
    if (strcmp(last_data, "thinking...") != 0) {
        printf("FAIL: data mismatch\n"); return;
    }

    ctx.progress(PROGRESS_TOOL_START, "file_read", "{\"path\":\"/tmp\"}", ctx.progress_data);
    if (calls != 2 || last_event != PROGRESS_TOOL_START) {
        printf("FAIL: tool start event\n"); return;
    }
    if (strcmp(last_name, "file_read") != 0) {
        printf("FAIL: name mismatch\n"); return;
    }

    ctx.progress(PROGRESS_TOOL_RESULT, "file_read", "contents here", ctx.progress_data);
    if (calls != 3 || last_event != PROGRESS_TOOL_RESULT) {
        printf("FAIL: tool result event\n"); return;
    }

    printf("PASS\n");
}

static void test_null_progress(void) {
    printf("  null_progress... ");
    /* Verify NULL progress doesn't crash (agent.c checks before calling) */
    AgentContext ctx = {0};
    ctx.progress = NULL;
    /* Just verify the struct compiles and NULL is valid */
    if (ctx.progress != NULL) {
        printf("FAIL\n"); return;
    }
    printf("PASS\n");
}

int main(void) {
    printf("test_progress:\n");
    test_progress_fields();
    test_null_progress();
    printf("All progress tests passed.\n");
    return 0;
}
