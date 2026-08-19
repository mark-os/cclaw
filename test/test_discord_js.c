/* Unit tests for the button/interaction helpers in
 * templates/channel_discord.qjs, run against the REAL template source
 * (TPL_CHANNEL_DISCORD_QJS) so the tests can't drift from what ships. Same
 * harness as test_telegram_js.c: eval a stubbed `channel` global + the
 * template (top-level runs only declarations), then drive one function and
 * assert on the captured sends/emits. */
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
        "var __sent=[];var __emits=[];var __cfg={};var __res=[];\n"
        "var channel={getConfig:function(k){return __cfg[k]!==undefined?__cfg[k]:null;},"
        "setState:function(k,v){__cfg[k]=v;},getState:function(k){return __cfg[k]!==undefined?__cfg[k]:null;},log:function(){},"
        "send:function(r){__sent.push(r);},"
        "http:function(r){__sent.push(r);return new Promise(function(res){__res.push(res);});},"
        "emit:function(t,p,e){__emits.push({type:t,payload:p,ext:e});return 0;},"
        "conn:{open:function(){return 1;},send:function(){},close:function(){}},"
        "admin:{isAdmin:function(id){return false;},"
        "dashboardUrl:function(){return 'http://192.0.2.1:8080/admin?token=t0k';},"
        "listPendingApprovals:function(){return [];}}};\n";
    size_t n = strlen(stub) + strlen(TPL_CHANNEL_DISCORD_QJS) + strlen(expr) + 8;
    char *code = malloc(n);
    snprintf(code, n, "%s%s\n;%s", stub, TPL_CHANNEL_DISCORD_QJS, expr);
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

int main(void) {
    TEST_INIT();
    printf("test_discord_js:\n");

    /* ── kbToComponents: Telegram keyboard shape → Discord action rows ── */
    expect_has("kb_custom_id_verbatim",
        "JSON.stringify(kbToComponents([[{text:'Allow once',callback_data:'appr:7:once'}]]))",
        "\"custom_id\":\"appr:7:once\"");
    expect_has("kb_once_secondary_style",
        "JSON.stringify(kbToComponents([[{text:'Allow once',callback_data:'appr:7:once'}]]))",
        "\"style\":2");
    expect_has("kb_deny_danger_style",
        "JSON.stringify(kbToComponents([[{text:'Deny',callback_data:'appr:7:no'}]]))",
        "\"style\":4");
    expect_has("kb_yes_primary_style",
        "JSON.stringify(kbToComponents([[{text:'Always allow',callback_data:'appr:7:yes'}]]))",
        "\"style\":1");
    expect("kb_empty_is_null", "'' + kbToComponents([])", "null");

    /* ── outbox: keyboard rides the final chunk as components ── */
    expect_has("outbox_keyboard_components",
        "config.base='https://d.test'; config.token='t';"
        "onOutbox({id:9, payload: JSON.stringify({chat_id:'5', text:'Approve?',"
        " keyboard:[[{text:'Allow once',callback_data:'appr:3:once'}]]})});"
        "__sent[0].body",
        "appr:3:once");
    expect("outbox_keyboard_final",
        "config.base='https://d.test'; config.token='t';"
        "onOutbox({id:9, payload: JSON.stringify({chat_id:'5', text:'Approve?',"
        " keyboard:[[{text:'A',callback_data:'appr:3:once'}]]})});"
        "'' + __sent[0].final",
        "1");
    expect("outbox_plain_no_components",
        "config.base='https://d.test'; config.token='t';"
        "onOutbox({id:9, payload: JSON.stringify({chat_id:'5', text:'hello'})});"
        "'' + (__sent[0].body.indexOf('components') === -1)",
        "true");

    /* ── interactions: the isAdmin gate is the authority boundary ── */
    expect("interaction_non_admin_no_emit",
        "onInteraction({type:3, id:'i1', token:'tk',"
        " data:{custom_id:'appr:5:yes'}, user:{id:'99'}});"
        "'' + __emits.length",
        "0");
    expect_has("interaction_non_admin_deferred_ack",
        "onInteraction({type:3, id:'i1', token:'tk',"
        " data:{custom_id:'appr:5:yes'}, user:{id:'99'}});"
        "__sent[0].body",
        "\"type\":6");
    expect("interaction_wrong_type_ignored",
        "onInteraction({type:2, id:'i1', token:'tk',"
        " data:{custom_id:'appr:5:yes'}, user:{id:'42'}});"
        "'' + (__sent.length + __emits.length)",
        "0");
    expect_has("interaction_admin_emits_decision",
        "channel.admin.isAdmin=function(){return true;};"
        "config.base='https://d.test';"
        "onInteraction({type:3, id:'i1', token:'tk',"
        " data:{custom_id:'appr:5:yes'}, user:{id:'42',global_name:'Mark'},"
        " message:{content:'Prompt'}});"
        "__emits[0].type + ' ' + __emits[0].payload + ' ' + __emits[0].ext",
        "approval_decision {\"approval_id\":5,\"decision\":\"yes\","
        "\"sender\":\"Mark\"} disc_int_i1");
    expect_has("interaction_admin_updates_message",
        "channel.admin.isAdmin=function(){return true;};"
        "config.base='https://d.test';"
        "onInteraction({type:3, id:'i1', token:'tk',"
        " data:{custom_id:'appr:5:once'}, user:{id:'42',global_name:'Mark'},"
        " message:{content:'Prompt'}});"
        "__sent[0].body",
        "\"type\":7");
    expect_has("interaction_receipt_names_decider",
        "channel.admin.isAdmin=function(){return true;};"
        "config.base='https://d.test';"
        "onInteraction({type:3, id:'i1', token:'tk',"
        " data:{custom_id:'appr:5:once'}, user:{id:'42',global_name:'Mark'},"
        " message:{content:'Prompt'}});"
        "__sent[0].body",
        "Approved once by Mark");
    expect_has("interaction_clears_buttons",
        "channel.admin.isAdmin=function(){return true;};"
        "config.base='https://d.test';"
        "onInteraction({type:3, id:'i1', token:'tk',"
        " data:{custom_id:'appr:5:no'}, user:{id:'42'},"
        " message:{content:'Prompt'}});"
        "__sent[0].body",
        "\"components\":[]");

    /* ── /approvals digest carries per-approval button rows ── */
    expect_has("digest_has_buttons",
        "channel.admin.listPendingApprovals=function(){return"
        " [{id:4, agent:'A', tool_name:'js_eval', action:'secret_bind', args_json:'{}'}];};"
        "config.base='https://d.test'; config.token='t';"
        "sendApprovals('7'); __sent[0].body",
        "appr:4:yes");
    expect_has("digest_empty",
        "sendApprovals('7'); __sent[0].body",
        "No pending approvals");

    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
