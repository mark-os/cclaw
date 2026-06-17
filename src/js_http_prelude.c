#include <stddef.h>

/* Shared HTTP shim eval'd into every JS profile: http_request is the one HTTP
 * call (synchronous, forgiving response object); fetch throws with a redirect
 * so the Promise name can't be used silently. */
const char JS_HTTP_PRELUDE[] =
    "var __http_native = http_request;\n"
    "function __http_wrap(r) {\n"
    "  if (r && typeof r === 'object') {\n"
    "    r.ok = r.status >= 200 && r.status < 300;\n"
    "    r.text = r.body;\n"
    "    r.responseText = r.body;\n"
    "    r.response = r.body;\n"
    "    r.json = function () { return JSON.parse(this.body); };\n"
    "    r.then = function () { throw new TypeError('response is synchronous — it is already here; use r.body / r.json(), not .then()/await'); };\n"
    "  }\n"
    "  return r;\n"
    "}\n"
    "var http_request = function (url, opts) { return __http_wrap(__http_native(url, opts)); };\n"
    "var fetch = function () { throw new TypeError('fetch() is not available. Use http_request(url[, opts]) — SYNCHRONOUS, returns {status, ok, body, json()}. No Promise/await/.then(); parse with r.json() or JSON.parse(r.body).'); };\n";
