/*
 * Shared base-class definitions for mquickjs stdlib generators.
 * Included by gen_stdlib_main.c, gen_stdlib_channel.c, etc.
 *
 * Copyright (c) 2017-2025 Fabrice Bellard
 * Copyright (c) 2017-2025 Charlie Gordon
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef CCLAW_MQJS_OBJECTS_H
#define CCLAW_MQJS_OBJECTS_H

#include "mquickjs_build.h"
#include <math.h>

static const JSPropDef js_object_proto[] = {
    JS_CFUNC_DEF("hasOwnProperty", 1, js_object_hasOwnProperty),
    JS_CFUNC_DEF("toString", 0, js_object_toString),
    JS_PROP_END,
};

static const JSPropDef js_object[] = {
    JS_CFUNC_DEF("defineProperty", 3, js_object_defineProperty),
    JS_CFUNC_DEF("getPrototypeOf", 1, js_object_getPrototypeOf),
    JS_CFUNC_DEF("setPrototypeOf", 2, js_object_setPrototypeOf),
    JS_CFUNC_DEF("create", 2, js_object_create),
    JS_CFUNC_DEF("keys", 1, js_object_keys),
    JS_PROP_END,
};

static const JSClassDef js_object_class =
    JS_CLASS_DEF("Object", 1, js_object_constructor, JS_CLASS_OBJECT,
                 js_object, js_object_proto, NULL, NULL);

static const JSPropDef js_function_proto[] = {
    JS_CGETSET_DEF("prototype", js_function_get_prototype, js_function_set_prototype ),
    JS_CFUNC_DEF("call", 1, js_function_call ),
    JS_CFUNC_DEF("apply", 2, js_function_apply ),
    JS_CFUNC_DEF("bind", 1, js_function_bind ),
    JS_CFUNC_DEF("toString", 0, js_function_toString ),
    JS_CGETSET_MAGIC_DEF("length", js_function_get_length_name, NULL, 0 ),
    JS_CGETSET_MAGIC_DEF("name", js_function_get_length_name, NULL, 1 ),
    JS_PROP_END,
};

static const JSClassDef js_function_class =
    JS_CLASS_DEF("Function", 1, js_function_constructor, JS_CLASS_CLOSURE, NULL, js_function_proto, NULL, NULL);

static const JSPropDef js_number_proto[] = {
    JS_CFUNC_DEF("toExponential", 1, js_number_toExponential ),
    JS_CFUNC_DEF("toFixed", 1, js_number_toFixed ),
    JS_CFUNC_DEF("toPrecision", 1, js_number_toPrecision ),
    JS_CFUNC_DEF("toString", 1, js_number_toString ),
    JS_PROP_END,
};

static const JSPropDef js_number[] = {
    JS_CFUNC_DEF("parseInt", 2, js_number_parseInt ),
    JS_CFUNC_DEF("parseFloat", 1, js_number_parseFloat ),
    JS_PROP_DOUBLE_DEF("MAX_VALUE", 1.7976931348623157e+308, 0 ),
    JS_PROP_DOUBLE_DEF("MIN_VALUE", 5e-324, 0 ),
    JS_PROP_DOUBLE_DEF("NaN", NAN, 0 ),
    JS_PROP_DOUBLE_DEF("NEGATIVE_INFINITY", -INFINITY, 0 ),
    JS_PROP_DOUBLE_DEF("POSITIVE_INFINITY", INFINITY, 0 ),
    JS_PROP_DOUBLE_DEF("EPSILON", 2.220446049250313e-16, 0 ),
    JS_PROP_DOUBLE_DEF("MAX_SAFE_INTEGER", 9007199254740991.0, 0 ),
    JS_PROP_DOUBLE_DEF("MIN_SAFE_INTEGER", -9007199254740991.0, 0 ),
    JS_PROP_END,
};

static const JSClassDef js_number_class =
    JS_CLASS_DEF("Number", 1, js_number_constructor, JS_CLASS_NUMBER, js_number, js_number_proto, NULL, NULL);

static const JSClassDef js_boolean_class =
    JS_CLASS_DEF("Boolean", 1, js_boolean_constructor, JS_CLASS_BOOLEAN, NULL, NULL, NULL, NULL);

static const JSPropDef js_string_proto[] = {
    JS_CGETSET_DEF("length", js_string_get_length, js_string_set_length ),
    JS_CFUNC_MAGIC_DEF("charAt", 1, js_string_charAt, magic_charAt ),
    JS_CFUNC_MAGIC_DEF("charCodeAt", 1, js_string_charAt, magic_charCodeAt ),
    JS_CFUNC_MAGIC_DEF("codePointAt", 1, js_string_charAt, magic_codePointAt ),
    JS_CFUNC_DEF("slice", 2, js_string_slice ),
    JS_CFUNC_DEF("substring", 2, js_string_substring ),
    JS_CFUNC_DEF("concat", 1, js_string_concat ),
    JS_CFUNC_MAGIC_DEF("indexOf", 1, js_string_indexOf, 0 ),
    JS_CFUNC_MAGIC_DEF("lastIndexOf", 1, js_string_indexOf, 1 ),
    JS_CFUNC_DEF("match", 1, js_string_match ),
    JS_CFUNC_MAGIC_DEF("replace", 2, js_string_replace, 0 ),
    JS_CFUNC_MAGIC_DEF("replaceAll", 2, js_string_replace, 1 ),
    JS_CFUNC_DEF("search", 1, js_string_search ),
    JS_CFUNC_DEF("split", 2, js_string_split ),
    JS_CFUNC_MAGIC_DEF("toLowerCase", 0, js_string_toLowerCase, 1 ),
    JS_CFUNC_MAGIC_DEF("toUpperCase", 0, js_string_toLowerCase, 0 ),
    JS_CFUNC_MAGIC_DEF("trim", 0, js_string_trim, 3 ),
    JS_CFUNC_MAGIC_DEF("trimEnd", 0, js_string_trim, 2 ),
    JS_CFUNC_MAGIC_DEF("trimStart", 0, js_string_trim, 1 ),
    JS_CFUNC_DEF("toString", 0, js_string_toString ),
    JS_CFUNC_DEF("repeat", 1, js_string_repeat ),
    JS_PROP_END,
};

static const JSPropDef js_string[] = {
    JS_CFUNC_MAGIC_DEF("fromCharCode", 1, js_string_fromCharCode, 0 ),
    JS_CFUNC_MAGIC_DEF("fromCodePoint", 1, js_string_fromCharCode, 1 ),
    JS_PROP_END,
};

static const JSClassDef js_string_class =
    JS_CLASS_DEF("String", 1, js_string_constructor, JS_CLASS_STRING, js_string, js_string_proto, NULL, NULL);

static const JSPropDef js_array_proto[] = {
    JS_CFUNC_DEF("concat", 1, js_array_concat ),
    JS_CGETSET_DEF("length", js_array_get_length, js_array_set_length ),
    JS_CFUNC_MAGIC_DEF("push", 1, js_array_push, 0 ),
    JS_CFUNC_DEF("pop", 0, js_array_pop ),
    JS_CFUNC_DEF("join", 1, js_array_join ),
    JS_CFUNC_DEF("toString", 0, js_array_toString ),
    JS_CFUNC_DEF("reverse", 0, js_array_reverse ),
    JS_CFUNC_DEF("shift", 0, js_array_shift ),
    JS_CFUNC_DEF("slice", 2, js_array_slice ),
    JS_CFUNC_DEF("splice", 2, js_array_splice ),
    JS_CFUNC_MAGIC_DEF("unshift", 1, js_array_push, 1 ),
    JS_CFUNC_MAGIC_DEF("indexOf", 1, js_array_indexOf, 0 ),
    JS_CFUNC_MAGIC_DEF("lastIndexOf", 1, js_array_indexOf, 1 ),
    JS_CFUNC_MAGIC_DEF("every", 1, js_array_every, js_special_every ),
    JS_CFUNC_MAGIC_DEF("some", 1, js_array_every, js_special_some ),
    JS_CFUNC_MAGIC_DEF("forEach", 1, js_array_every, js_special_forEach ),
    JS_CFUNC_MAGIC_DEF("map", 1, js_array_every, js_special_map ),
    JS_CFUNC_MAGIC_DEF("filter", 1, js_array_every, js_special_filter ),
    JS_CFUNC_MAGIC_DEF("reduce", 1, js_array_reduce, js_special_reduce ),
    JS_CFUNC_MAGIC_DEF("reduceRight", 1, js_array_reduce, js_special_reduceRight ),
    JS_CFUNC_MAGIC_DEF("reduce", 1, js_array_reduce, js_special_reduce ),
    JS_CFUNC_DEF("sort", 1, js_array_sort ),
    JS_PROP_END,
};

static const JSPropDef js_array[] = {
    JS_CFUNC_DEF("isArray", 1, js_array_isArray ),
    JS_PROP_END,
};

static const JSClassDef js_array_class =
    JS_CLASS_DEF("Array", 1, js_array_constructor, JS_CLASS_ARRAY, js_array, js_array_proto, NULL, NULL);

static const JSPropDef js_error_proto[] = {
    JS_CFUNC_DEF("toString", 0, js_error_toString ),
    JS_PROP_STRING_DEF("name", "Error", 0 ),
    JS_CGETSET_MAGIC_DEF("message", js_error_get_message, NULL, 0 ),
    JS_CGETSET_MAGIC_DEF("stack", js_error_get_message, NULL, 1 ),
    JS_PROP_END,
};

static const JSClassDef js_error_class =
    JS_CLASS_MAGIC_DEF("Error", 1, js_error_constructor, JS_CLASS_ERROR, NULL, js_error_proto, NULL, NULL);

#define ERROR_DEF(cname, name, class_id)                       \
    static const JSPropDef js_ ## cname ## _proto[] = { \
        JS_PROP_STRING_DEF("name", name, 0 ),                  \
        JS_PROP_END,                                         \
    };                                                                 \
    static const JSClassDef js_ ## cname ## _class =                    \
        JS_CLASS_MAGIC_DEF(name, 1, js_error_constructor, class_id, NULL, js_ ## cname ## _proto, &js_error_class, NULL);

ERROR_DEF(eval_error, "EvalError", JS_CLASS_EVAL_ERROR)
ERROR_DEF(range_error, "RangeError", JS_CLASS_RANGE_ERROR)
ERROR_DEF(reference_error, "ReferenceError", JS_CLASS_REFERENCE_ERROR)
ERROR_DEF(syntax_error, "SyntaxError", JS_CLASS_SYNTAX_ERROR)
ERROR_DEF(type_error, "TypeError", JS_CLASS_TYPE_ERROR)
ERROR_DEF(uri_error, "URIError", JS_CLASS_URI_ERROR)
ERROR_DEF(internal_error, "InternalError", JS_CLASS_INTERNAL_ERROR)

static const JSPropDef js_math[] = {
    JS_CFUNC_MAGIC_DEF("min", 2, js_math_min_max, 0 ),
    JS_CFUNC_MAGIC_DEF("max", 2, js_math_min_max, 1 ),
    JS_CFUNC_SPECIAL_DEF("sign", 1, f_f, js_math_sign ),
    JS_CFUNC_SPECIAL_DEF("abs", 1, f_f, js_fabs ),
    JS_CFUNC_SPECIAL_DEF("floor", 1, f_f, js_floor ),
    JS_CFUNC_SPECIAL_DEF("ceil", 1, f_f, js_ceil ),
    JS_CFUNC_SPECIAL_DEF("round", 1, f_f, js_round_inf ),
    JS_CFUNC_SPECIAL_DEF("sqrt", 1, f_f, js_sqrt ),

    JS_PROP_DOUBLE_DEF("E", 2.718281828459045, 0 ),
    JS_PROP_DOUBLE_DEF("LN10", 2.302585092994046, 0 ),
    JS_PROP_DOUBLE_DEF("LN2", 0.6931471805599453, 0 ),
    JS_PROP_DOUBLE_DEF("LOG2E", 1.4426950408889634, 0 ),
    JS_PROP_DOUBLE_DEF("LOG10E", 0.4342944819032518, 0 ),
    JS_PROP_DOUBLE_DEF("PI", 3.141592653589793, 0 ),
    JS_PROP_DOUBLE_DEF("SQRT1_2", 0.7071067811865476, 0 ),
    JS_PROP_DOUBLE_DEF("SQRT2", 1.4142135623730951, 0 ),

    JS_CFUNC_SPECIAL_DEF("sin", 1, f_f, js_sin ),
    JS_CFUNC_SPECIAL_DEF("cos", 1, f_f, js_cos ),
    JS_CFUNC_SPECIAL_DEF("tan", 1, f_f, js_tan ),
    JS_CFUNC_SPECIAL_DEF("asin", 1, f_f, js_asin ),
    JS_CFUNC_SPECIAL_DEF("acos", 1, f_f, js_acos ),
    JS_CFUNC_SPECIAL_DEF("atan", 1, f_f, js_atan ),
    JS_CFUNC_DEF("atan2", 2, js_math_atan2 ),
    JS_CFUNC_SPECIAL_DEF("exp", 1, f_f, js_exp ),
    JS_CFUNC_SPECIAL_DEF("log", 1, f_f, js_log ),
    JS_CFUNC_DEF("pow", 2, js_math_pow ),
    JS_CFUNC_DEF("random", 0, js_math_random ),

    /* some ES6 functions */
    JS_CFUNC_DEF("imul", 2, js_math_imul ),
    JS_CFUNC_DEF("clz32", 1, js_math_clz32 ),
    JS_CFUNC_SPECIAL_DEF("fround", 1, f_f, js_math_fround ),
    JS_CFUNC_SPECIAL_DEF("trunc", 1, f_f, js_trunc ),
    JS_CFUNC_SPECIAL_DEF("log2", 1, f_f, js_log2 ),
    JS_CFUNC_SPECIAL_DEF("log10", 1, f_f, js_log10 ),
    
    JS_PROP_END,
};

