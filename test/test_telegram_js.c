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
        "var __sent=[];var __emits=[];var __cfg={};var __adminArg=null;\n"
        "var channel={getConfig:function(k){return __cfg[k]!==undefined?__cfg[k]:null;},"
        "setConfig:function(k,v){__cfg[k]=v;},log:function(){},"
        "send:function(r){__sent.push(r);},"
        "emit:function(t,p,e){__emits.push({type:t,payload:p,ext:e});return 0;},"
        "admin:{isAdmin:function(id){__adminArg=id;return false;},"
        "dashboardUrl:function(){return 'http://192.0.2.1:8080/admin?token=t0k';},"
        "listPendingApprovals:function(){return [];}}};\n";
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

    /* ── utf8ByteLen: UTF-8 byte counting on UTF-16 strings ─────────── */
    expect("utf8len_ascii", "'' + utf8ByteLen('abc')", "3");
    expect("utf8len_2byte", "'' + utf8ByteLen('\\u00e9')", "2");       /* é */
    expect("utf8len_3byte", "'' + utf8ByteLen('\\u20ac')", "3");       /* € */
    expect("utf8len_astral",
        "'' + utf8ByteLen(String.fromCharCode(0xD83D,0xDE00))", "4");  /* 😀 */

    /* ── onOutbox end-to-end: mode ladder rich → html → plain ───────── */
    /* Mode 0 (auto): one sendRichMessage carrying the RAW markdown. */
    expect_has("onOutbox_rich_method",
        "onOutbox({id:1,payload:JSON.stringify({chat_id:5,text:'**hi**'}),mode:0}),"
        "__sent[0].url",
        "sendRichMessage");
    expect_has("onOutbox_rich_raw_markdown",
        "onOutbox({id:1,payload:JSON.stringify({chat_id:5,text:'**hi**'}),mode:0}),"
        "__sent[0].body",
        "\"rich_message\":{\"markdown\":\"**hi**\"}");
    /* rich_disabled latch → straight to chunked HTML. */
    expect_has("onOutbox_rich_disabled_falls_to_html",
        "channel.setConfig('rich_disabled','1'),"
        "onOutbox({id:1,payload:JSON.stringify({chat_id:5,text:'**hi**'}),mode:0}),"
        "__sent[0].body",
        "<b>hi</b>");
    /* Over the 32KB byte limit → no rich attempt, chunked HTML. */
    expect_lacks("onOutbox_oversize_no_rich",
        "onOutbox({id:1,payload:JSON.stringify({chat_id:5,text:'x'.repeat(33000)}),mode:0}),"
        "__sent[0].url",
        "sendRichMessage");
    /* Mode 1 (html): body carries HTML + parse_mode. */
    expect_has("onOutbox_html",
        "onOutbox({id:1,payload:JSON.stringify({chat_id:5,text:'**hi**'}),mode:1}),"
        "__sent[0].body",
        "<b>hi</b>");
    expect_has("onOutbox_html_parsemode",
        "onOutbox({id:1,payload:JSON.stringify({chat_id:5,text:'**hi**'}),mode:1}),"
        "__sent[0].body",
        "\"parse_mode\":\"HTML\"");
    /* Mode 2 (plain): raw text, no parse_mode. */
    expect_has("onOutbox_plain_text",
        "onOutbox({id:1,payload:JSON.stringify({chat_id:5,text:'**hi**'}),mode:2}),"
        "__sent[0].body",
        "**hi**");
    expect_lacks("onOutbox_plain_no_parsemode",
        "onOutbox({id:1,payload:JSON.stringify({chat_id:5,text:'**hi**'}),mode:2}),"
        "__sent[0].body",
        "parse_mode");

    /* ── voice messages: getFile → base64 download → media emit ─────── */
    #define VOICE_SETUP "config.token='T';config.base='https://api.telegram.org';"
    #define VOICE_MSG "{chat:{id:5}, from:{first_name:'Mark'}, voice:{file_id:'F1',duration:10,file_size:1000}}"
    /* Inbound voice → getFile with the pending tag. */
    expect_has("voice_calls_getfile",
        VOICE_SETUP "processMessage(" VOICE_MSG ", 77), __sent[0].url",
        "/getFile");
    expect("voice_getfile_tag",
        VOICE_SETUP "processMessage(" VOICE_MSG ", 77), __sent[0].tag", "vf_77");
    expect_has("voice_getfile_file_id",
        VOICE_SETUP "processMessage(" VOICE_MSG ", 77), __sent[0].body", "F1");
    /* Oversize voice → polite rejection, no getFile. */
    expect_has("voice_oversize_rejected",
        VOICE_SETUP "processMessage({chat:{id:5}, from:{}, voice:{file_id:'F1',duration:300}}, 78),"
        "__sent[0].url",
        "/sendMessage");
    expect_lacks("voice_oversize_no_getfile",
        VOICE_SETUP "processMessage({chat:{id:5}, from:{}, voice:{file_id:'F1',file_size:9999999}}, 78),"
        "__sent.map(function(s){return s.url;}).join(' ')",
        "getFile");
    /* getFile result → base64-flagged download of the file path. */
    expect_has("voice_download_url",
        VOICE_SETUP "processMessage(" VOICE_MSG ", 77);"
        "onResult({tag:'vf_77',status:200,body:JSON.stringify({ok:true,result:{file_path:'voice/f.oga'}}),error:null});"
        "__sent[1].url",
        "/file/botT/voice/f.oga");
    expect("voice_download_base64_flag",
        VOICE_SETUP "processMessage(" VOICE_MSG ", 77);"
        "onResult({tag:'vf_77',status:200,body:JSON.stringify({ok:true,result:{file_path:'voice/f.oga'}}),error:null});"
        "'' + __sent[1].base64",
        "1");
    /* Download result → emit with media payload + tg_<updateId> dedup id. */
    #define VOICE_FLOW \
        VOICE_SETUP "processMessage(" VOICE_MSG ", 77);" \
        "onResult({tag:'vf_77',status:200,body:JSON.stringify({ok:true,result:{file_path:'voice/f.oga'}}),error:null});" \
        "onResult({tag:'vd_77',status:200,body:'T2dnUw==',error:null});"
    expect("voice_emit_external_id", VOICE_FLOW "__emits[0].ext", "tg_77");
    expect_has("voice_emit_media", VOICE_FLOW "__emits[0].payload",
        "\"media\":{\"kind\":\"audio\",\"mime\":\"audio/ogg\",\"data_b64\":\"T2dnUw==\"}");
    expect_has("voice_emit_channel_id", VOICE_FLOW "__emits[0].payload", "\"channel_id\":\"5\"");
    /* Failed download → apology, no emit. */
    expect_has("voice_download_fail_apologizes",
        VOICE_SETUP "processMessage(" VOICE_MSG ", 77);"
        "onResult({tag:'vd_77',status:404,body:'',error:null});"
        "__sent[__sent.length-1].body",
        "couldn't process that voice message");
    expect("voice_download_fail_no_emit",
        VOICE_SETUP "processMessage(" VOICE_MSG ", 77);"
        "onResult({tag:'vd_77',status:404,body:'',error:null});"
        "'' + __emits.length",
        "0");

    /* ── admin gate keys on from.id, not chat.id ─────────────────────── */
    /* Group chat: chat.id is the group (-100), sender is 42 → isAdmin sees 42. */
    expect("admin_gate_uses_from_id",
        "processMessage({chat:{id:-100}, from:{id:42}, text:'/approvals'}, 1), '' + __adminArg",
        "42");

    /* ── slash-command surface: approvals + admin only ──────────────── */
    expect("commands_are_approvals_admin",
        "Object.keys(COMMANDS).sort().join(',')", "admin,approvals");
    /* /admin replies with the tokenized dashboard URL from the bridge. */
    expect_has("admin_command_sends_dashboard_url",
        "COMMANDS.admin.handler('7'); __sent[0].body",
        "http://192.0.2.1:8080/admin?token=t0k");
    /* /approvals with nothing pending says so. */
    expect_has("approvals_empty",
        "COMMANDS.approvals.handler('7'); __sent[0].body",
        "No pending approvals");

    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
