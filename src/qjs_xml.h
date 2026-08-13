#ifndef CCLAW_QJS_XML_H
#define CCLAW_QJS_XML_H

#include "qjs_helpers.h"

/* Registers the `XML` global (+ lowercase `xml` alias): XML.parse(str) →
 * fast-xml-parser-shaped object
 * (element names as keys, attributes as "@_name", repeated siblings as
 * arrays, text-only elements collapsed to strings). */
void qjs_register_xml(JSContext *ctx);

#endif
