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
        "var __sent=[];var __emits=[];var __cfg={};var __adminArg=null;var __res=[];\n"
        "var channel={getConfig:function(k){return __cfg[k]!==undefined?__cfg[k]:null;},"
        "setState:function(k,v){__cfg[k]=v;},getState:function(k){return __cfg[k]!==undefined?__cfg[k]:null;},log:function(){},"
        "send:function(r){__sent.push(r);},"
        "http:function(r){__sent.push(r);return new Promise(function(res){__res.push(res);});},"
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
        "channel.setState('rich_disabled','1'),"
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

    /* ── voice messages: getFile → channel.http download → media emit ── */
    #define VOICE_SETUP "config.token='TOK';config.base='https://api.telegram.org';"
    #define VOICE_MSG "{chat:{id:5}, from:{first_name:'Mark'}, voice:{file_id:'F1',duration:10,file_size:1000}}"

    /* Inbound voice → getFile request shape (url ends /getFile, body has file_id). */
    expect_has("voice_getfile_url",
        "(async function(){" VOICE_SETUP
        "processMessage(" VOICE_MSG ", 77);"
        "await 0;"
        "return __sent[0].url;"
        "})()",
        "/getFile");
    expect_has("voice_getfile_body_has_file_id",
        "(async function(){" VOICE_SETUP
        "processMessage(" VOICE_MSG ", 77);"
        "await 0;"
        "return __sent[0].body;"
        "})()",
        "\"file_id\":\"F1\"");

    /* Download request has save_to and url containing /file/botTOK/. */
    expect_has("voice_download_url",
        "(async function(){" VOICE_SETUP
        "processMessage(" VOICE_MSG ", 77);"
        "await 0;"
        "__res[0]({status:200, body:JSON.stringify({ok:true,result:{file_path:'voice/f.oga'}}), error:null});"
        "await 0; await 0; await 0;"
        "return __sent[1].url;"
        "})()",
        "/file/botTOK/voice/f.oga");
    expect("voice_download_save_to",
        "(async function(){" VOICE_SETUP
        "processMessage(" VOICE_MSG ", 77);"
        "await 0;"
        "__res[0]({status:200, body:JSON.stringify({ok:true,result:{file_path:'voice/f.oga'}}), error:null});"
        "await 0; await 0; await 0;"
        "return __sent[1].save_to;"
        "})()",
        "tg_77.oga");

    /* Emitted payload contains media with kind/mime/path and external id tg_77. */
    expect("voice_emit_external_id",
        "(async function(){" VOICE_SETUP
        "processMessage(" VOICE_MSG ", 77);"
        "await 0;"
        "__res[0]({status:200, body:JSON.stringify({ok:true,result:{file_path:'voice/f.oga'}}), error:null});"
        "await 0; await 0; await 0;"
        "__res[1]({status:200, path:'/tmp/spool/tg_77.oga', bytes:5, error:null});"
        "await 0; await 0; await 0;"
        "return __emits[0].ext;"
        "})()",
        "tg_77");
    expect_has("voice_emit_media_kind",
        "(async function(){" VOICE_SETUP
        "processMessage(" VOICE_MSG ", 77);"
        "await 0;"
        "__res[0]({status:200, body:JSON.stringify({ok:true,result:{file_path:'voice/f.oga'}}), error:null});"
        "await 0; await 0; await 0;"
        "__res[1]({status:200, path:'/tmp/spool/tg_77.oga', bytes:5, error:null});"
        "await 0; await 0; await 0;"
        "return __emits[0].payload;"
        "})()",
        "\"media\":{\"kind\":\"audio\",\"mime\":\"audio/ogg\",\"path\":\"/tmp/spool/tg_77.oga\"}");
    expect_has("voice_emit_channel_id",
        "(async function(){" VOICE_SETUP
        "processMessage(" VOICE_MSG ", 77);"
        "await 0;"
        "__res[0]({status:200, body:JSON.stringify({ok:true,result:{file_path:'voice/f.oga'}}), error:null});"
        "await 0; await 0; await 0;"
        "__res[1]({status:200, path:'/tmp/spool/tg_77.oga', bytes:5, error:null});"
        "await 0; await 0; await 0;"
        "return __emits[0].payload;"
        "})()",
        "\"channel_id\":\"5\"");

    /* Oversize voice (duration) → polite rejection, no http call. */
    expect_has("voice_oversize_rejected",
        "(async function(){" VOICE_SETUP
        "processMessage({chat:{id:5}, from:{}, voice:{file_id:'F1',duration:300}}, 78);"
        "await 0; await 0;"
        "return __sent[0].body;"
        "})()",
        "too long");
    expect("voice_oversize_no_http",
        "(async function(){" VOICE_SETUP
        "processMessage({chat:{id:5}, from:{}, voice:{file_id:'F1',file_size:9999999}}, 78);"
        "await 0; await 0;"
        "return __sent.map(function(s){return s.url||'';}).join(' ');"
        "})()",
        "https://api.telegram.org/botTOK/sendMessage");

    /* getFile failure → apology sendMessage. */
    expect_has("voice_getfile_fail_apologizes",
        "(async function(){" VOICE_SETUP
        "processMessage(" VOICE_MSG ", 77);"
        "await 0;"
        "__res[0]({status:500, body:'err', error:'timeout'});"
        "await 0; await 0; await 0;"
        "return __sent[__sent.length-1].body;"
        "})()",
        "process that voice message");
    expect("voice_getfile_fail_no_emit",
        "(async function(){" VOICE_SETUP
        "processMessage(" VOICE_MSG ", 77);"
        "await 0;"
        "__res[0]({status:500, body:'err', error:'timeout'});"
        "await 0; await 0; await 0;"
        "return '' + __emits.length;"
        "})()",
        "0");

    /* ── photo messages: picks largest under cap, emits kind "image" ── */
    #define PHOTO_SIZES "[{file_id:'S',file_size:1000},{file_id:'M',file_size:50000},{file_id:'L',file_size:9000000}]"
    /* 3 sizes where the biggest is over PHOTO_MAX_BYTES → middle file_id chosen. */
    expect_has("photo_picks_largest_under_cap",
        "(async function(){" VOICE_SETUP
        "processMessage({chat:{id:9}, from:{first_name:'A'}, photo:" PHOTO_SIZES "}, 80);"
        "await 0;"
        "return __sent[0].body;"
        "})()",
        "\"file_id\":\"M\"");
    /* Full photo flow → emit with kind "image". */
    expect_has("photo_emit_kind_image",
        "(async function(){" VOICE_SETUP
        "processMessage({chat:{id:9}, from:{first_name:'A'}, photo:" PHOTO_SIZES "}, 80);"
        "await 0;"
        "__res[0]({status:200, body:JSON.stringify({ok:true,result:{file_path:'photos/p.jpg'}}), error:null});"
        "await 0; await 0; await 0;"
        "__res[1]({status:200, path:'/tmp/spool/tg_80.jpg', bytes:50000, error:null});"
        "await 0; await 0; await 0;"
        "return __emits[0].payload;"
        "})()",
        "\"kind\":\"image\"");
    expect_has("photo_emit_mime",
        "(async function(){" VOICE_SETUP
        "processMessage({chat:{id:9}, from:{first_name:'A'}, photo:" PHOTO_SIZES "}, 80);"
        "await 0;"
        "__res[0]({status:200, body:JSON.stringify({ok:true,result:{file_path:'photos/p.jpg'}}), error:null});"
        "await 0; await 0; await 0;"
        "__res[1]({status:200, path:'/tmp/spool/tg_80.jpg', bytes:50000, error:null});"
        "await 0; await 0; await 0;"
        "return __emits[0].payload;"
        "})()",
        "\"mime\":\"image/jpeg\"");
    expect("photo_emit_external_id",
        "(async function(){" VOICE_SETUP
        "processMessage({chat:{id:9}, from:{first_name:'A'}, photo:" PHOTO_SIZES "}, 80);"
        "await 0;"
        "__res[0]({status:200, body:JSON.stringify({ok:true,result:{file_path:'photos/p.jpg'}}), error:null});"
        "await 0; await 0; await 0;"
        "__res[1]({status:200, path:'/tmp/spool/tg_80.jpg', bytes:50000, error:null});"
        "await 0; await 0; await 0;"
        "return __emits[0].ext;"
        "})()",
        "tg_80");
    expect_has("photo_download_save_to",
        "(async function(){" VOICE_SETUP
        "processMessage({chat:{id:9}, from:{first_name:'A'}, photo:" PHOTO_SIZES "}, 80);"
        "await 0;"
        "__res[0]({status:200, body:JSON.stringify({ok:true,result:{file_path:'photos/p.jpg'}}), error:null});"
        "await 0; await 0; await 0;"
        "return __sent[1].save_to;"
        "})()",
        "tg_80.jpg");

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
