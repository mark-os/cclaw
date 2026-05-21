/* Generate mquickjs stdlib ROM table.
   Compile: cc -Ivendor/mquickjs gen_stdlib.c mquickjs_build.c cutils.c -lm -o gen_stdlib
   Run:     ./gen_stdlib -m64 > mquickjs_stdlib.c
            (or -m32 for 32-bit targets)
*/
#include "mquickjs_build.h"

/* --- Math object --- */
static const JSPropDef js_math_funcs[] = {
    JS_CFUNC_MAGIC_DEF("min", 2, js_math_min_max, 0),
    JS_CFUNC_MAGIC_DEF("max", 2, js_math_min_max, 1),
    JS_CFUNC_SPECIAL_DEF("abs", 1, f_f, js_fabs),
    JS_CFUNC_SPECIAL_DEF("floor", 1, f_f, js_floor),
    JS_CFUNC_SPECIAL_DEF("ceil", 1, f_f, js_ceil),
    JS_CFUNC_SPECIAL_DEF("round", 1, f_f, js_round_inf),
    JS_CFUNC_SPECIAL_DEF("sqrt", 1, f_f, js_sqrt),
    JS_CFUNC_SPECIAL_DEF("log", 1, f_f, js_log),
    JS_CFUNC_SPECIAL_DEF("exp", 1, f_f, js_exp),
    JS_CFUNC_SPECIAL_DEF("sin", 1, f_f, js_sin),
    JS_CFUNC_SPECIAL_DEF("cos", 1, f_f, js_cos),
    JS_CFUNC_SPECIAL_DEF("tan", 1, f_f, js_tan),
    JS_CFUNC_SPECIAL_DEF("trunc", 1, f_f, js_trunc),
    JS_CFUNC_SPECIAL_DEF("sign", 1, f_f, js_math_sign),
    JS_CFUNC_SPECIAL_DEF("fround", 1, f_f, js_math_fround),
    JS_CFUNC_DEF("imul", 2, js_math_imul),
    JS_CFUNC_DEF("clz32", 1, js_math_clz32),
    JS_CFUNC_DEF("atan2", 2, js_math_atan2),
    JS_CFUNC_DEF("pow", 2, js_math_pow),
    JS_CFUNC_DEF("random", 0, js_math_random),
    JS_PROP_DOUBLE_DEF("PI", 3.141592653589793, 0),
    JS_PROP_DOUBLE_DEF("E", 2.718281828459045, 0),
    JS_PROP_DOUBLE_DEF("LN2", 0.6931471805599453, 0),
    JS_PROP_DOUBLE_DEF("LN10", 2.302585092994046, 0),
    JS_PROP_DOUBLE_DEF("SQRT2", 1.4142135623730951, 0),
    JS_PROP_END,
};

/* --- JSON object --- */
static const JSPropDef js_json_funcs[] = {
    JS_CFUNC_DEF("parse", 1, js_json_parse),
    JS_CFUNC_DEF("stringify", 1, js_json_stringify),
    JS_PROP_END,
};

/* --- Number class --- */
static const JSPropDef js_number_proto_funcs[] = {
    JS_CFUNC_DEF("toString", 0, js_number_toString),
    JS_CFUNC_DEF("toFixed", 1, js_number_toFixed),
    JS_CFUNC_DEF("toExponential", 1, js_number_toExponential),
    JS_CFUNC_DEF("toPrecision", 1, js_number_toPrecision),
    JS_PROP_END,
};

static const JSClassDef js_number_class = JS_CLASS_DEF(
    "Number", 1, js_number_constructor, JS_CLASS_NUMBER,
    NULL, js_number_proto_funcs, NULL, NULL
);

/* --- Boolean class --- */
static const JSClassDef js_boolean_class = JS_CLASS_DEF(
    "Boolean", 1, js_boolean_constructor, JS_CLASS_BOOLEAN,
    NULL, NULL, NULL, NULL
);

/* --- String class --- */
static const JSPropDef js_string_class_funcs[] = {
    JS_CFUNC_MAGIC_DEF("fromCharCode", 1, js_string_fromCharCode, 0),
    JS_PROP_END,
};

