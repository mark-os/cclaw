#ifndef CCLAW_TOOL_WEB_FETCH_H
#define CCLAW_TOOL_WEB_FETCH_H

#include "tools.h"

/* Register web_fetch tool into registry. Returns 0 on success. */
int tool_web_fetch_register(ToolRegistry *reg);

/* Handler: parse JSON args {"url":"..."}, HTTP GET, strip HTML, wrap in
 * external input protection boundary markers (V15). */
char *tool_web_fetch_handler(const char *arguments, void *user_data);

/* Strip HTML tags from src, write plain text to dst. Returns bytes written. */
size_t html_strip_tags(const char *src, char *dst, size_t dst_cap);

/* V15: sanitize boundary marker lookalikes (homoglyphs) in content. */
void sanitize_homoglyphs(char *text);

#endif
