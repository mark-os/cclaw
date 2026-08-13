/* XML.parse() — vendored yxml → fast-xml-parser-shaped JS tree.
 * Runs through the same in-process js_eval entry as test_qjs_host_eval.c. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_util.h"

static int tests_run = 0;
static int tests_passed = 0;

#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)

/* Runs `code` (a JSON-encoded JS string), expects result == want. */
static void expect_eq(const char *name, const char *code, const char *want) {
    tests_run++; printf("  %s... ", name);
    char args[2048];
    snprintf(args, sizeof(args), "{\"code\":%s}", code);
    char *r = test_js_eval_run_json(args);
    if (!r || strcmp(r, want) != 0) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    PASS();
}

static void expect_error(const char *name, const char *code, const char *needle) {
    tests_run++; printf("  %s... ", name);
    char args[2048];
    snprintf(args, sizeof(args), "{\"code\":%s}", code);
    char *r = test_js_eval_run_json(args);
    if (!r || strncmp(r, "error:", 6) != 0 || !strstr(r, needle)) {
        FAIL(r ? r : "NULL"); free(r); return;
    }
    free(r);
    PASS();
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_qjs_xml:\n");

    expect_eq("nested tree, leaf collapse",
        "\"JSON.stringify(XML.parse('<rss><channel><item><title>A</title></item></channel></rss>'))\"",
        "{\"rss\":{\"channel\":{\"item\":{\"title\":\"A\"}}}}");

    expect_eq("attributes and mixed text",
        "\"JSON.stringify(XML.parse('<a href=\\\"x\\\">t</a>'))\"",
        "{\"a\":{\"@_href\":\"x\",\"#text\":\"t\"}}");

    expect_eq("repeated siblings become array",
        "\"JSON.stringify(XML.parse('<l><i>1</i><i>2</i><i>3</i></l>'))\"",
        "{\"l\":{\"i\":[\"1\",\"2\",\"3\"]}}");

    expect_eq("self-closing empty element",
        "\"JSON.stringify(XML.parse('<a/>'))\"",
        "{\"a\":\"\"}");

    expect_eq("xml declaration skipped, whitespace trimmed",
        "\"JSON.stringify(XML.parse('<?xml version=\\\"1.0\\\"?>\\\\n<r>\\\\n  <t>  hi  </t>\\\\n</r>'))\"",
        "{\"r\":{\"t\":\"hi\"}}");

    expect_eq("entity decode",
        "\"XML.parse('<a>x &amp; &lt;y&gt;</a>').a\"",
        "x & <y>");

    expect_eq("cdata content",
        "\"XML.parse('<a><![CDATA[<raw> & stuff]]></a>').a\"",
        "<raw> & stuff");

    expect_eq("attrs on parent with children",
        "\"JSON.stringify(XML.parse('<f v=\\\"2\\\"><c>x</c></f>'))\"",
        "{\"f\":{\"@_v\":\"2\",\"c\":\"x\"}}");

    expect_error("malformed: mismatched close tag",
        "\"XML.parse('<a><b></a>')\"", "xml.parse");

    expect_error("malformed: truncated document",
        "\"XML.parse('<a><b>text')\"", "xml.parse");

    expect_error("non-string argument", "\"XML.parse()\"", "string argument required");

    expect_eq("lowercase xml alias",
        "\"JSON.stringify(xml.parse('<a>hi</a>'))\"", "{\"a\":\"hi\"}");

    /* Element names that collide with Object.prototype members: node_add's
     * lookup would find the inherited function and treat it as a sibling
     * already present, wrapping it into an array. Nodes are NULL-prototype. */
    expect_eq("name collides with prototype member",
        "\"JSON.stringify(XML.parse('<r><constructor>c</constructor><toString>t</toString></r>'))\"",
        "{\"r\":{\"constructor\":\"c\",\"toString\":\"t\"}}");

    expect_eq("__proto__ element kept as data",
        "\"JSON.stringify(XML.parse('<r><__proto__>x</__proto__></r>'))\"",
        "{\"r\":{\"__proto__\":\"x\"}}");

    expect_eq("prototype not polluted",
        "\"XML.parse('<r><__proto__><zz>1</zz></__proto__></r>'); String(({}).zz)\"",
        "undefined");

    /* Undeclared entities are fatal to a conforming parser; real feeds are
     * full of them, and losing the whole document over one is the wrong
     * trade. Known names decode, unknown ones survive as literal text. */
    expect_eq("html entity decodes",
        "\"XML.parse('<t>Fed&rsquo;s path&mdash;now</t>').t\"",
        "Fed\xe2\x80\x99s path\xe2\x80\x94now");

    expect_eq("unknown entity kept literal",
        "\"XML.parse('<t>a&foo;b</t>').t\"", "a&foo;b");

    expect_eq("bare ampersand kept literal",
        "\"XML.parse('<t>AT&T news</t>').t\"", "AT&T news");

    expect_eq("entity leniency reaches attributes",
        "\"XML.parse('<r t=\\\"A&nbsp;B&foo;\\\"/>').r['@_t']\"",
        "A\xc2\xa0" "B&foo;");

    /* CDATA and comments are literal spans — & carries no meaning there and
     * must reach yxml exactly as written, so the screening skips them. */
    expect_eq("cdata keeps entities raw",
        "\"XML.parse('<t><![CDATA[a&nbsp;b AT&T]]></t>').t\"", "a&nbsp;b AT&T");

    expect_eq("comment with entity ignored",
        "\"JSON.stringify(XML.parse('<r><!-- a&nbsp;b --><t>x</t></r>'))\"",
        "{\"r\":{\"t\":\"x\"}}");

    /* yxml's five failure codes carry the only clue about what to change. */
    expect_error("error names the mismatch",
        "\"XML.parse('<a><b></c></a>')\"", "close tag does not match");

    expect_error("error names truncation",
        "\"XML.parse('<a><b>')\"", "ended mid-element");

    printf("test_qjs_xml: %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