static const JSClassDef js_math_obj =
    JS_OBJECT_DEF("Math", js_math);

static const JSPropDef js_json[] = {
    JS_CFUNC_DEF("parse", 2, js_json_parse ),
    JS_CFUNC_DEF("stringify", 3, js_json_stringify ),
    JS_PROP_END,
};

static const JSClassDef js_json_obj =
    JS_OBJECT_DEF("JSON", js_json);

/* typed arrays */
static const JSPropDef js_array_buffer_proto[] = {
    JS_CGETSET_DEF("byteLength", js_array_buffer_get_byteLength, NULL ),
    JS_PROP_END,
};

static const JSClassDef js_array_buffer_class =
    JS_CLASS_DEF("ArrayBuffer", 1, js_array_buffer_constructor, JS_CLASS_ARRAY_BUFFER, NULL, js_array_buffer_proto, NULL, NULL);

static const JSPropDef js_typed_array_base_proto[] = {
    JS_CGETSET_MAGIC_DEF("length", js_typed_array_get_length, NULL, 0 ),
    JS_CGETSET_MAGIC_DEF("byteLength", js_typed_array_get_length, NULL, 1 ),
    JS_CGETSET_MAGIC_DEF("byteOffset", js_typed_array_get_length, NULL, 2 ),
    JS_CGETSET_MAGIC_DEF("buffer", js_typed_array_get_length, NULL, 3 ),
    JS_CFUNC_DEF("join", 1, js_array_join ),
    JS_CFUNC_DEF("toString", 0, js_array_toString ),
    JS_CFUNC_DEF("subarray", 2, js_typed_array_subarray ),
    JS_CFUNC_DEF("set", 1, js_typed_array_set ),
    JS_PROP_END,
};

