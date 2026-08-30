/* Unit tests for templates/channel_whatsapp.qjs, run against the REAL
 * template source (TPL_CHANNEL_WHATSAPP_QJS) so the tests can't drift from
 * what ships. Same harness as test_telegram_js.c / test_discord_js.c: eval a
 * stubbed `channel` global + the template (top-level runs only declarations),
 * then drive one function and assert on the captured sends/emits. */
#define _GNU_SOURCE
#include "test_util.h"
#include "templates.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int tests_run = 0, tests_passed = 0;

/* JSON-escape src into a freshly malloc'd string (no surrounding quotes). */
static char *json_escape(const char *src) {
    size_t n = strlen(src);
    char *out = malloc(n * 6 + 1);
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
        "var __sent=[];var __emits=[];var __cfg={};var __failed=[];var __logs=[];\n"
        "var channel={getConfig:function(k){return __cfg[k]!==undefined?__cfg[k]:null;},"
        "setState:function(k,v){__cfg[k]=v;},getState:function(k){return __cfg[k]!==undefined?__cfg[k]:null;},"
        "log:function(m){__logs.push(m);},"
        "send:function(r){__sent.push(r);},"
        "http:function(r){__sent.push(r);return new Promise(function(){});},"
        "emit:function(t,p,e){__emits.push({type:t,payload:p,ext:e});return 0;},"
        "failOutbox:function(id,why){__failed.push({id:id,why:why});},"
        "admin:{isAdmin:function(){return false;}}};\n";
    size_t n = strlen(stub) + strlen(TPL_CHANNEL_WHATSAPP_QJS) + strlen(expr) + 8;
    char *code = malloc(n);
    snprintf(code, n, "%s%s\n;%s", stub, TPL_CHANNEL_WHATSAPP_QJS, expr);
    char *esc = json_escape(code);
    free(code);
    size_t an = strlen(esc) + 16;
    char *args = malloc(an);
    snprintf(args, an, "{\"code\":\"%s\"}", esc);
    free(esc);
    char *r = test_js_eval_run_json(args);
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

/* One "message" event as the bridge would deliver it. */
#define MSG_EVENT(id, msgid, text) \
    "{id:" #id ",type:'message',msg_id:'" msgid "'," \
    "chat_id:'15551234567@s.whatsapp.net',sender_id:'15551234567@s.whatsapp.net'," \
    "sender_name:'Ana',chat_type:'dm',text:'" text "'}"

int main(void) {
    TEST_INIT();
    printf("test_whatsapp_js:\n");

    /* ── onInit: poll shape, default base, saved cursor, token query ── */
    expect_has("init_default_base",
        "onInit().poll.url",
        "http://127.0.0.1:8471/v1/events?cursor=0&timeout=25");
    expect_has("init_saved_cursor",
        "__cfg['wa_cursor']='17'; onInit().poll.url",
        "cursor=17");
    expect_has("init_token_in_query",
        "__cfg['api_token']='s3cr3t'; onInit().poll.url",
        "token=s3cr3t");
    expect("init_no_token_no_query",
        "onInit(); '' + (pollShape().url.indexOf('token=') === -1)",
        "true");

    /* ── onPoll: envelope facts + external id + cursor persistence ── */
    expect_has("poll_emits_envelope",
        "onInit(); onPoll({status:200, body: JSON.stringify({cursor:5, events:["
        MSG_EVENT(5, "3EB0AA", "hello") "]})});"
        "__emits[0].payload",
        "\"chat_id\":\"15551234567@s.whatsapp.net\"");
    expect("poll_external_id",
        "onInit(); onPoll({status:200, body: JSON.stringify({cursor:5, events:["
        MSG_EVENT(5, "3EB0AA", "hello") "]})});"
        "__emits[0].ext",
        "wa_3EB0AA");
    expect("poll_cursor_persisted",
        "onInit(); onPoll({status:200, body: JSON.stringify({cursor:5, events:["
        MSG_EVENT(5, "3EB0AA", "hello") "]})});"
        "__cfg['wa_cursor']",
        "5");
    expect("poll_group_chat_type",
        "onInit(); onPoll({status:200, body: JSON.stringify({cursor:2, events:["
        "{id:2,type:'message',msg_id:'m1',chat_id:'123-456@g.us',"
        "sender_id:'1555@s.whatsapp.net',sender_name:'Ana',chat_type:'group',"
        "text:'hi',mentioned:true}]})});"
        "'' + JSON.parse(__emits[0].payload).chat_type + ':' + JSON.parse(__emits[0].payload).mentioned",
        "group:true");

    /* status events log, never emit */
    expect("poll_status_no_emit",
        "onInit(); onPoll({status:200, body: JSON.stringify({cursor:3, events:["
        "{id:3,type:'status',state:'pairing',pair_code:'ABCD-EFGH'}]})});"
        "'' + __emits.length",
        "0");
    expect_has("poll_pair_code_logged",
        "onInit(); onPoll({status:200, body: JSON.stringify({cursor:3, events:["
        "{id:3,type:'status',state:'pairing',pair_code:'ABCD-EFGH'}]})});"
        "__logs.join('|')",
        "PAIR CODE: ABCD-EFGH");

    /* bridge restart: empty answer with a LOWER cursor resyncs */
    expect("poll_restart_resync",
        "onInit(); onPoll({status:200, body: JSON.stringify({cursor:50, events:["
        MSG_EVENT(50, "m50", "x") "]})});"
        "onPoll({status:200, body: JSON.stringify({cursor:2, events:[]})});"
        "__cfg['wa_cursor']",
        "2");

    /* a failed emit halts the batch: cursor never passes the failure */
    expect("poll_emit_fail_halts",
        "onInit(); channel.emit=function(){return -1;};"
        "onPoll({status:200, body: JSON.stringify({cursor:6, events:["
        MSG_EVENT(5, "m5", "a") "," MSG_EVENT(6, "m6", "b") "]})});"
        "'' + cursor",
        "0");

    /* poll errors keep polling (backoff is C's job) */
    expect_has("poll_error_keeps_shape",
        "onInit(); onPoll({error:'connection refused'}).poll.url",
        "/v1/events?cursor=0");

    /* ── onOutbox ── */
    expect_has("outbox_send_url",
        "config.base='http://127.0.0.1:8471'; config.token='';"
        "onOutbox({id:9, payload: JSON.stringify({chat_id:'1555@s.whatsapp.net', text:'hi'})});"
        "__sent[0].url",
        "/v1/send");
    expect("outbox_final",
        "config.base='http://127.0.0.1:8471'; config.token='';"
        "onOutbox({id:9, payload: JSON.stringify({chat_id:'1555@s.whatsapp.net', text:'hi'})});"
        "'' + __sent[0].final + ':' + __sent[0].outbox_id",
        "1:9");
    expect_has("outbox_body",
        "config.base='http://127.0.0.1:8471'; config.token='';"
        "onOutbox({id:9, payload: JSON.stringify({chat_id:'1555@s.whatsapp.net', text:'hi'})});"
        "__sent[0].body",
        "\"text\":\"hi\"");
    expect("outbox_bad_payload_fails",
        "onOutbox({id:4, payload:'not json'}); '' + __failed[0].id",
        "4");
    expect("outbox_missing_fields_fails",
        "onOutbox({id:5, payload: JSON.stringify({text:'no chat'})}); '' + __failed[0].id",
        "5");

    printf("test_whatsapp_js: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
