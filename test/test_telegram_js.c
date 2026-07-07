/* Unit tests for the pure helpers in templates/channel_telegram.qjs, run
 * against the REAL template source (TPL_CHANNEL_TELEGRAM_QJS) so the tests
 * can't drift from what ships. The template is eval'd via the qjs host with a
 * stubbed `channel` global — only its top-level runs (locale parse + function
 * / COMMANDS defs); no handler is invoked — then we call the pure function
 * under test and assert on its string result. */
#define _GNU_SOURCE
#include "tool_js.h"
#include "templates.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int tests_run = 0, tests_passed = 0;

/* JSON-escape src into a freshly malloc'd string (no surrounding quotes). */
static char *json_escape(const char *src) {
    size_t n = strlen(src);
    char *out = malloc(n * 6 + 1);   /* worst case every char → \u00XX */
    char *p = out;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)src[i];
        switch (c) {
            case '"':  *p++ = '\\'; *p++ = '"';  break;
            case '\\': *p++ = '\\'; *p++ = '\\'; break;
            case '\n': *p++ = '\\'; *p++ = 'n';  break;
            case '\r': *p++ = '\\'; *p++ = 'r';  break;
            case '\t': *p++ = '\\'; *p++ = 't';  break;
            default:
                if (c < 0x20) { p += sprintf(p, "\\u%04x", c); }
                else *p++ = (char)c;
        }
    }
    *p = '\0';
    return out;
}

/* Eval `channel-stub + real template + ; + expr`; return the result string. */
static char *eval_expr(const char *expr) {
    const char *stub =
        "var __sent=[];var __cfg={};var __adminArg=null;\n"
        "var channel={getConfig:function(k){return __cfg[k]!==undefined?__cfg[k]:null;},"
        "setConfig:function(k,v){__cfg[k]=v;},log:function(){},"
        "send:function(r){__sent.push(r);},emit:function(){return 0;},"
        "admin:{isAdmin:function(id){__adminArg=id;return false;}}};\n";
    size_t n = strlen(stub) + strlen(TPL_CHANNEL_TELEGRAM_QJS) + strlen(expr) + 8;
    char *code = malloc(n);
    snprintf(code, n, "%s%s\n;%s", stub, TPL_CHANNEL_TELEGRAM_QJS, expr);
    char *esc = json_escape(code);
    free(code);
    size_t an = strlen(esc) + 16;
    char *args = malloc(an);
    snprintf(args, an, "{\"code\":\"%s\"}", esc);
    free(esc);
    char *r = tool_js_eval_handler(args, NULL);
    free(args);
    return r;
}

static void expect(const char *name, const char *expr, const char *want) {
    tests_run++;
    printf("  %s... ", name);
    char *r = eval_expr(expr);
    if (!r || strcmp(r, want) != 0) {
        printf("FAIL: got '%s' want '%s'\n", r ? r : "NULL", want);
    } else {
        tests_passed++;
        printf("PASS\n");
    }
    free(r);
}

static void expect_has(const char *name, const char *expr, const char *needle) {
    tests_run++;
    printf("  %s... ", name);
    char *r = eval_expr(expr);
    if (!r || !strstr(r, needle)) {
        printf("FAIL: '%s' missing '%s'\n", r ? r : "NULL", needle);
    } else {
        tests_passed++;
        printf("PASS\n");
    }
    free(r);
}

