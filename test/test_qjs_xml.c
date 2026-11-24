/* XML.parse() — vendored yxml → fast-xml-parser-shaped JS tree.
 * Runs through the same in-process js_eval entry as test_qjs_host_eval.c. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_util.h"

/* Each row runs `code` (a JSON-encoded JS string). A value row expects the
 * result to equal `want`; an error row expects "error:" carrying `want`. */
static const struct {
    int is_error;
    const char *name;
    const char *code;
    const char *want;
} cases[] = {
    { 0, "nested tree, leaf collapse",
        "\"JSON.stringify(XML.parse('<rss><channel><item><title>A</title></item></channel></rss>'))\"",
        "{\"rss\":{\"channel\":{\"item\":{\"title\":\"A\"}}}}" },

    { 0, "attributes and mixed text",
        "\"JSON.stringify(XML.parse('<a href=\\\"x\\\">t</a>'))\"",
        "{\"a\":{\"@_href\":\"x\",\"#text\":\"t\"}}" },

    { 0, "repeated siblings become array",
        "\"JSON.stringify(XML.parse('<l><i>1</i><i>2</i><i>3</i></l>'))\"",
        "{\"l\":{\"i\":[\"1\",\"2\",\"3\"]}}" },

    { 0, "self-closing empty element",
        "\"JSON.stringify(XML.parse('<a/>'))\"",
        "{\"a\":\"\"}" },

    { 0, "xml declaration skipped, whitespace trimmed",
        "\"JSON.stringify(XML.parse('<?xml version=\\\"1.0\\\"?>\\\\n<r>\\\\n  <t>  hi  </t>\\\\n</r>'))\"",
        "{\"r\":{\"t\":\"hi\"}}" },

    { 0, "entity decode",
        "\"XML.parse('<a>x &amp; &lt;y&gt;</a>').a\"",
        "x & <y>" },

    { 0, "cdata content",
        "\"XML.parse('<a><![CDATA[<raw> & stuff]]></a>').a\"",
        "<raw> & stuff" },

    { 0, "attrs on parent with children",
        "\"JSON.stringify(XML.parse('<f v=\\\"2\\\"><c>x</c></f>'))\"",
        "{\"f\":{\"@_v\":\"2\",\"c\":\"x\"}}" },

    { 1, "malformed: mismatched close tag",
        "\"XML.parse('<a><b></a>')\"", "xml.parse" },

    { 1, "malformed: truncated document",
        "\"XML.parse('<a><b>text')\"", "xml.parse" },

    { 1, "non-string argument", "\"XML.parse()\"", "string argument required" },

    { 0, "lowercase xml alias",
        "\"JSON.stringify(xml.parse('<a>hi</a>'))\"", "{\"a\":\"hi\"}" },

    /* Element names that collide with Object.prototype members: node_add's
     * lookup would find the inherited function and treat it as a sibling
     * already present, wrapping it into an array. Nodes are NULL-prototype. */
    { 0, "name collides with prototype member",
        "\"JSON.stringify(XML.parse('<r><constructor>c</constructor><toString>t</toString></r>'))\"",
        "{\"r\":{\"constructor\":\"c\",\"toString\":\"t\"}}" },

    { 0, "__proto__ element kept as data",
        "\"JSON.stringify(XML.parse('<r><__proto__>x</__proto__></r>'))\"",
        "{\"r\":{\"__proto__\":\"x\"}}" },

    { 0, "prototype not polluted",
        "\"XML.parse('<r><__proto__><zz>1</zz></__proto__></r>'); String(({}).zz)\"",
        "undefined" },

    /* Undeclared entities are fatal to a conforming parser; real feeds are
     * full of them, and losing the whole document over one is the wrong
     * trade. Known names decode, unknown ones survive as literal text. */
    { 0, "html entity decodes",
        "\"XML.parse('<t>Fed&rsquo;s path&mdash;now</t>').t\"",
        "Fed\xe2\x80\x99s path\xe2\x80\x94now" },

    { 0, "unknown entity kept literal",
        "\"XML.parse('<t>a&foo;b</t>').t\"", "a&foo;b" },

    { 0, "bare ampersand kept literal",
        "\"XML.parse('<t>AT&T news</t>').t\"", "AT&T news" },

    { 0, "entity leniency reaches attributes",
        "\"XML.parse('<r t=\\\"A&nbsp;B&foo;\\\"/>').r['@_t']\"",
        "A\xc2\xa0" "B&foo;" },

    /* CDATA and comments are literal spans — & carries no meaning there and
     * must reach yxml exactly as written, so the screening skips them. */
    { 0, "cdata keeps entities raw",
        "\"XML.parse('<t><![CDATA[a&nbsp;b AT&T]]></t>').t\"", "a&nbsp;b AT&T" },

    { 0, "comment with entity ignored",
        "\"JSON.stringify(XML.parse('<r><!-- a&nbsp;b --><t>x</t></r>'))\"",
        "{\"r\":{\"t\":\"x\"}}" },

    /* yxml's five failure codes carry the only clue about what to change. */
    { 1, "error names the mismatch",
        "\"XML.parse('<a><b></c></a>')\"", "close tag does not match" },

    { 1, "error names truncation",
        "\"XML.parse('<a><b>')\"", "ended mid-element" },
};

static void test_xml_cases(void) {
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char args[2048];
        snprintf(args, sizeof(args), "{\"code\":%s}", cases[i].code);
        char *r = test_js_eval_run_json(args);
        int ok = r && (cases[i].is_error
                       ? strncmp(r, "error:", 6) == 0 && strstr(r, cases[i].want)
                       : strcmp(r, cases[i].want) == 0);
        if (!ok) printf("    row %zu (%s) got: %s\n", i, cases[i].name, r ? r : "NULL");
        assert(ok);
        free(r);
    }
}

int main(void) {
    TEST_INIT();
    printf("test_qjs_xml:\n");
    RUN(test_xml_cases);
    return 0;
}