static const JSPropDef js_string_proto_funcs[] = {
    JS_CGETSET_DEF("length", js_string_get_length, js_string_set_length),
    JS_CFUNC_DEF("slice", 2, js_string_slice),
    JS_CFUNC_DEF("substring", 2, js_string_substring),
    JS_CFUNC_MAGIC_DEF("charAt", 1, js_string_charAt, 1),
    JS_CFUNC_MAGIC_DEF("charCodeAt", 1, js_string_charAt, 2),
    JS_CFUNC_DEF("concat", 1, js_string_concat),
    JS_CFUNC_MAGIC_DEF("indexOf", 1, js_string_indexOf, 0),
    JS_CFUNC_MAGIC_DEF("lastIndexOf", 1, js_string_indexOf, 1),
    JS_CFUNC_DEF("match", 1, js_string_match),
    JS_CFUNC_MAGIC_DEF("replace", 2, js_string_replace, 0),
    JS_CFUNC_MAGIC_DEF("replaceAll", 2, js_string_replace, 1),
    JS_CFUNC_DEF("search", 1, js_string_search),
    JS_CFUNC_DEF("split", 2, js_string_split),
    JS_CFUNC_MAGIC_DEF("toLowerCase", 0, js_string_toLowerCase, 1),
    JS_CFUNC_MAGIC_DEF("toUpperCase", 0, js_string_toLowerCase, 0),
    JS_CFUNC_MAGIC_DEF("trim", 0, js_string_trim, 3),
    JS_CFUNC_MAGIC_DEF("trimStart", 0, js_string_trim, 1),
    JS_CFUNC_MAGIC_DEF("trimEnd", 0, js_string_trim, 2),
    JS_CFUNC_DEF("toString", 0, js_string_toString),
    JS_CFUNC_DEF("repeat", 1, js_string_repeat),
    JS_PROP_END,
};

static const JSClassDef js_string_class = JS_CLASS_DEF(
    "String", 1, js_string_constructor, JS_CLASS_STRING,
    js_string_class_funcs, js_string_proto_funcs, NULL, NULL
);

/* --- Error class --- */
static const JSPropDef js_error_proto_funcs[] = {
    JS_CFUNC_DEF("toString", 0, js_error_toString),
    JS_CGETSET_MAGIC_DEF("message", js_error_get_message, NULL, 0),
    JS_PROP_END,
};

static const JSClassDef js_error_class = JS_CLASS_MAGIC_DEF(
    "Error", 1, js_error_constructor, JS_CLASS_ERROR,
    NULL, js_error_proto_funcs, NULL, NULL
);

/* --- Object class --- */
static const JSPropDef js_object_class_funcs[] = {
    JS_CFUNC_DEF("defineProperty", 3, js_object_defineProperty),
    JS_CFUNC_DEF("getPrototypeOf", 1, js_object_getPrototypeOf),
    JS_CFUNC_DEF("setPrototypeOf", 2, js_object_setPrototypeOf),
    JS_CFUNC_DEF("create", 2, js_object_create),
    JS_CFUNC_DEF("keys", 1, js_object_keys),
    JS_PROP_END,
};

static const JSPropDef js_object_proto_funcs[] = {
    JS_CFUNC_DEF("hasOwnProperty", 1, js_object_hasOwnProperty),
    JS_CFUNC_DEF("toString", 0, js_object_toString),
    JS_PROP_END,
};

static const JSClassDef js_object_class = JS_CLASS_DEF(
    "Object", 1, js_object_constructor, JS_CLASS_OBJECT,
    js_object_class_funcs, js_object_proto_funcs, NULL, NULL
);

/* --- Function class --- */
static const JSPropDef js_function_proto_funcs[] = {
    JS_CGETSET_DEF("prototype", js_function_get_prototype, js_function_set_prototype),
    JS_CGETSET_MAGIC_DEF("length", js_function_get_length_name, NULL, 0),
    JS_CGETSET_MAGIC_DEF("name", js_function_get_length_name, NULL, 1),
    JS_CFUNC_DEF("toString", 0, js_function_toString),
    JS_CFUNC_DEF("call", 1, js_function_call),
    JS_CFUNC_DEF("apply", 2, js_function_apply),
    JS_CFUNC_DEF("bind", 1, js_function_bind),
    JS_PROP_END,
};

static const JSClassDef js_function_class = JS_CLASS_DEF(
    "Function", 1, js_function_constructor, JS_CLASS_CLOSURE,
    NULL, js_function_proto_funcs, NULL, NULL
);

/* --- Array class --- */
static const JSPropDef js_array_class_funcs[] = {
    JS_CFUNC_DEF("isArray", 1, js_array_isArray),
    JS_PROP_END,
};

