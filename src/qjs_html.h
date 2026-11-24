#ifndef CCLAW_QJS_HTML_H
#define CCLAW_QJS_HTML_H

#include "qjs_helpers.h"
#include <stddef.h>

/* Convert HTML to markdown (script/style dropped, headings/lists/links to
 * markers, entities decoded). Returns a malloc'd string, caller frees.
 * Non-HTML text passes through mostly unchanged. It parses untrusted bytes,
 * so it is linked only into the JS engine's host functions, which run in the
 * sandboxed tool child — never called from the trusted parent. */
char *html_to_markdown(const char *html, size_t html_len);

/* Registers the `HTML` global: HTML.toMarkdown(str) → markdown string. */
void qjs_register_html(JSContext *ctx);

#endif