static const JSClassDef js_typed_array_base_class =
    JS_CLASS_DEF("TypedArray", 0, js_typed_array_base_constructor, JS_CLASS_TYPED_ARRAY, NULL, js_typed_array_base_proto, NULL, NULL);

#define TA_DEF(name, class_name, bpe)\
static const JSPropDef js_ ## name [] = {\
    JS_PROP_DOUBLE_DEF("BYTES_PER_ELEMENT", bpe, 0),\
    JS_PROP_END,\
};\
static const JSPropDef js_ ## name ## _proto[] = {\
    JS_PROP_DOUBLE_DEF("BYTES_PER_ELEMENT", bpe, 0),\
    JS_PROP_END,\
};\
static const JSClassDef js_ ## name ## _class =\
    JS_CLASS_MAGIC_DEF(#name, 3, js_typed_array_constructor, class_name, js_ ## name, js_ ## name ## _proto, &js_typed_array_base_class, NULL);

TA_DEF(Uint8ClampedArray, JS_CLASS_UINT8C_ARRAY, 1)
TA_DEF(Int8Array, JS_CLASS_INT8_ARRAY, 1)
TA_DEF(Uint8Array, JS_CLASS_UINT8_ARRAY, 1)
TA_DEF(Int16Array, JS_CLASS_INT16_ARRAY, 2)
TA_DEF(Uint16Array, JS_CLASS_UINT16_ARRAY, 2)
TA_DEF(Int32Array, JS_CLASS_INT32_ARRAY, 4)
TA_DEF(Uint32Array, JS_CLASS_UINT32_ARRAY, 4)
TA_DEF(Float32Array, JS_CLASS_FLOAT32_ARRAY, 4)
TA_DEF(Float64Array, JS_CLASS_FLOAT64_ARRAY, 8)

/* regexp */

static const JSPropDef js_regexp_proto[] = {
    JS_CGETSET_DEF("lastIndex", js_regexp_get_lastIndex, js_regexp_set_lastIndex ),
    JS_CGETSET_DEF("source", js_regexp_get_source, NULL ),
    JS_CGETSET_DEF("flags", js_regexp_get_flags, NULL ),
    JS_CFUNC_MAGIC_DEF("exec", 1, js_regexp_exec, 0 ),
    JS_CFUNC_MAGIC_DEF("test", 1, js_regexp_exec, 1 ),
    JS_PROP_END,
};

static const JSClassDef js_regexp_class =
    JS_CLASS_DEF("RegExp", 2, js_regexp_constructor, JS_CLASS_REGEXP, NULL, js_regexp_proto, NULL, NULL);

/* fs object — eval profile */

static const JSPropDef js_fs[] = {
    JS_CFUNC_DEF("readFile", 1, js_fs_readFile),
    JS_CFUNC_DEF("writeFile", 2, js_fs_writeFile),
    JS_CFUNC_DEF("readDir", 1, js_fs_readDir),
    JS_CFUNC_DEF("stat", 1, js_fs_stat),
    JS_CFUNC_DEF("cwd", 0, js_fs_cwd),
    JS_PROP_END,
};

static const JSClassDef js_fs_obj =
    JS_OBJECT_DEF("fs", js_fs);

/* other objects */

static const JSPropDef js_date[] = {
    JS_CFUNC_DEF("now", 0, js_date_now),
    JS_PROP_END,
};

static const JSClassDef js_date_class =
    JS_CLASS_DEF("Date", 7, js_date_constructor, JS_CLASS_DATE, js_date, NULL, NULL, NULL);

/* channel profile — baked native objects.
 * JS_CFUNC_DEF stringifies the function name (see mquickjs_build.h), so the
 * generator needs no C declaration of these host functions — they are defined
 * in mqjs_host_channel.c, which is compiled into the channel stdlib TU. */

static const JSPropDef js_channel_admin[] = {
    JS_CFUNC_DEF("setKey", 2, js_admin_set_key),
    JS_CFUNC_DEF("setModel", 2, js_admin_set_model),
    JS_CFUNC_DEF("setEndpoint", 2, js_admin_set_endpoint),
    JS_CFUNC_DEF("addHost", 2, js_admin_add_host),
    JS_CFUNC_DEF("removeHost", 2, js_admin_remove_host),
    JS_CFUNC_DEF("listProviders", 0, js_admin_list_providers),
    JS_CFUNC_DEF("listAgents", 0, js_admin_list_agents),
    JS_CFUNC_DEF("isAdmin", 1, js_admin_is_admin),
    JS_PROP_END,
};

static const JSClassDef js_channel_admin_obj =
    JS_OBJECT_DEF("admin", js_channel_admin);

static const JSPropDef js_channel[] = {
    JS_CFUNC_DEF("emit", 2, js_ch_emit),
    JS_CFUNC_DEF("send", 1, js_ch_send),
    JS_CFUNC_DEF("getConfig", 1, js_ch_get_config),
    JS_CFUNC_DEF("setConfig", 2, js_ch_set_config),
    JS_CFUNC_DEF("ackOutbox", 1, js_ch_ack_outbox),
    JS_CFUNC_DEF("failOutbox", 2, js_ch_fail_outbox),
    JS_CFUNC_DEF("log", 1, js_ch_log),
    JS_PROP_CLASS_DEF("admin", &js_channel_admin_obj),
    JS_PROP_END,
};

static const JSClassDef js_channel_obj =
    JS_OBJECT_DEF("channel", js_channel);

#endif /* CCLAW_MQJS_OBJECTS_H */