static const JSPropDef js_array_proto_funcs[] = {
    JS_CGETSET_DEF("length", js_array_get_length, js_array_set_length),
    JS_CFUNC_MAGIC_DEF("push", 1, js_array_push, 0),
    JS_CFUNC_DEF("pop", 0, js_array_pop),
    JS_CFUNC_DEF("shift", 0, js_array_shift),
    JS_CFUNC_MAGIC_DEF("unshift", 1, js_array_push, 1),
    JS_CFUNC_DEF("join", 1, js_array_join),
    JS_CFUNC_DEF("toString", 0, js_array_toString),
    JS_CFUNC_DEF("reverse", 0, js_array_reverse),
    JS_CFUNC_DEF("concat", 1, js_array_concat),
    JS_CFUNC_MAGIC_DEF("indexOf", 1, js_array_indexOf, 0),
    JS_CFUNC_MAGIC_DEF("lastIndexOf", 1, js_array_indexOf, 1),
    JS_CFUNC_DEF("slice", 2, js_array_slice),
    JS_CFUNC_DEF("splice", 2, js_array_splice),
    JS_CFUNC_DEF("sort", 1, js_array_sort),
    JS_CFUNC_MAGIC_DEF("every", 1, js_array_every, 0),
    JS_CFUNC_MAGIC_DEF("some", 1, js_array_every, 1),
    JS_CFUNC_MAGIC_DEF("forEach", 1, js_array_every, 2),
    JS_CFUNC_MAGIC_DEF("map", 1, js_array_every, 3),
    JS_CFUNC_MAGIC_DEF("filter", 1, js_array_every, 4),
    JS_CFUNC_MAGIC_DEF("reduce", 1, js_array_reduce, 0),
    JS_CFUNC_MAGIC_DEF("reduceRight", 1, js_array_reduce, 1),
    JS_PROP_END,
};

static const JSClassDef js_array_class = JS_CLASS_DEF(
    "Array", 1, js_array_constructor, JS_CLASS_ARRAY,
    js_array_class_funcs, js_array_proto_funcs, NULL, NULL
);

/* --- RegExp class --- */
static const JSPropDef js_regexp_proto_funcs[] = {
    JS_CGETSET_DEF("lastIndex", js_regexp_get_lastIndex, js_regexp_set_lastIndex),
    JS_PROP_END,
};

static const JSClassDef js_regexp_class = JS_CLASS_DEF(
    "RegExp", 2, js_regexp_constructor, JS_CLASS_REGEXP,
    NULL, js_regexp_proto_funcs, NULL, NULL
);

/* --- Date class --- */
static const JSClassDef js_date_class = JS_CLASS_DEF(
    "Date", 7, js_date_constructor, JS_CLASS_DATE,
    NULL, NULL, NULL, NULL
);

/* --- ArrayBuffer class --- */
static const JSPropDef js_arraybuffer_proto_funcs[] = {
    JS_CGETSET_DEF("byteLength", js_array_buffer_get_byteLength, NULL),
    JS_PROP_END,
};

static const JSClassDef js_arraybuffer_class = JS_CLASS_DEF(
    "ArrayBuffer", 1, js_array_buffer_constructor, JS_CLASS_ARRAY_BUFFER,
    NULL, js_arraybuffer_proto_funcs, NULL, NULL
);

/* --- Namespace objects (no constructor) --- */
static const JSClassDef js_math_obj = JS_OBJECT_DEF("Math", js_math_funcs);
static const JSClassDef js_json_obj = JS_OBJECT_DEF("JSON", js_json_funcs);

/* --- Global object --- */
static const JSPropDef js_global_obj[] = {
    JS_CFUNC_DEF("eval", 1, js_global_eval),
    JS_CFUNC_DEF("isNaN", 1, js_global_isNaN),
    JS_CFUNC_DEF("isFinite", 1, js_global_isFinite),
    JS_CFUNC_DEF("parseInt", 2, js_number_parseInt),
    JS_CFUNC_DEF("parseFloat", 1, js_number_parseFloat),
    JS_PROP_DOUBLE_DEF("NaN", __builtin_nan(""), 0),
    JS_PROP_DOUBLE_DEF("Infinity", __builtin_inf(), 0),
    JS_PROP_UNDEFINED_DEF("undefined", 0),
    JS_PROP_NULL_DEF("null", 0),
    JS_PROP_CLASS_DEF("Math", &js_math_obj),
    JS_PROP_CLASS_DEF("JSON", &js_json_obj),
    JS_PROP_CLASS_DEF("Object", &js_object_class),
    JS_PROP_CLASS_DEF("Function", &js_function_class),
    JS_PROP_CLASS_DEF("Number", &js_number_class),
    JS_PROP_CLASS_DEF("Boolean", &js_boolean_class),
    JS_PROP_CLASS_DEF("String", &js_string_class),
    JS_PROP_CLASS_DEF("Error", &js_error_class),
    JS_PROP_CLASS_DEF("Array", &js_array_class),
    JS_PROP_CLASS_DEF("RegExp", &js_regexp_class),
    JS_PROP_CLASS_DEF("Date", &js_date_class),
    JS_PROP_CLASS_DEF("ArrayBuffer", &js_arraybuffer_class),
    JS_PROP_END,
};

int main(int argc, char **argv) {
    return build_atoms("js_std_library", js_global_obj, NULL, argc, argv);
}