static void expect_lacks(const char *name, const char *expr, const char *needle) {
    tests_run++;
    printf("  %s... ", name);
    char *r = eval_expr(expr);
    if (!r || strstr(r, needle)) {
        printf("FAIL: '%s' should not contain '%s'\n", r ? r : "NULL", needle);
    } else {
        tests_passed++;
        printf("PASS\n");
    }
    free(r);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_telegram_js:\n");

    /* ── findSplit: UTF-16 surrogate-safe hard cut ──────────────────── */
    /* 4095 'a', then a surrogate pair at [4095,4096], then filler. A raw cut
     * at 4096 lands on the low surrogate; the guard backs up to 4095 so the
     * pair stays intact. */
    expect("findSplit_backs_off_low_surrogate",
        "'' + findSplit('a'.repeat(4095) + String.fromCharCode(0xD83D,0xDE00) + 'bb', 0, 4096)",
        "4095");
    /* No surrogate at the boundary → plain hard cut at maxLen. */
    expect("findSplit_plain_hard_cut",
        "'' + findSplit('a'.repeat(5000), 0, 4096)",
        "4096");
    /* A newline before the cut is preferred over the hard cut. */
    expect("findSplit_prefers_newline",
        "'' + findSplit('a'.repeat(100) + '\\n' + 'b'.repeat(5000), 0, 4096)",
        "101");

    /* ── messageKind: non-text update classification ────────────────── */
    expect("messageKind_photo", "messageKind({photo:[{}]})", "photo");
    expect("messageKind_voice", "messageKind({voice:{}})", "voice");
    expect("messageKind_sticker", "messageKind({sticker:{}})", "sticker");
    expect("messageKind_none", "messageKind({})", "non-text");

    /* ── markdownToTelegramHtml ─────────────────────────────────────── */
    expect("md_bold",    "markdownToTelegramHtml('a **b** c')", "a <b>b</b> c");
    expect("md_italic",  "markdownToTelegramHtml('a *b* c')",   "a <i>b</i> c");
    expect("md_code",    "markdownToTelegramHtml('run `ls` now')", "run <code>ls</code> now");
    expect("md_link",    "markdownToTelegramHtml('[x](http://y.z)')", "<a href=\"http://y.z\">x</a>");
    /* &<> escaped in plain text */
    expect("md_escape",  "markdownToTelegramHtml('1 < 2 & 3 > 0')", "1 &lt; 2 &amp; 3 &gt; 0");
    /* code content with < is escaped, not treated as a tag */
    expect("md_code_escapes", "markdownToTelegramHtml('`a<b>`')", "<code>a&lt;b&gt;</code>");
    /* unbalanced marker (chunk-split bold) degrades to literal text */
    expect("md_unbalanced", "markdownToTelegramHtml('a **b c')", "a **b c");
    /* digits in text must not be mistaken for stash placeholders */
    expect("md_digits_safe", "markdownToTelegramHtml('order 66 and `x`')",
           "order 66 and <code>x</code>");
    /* GFM table wrapped verbatim in <pre> (monospace keeps columns aligned) */
    expect("md_table",
           "markdownToTelegramHtml('| a | b |\\n|---|---|\\n| 1 | 2 |')",
           "<pre>| a | b |\n|---|---|\n| 1 | 2 |</pre>");
    /* table with surrounding prose keeps the prose, wraps only the block */
    expect("md_table_prose",
           "markdownToTelegramHtml('before\\n| a | b |\\n| - | - |\\n| 1 | 2 |\\nafter')",
           "before\n<pre>| a | b |\n| - | - |\n| 1 | 2 |</pre>\nafter");
    /* a lone dashes line (no pipe) is not a table delimiter */
    expect("md_not_table", "markdownToTelegramHtml('a | b\\n---')", "a | b\n---");

    /* ── onOutbox end-to-end: real onOutbox→sendChunked→tgCall→send ─── */
    /* Rich (default): body carries HTML + parse_mode. */
    expect_has("onOutbox_rich_html",
        "onOutbox({id:1,payload:JSON.stringify({chat_id:5,text:'**hi**'}),plain:false}),"
        "__sent[0].body",
        "<b>hi</b>");
    expect_has("onOutbox_rich_parsemode",
        "onOutbox({id:1,payload:JSON.stringify({chat_id:5,text:'**hi**'}),plain:false}),"
        "__sent[0].body",
        "\"parse_mode\":\"HTML\"");
    /* Plain (fallback): raw text, no parse_mode. */
    expect_has("onOutbox_plain_text",
        "onOutbox({id:1,payload:JSON.stringify({chat_id:5,text:'**hi**'}),plain:true}),"
        "__sent[0].body",
        "**hi**");
    expect_lacks("onOutbox_plain_no_parsemode",
        "onOutbox({id:1,payload:JSON.stringify({chat_id:5,text:'**hi**'}),plain:true}),"
        "__sent[0].body",
        "parse_mode");

    /* ── P4a: dialog state persists to channel_state ─────────────────── */
    /* setDialog writes the whole dialogs map to the "dialogs" config key so a
     * restart's onInit can restore it (round-trip is the mechanism onInit uses). */
    expect_has("dialog_persist_set",
        "setDialog('7', {type:'key', provider:'openrouter'}), channel.getConfig('dialogs')",
        "\"type\":\"key\"");
    expect("dialog_persist_clear",
        "setDialog('7', {type:'key'}), clearDialog('7'), channel.getConfig('dialogs')",
        "{}");

    /* ── P4c: admin gate keys on from.id, not chat.id ────────────────── */
    /* Group chat: chat.id is the group (-100), sender is 42 → isAdmin sees 42. */
    expect("admin_gate_uses_from_id",
        "processMessage({chat:{id:-100}, from:{id:42}, text:'/grants'}, 1), '' + __adminArg",
        "42");

    /* ── P4b: tracked secret-scrub delete + onResult retry/warn ──────── */
    expect("del_tracked_tag",
        "deleteSensitiveMessage('9', 123, 1), __sent[0].tag",
        "del:9:123:1");
    expect_has("del_is_deleteMessage",
        "deleteSensitiveMessage('9', 123, 1), __sent[0].url",
        "deleteMessage");
    /* Attempt 1 fails → retry as attempt 2. */
    expect("del_retry_once",
        "deleteSensitiveMessage('9',123,1),"
        "onResult({tag:'del:9:123:1',status:400,body:'no rights'}), __sent[1].tag",
        "del:9:123:2");
    /* Attempt 2 fails → warn the user (a tagless sendMessage), no third delete. */
    expect_has("del_warn_on_give_up",
        "onResult({tag:'del:9:123:2',status:400,body:'no rights'}), __sent[0].body",
        "manually");
    /* Success → no retry, no warn. */
    expect("del_success_no_followup",
        "deleteSensitiveMessage('9',123,1),"
        "onResult({tag:'del:9:123:1',status:200,body:'ok'}), '' + __sent.length",
        "1");
    /* 'message to delete not found' → already gone, no warn. */
    expect("del_not_found_no_warn",
        "deleteSensitiveMessage('9',123,2),"
        "onResult({tag:'del:9:123:2',status:400,body:'Bad Request: message to delete not found'}),"
        "'' + __sent.length",
        "1");

    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
