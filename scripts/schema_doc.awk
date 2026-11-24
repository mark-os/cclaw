# schema_doc.awk — the explanations in templates/schema.sql's comments, as
# JSON: [[table, column|null, explanation], ...]. Run at build time by
# gen_templates.sh (LC_ALL=C: lengths are bytes); the explorer serves it.
#
# The file's conventions: comment lines right above a CREATE TABLE explain
# the table (a "═══ Header ═══" line alone stands in when there are none);
# inside it a column's comment trails its line and continues on
# deeper-indented comment-only lines, while comment lines at the column
# indent explain the column that follows them.
#
# Three texts build up: T["t"] the table's, T["c"] the current column's,
# T["p"] comments waiting for the column below them. P[] is each one's
# indent of the previous line, -1 after a blank comment line.

BEGIN {
    for (i = 1; i < 128; i++) ASCII = ASCII sprintf("%c", i)
    for (i = 1; i < 32; i++) ESC[sprintf("%c", i)] = sprintf("\\u%04x", i)
    ESC["\b"] = "\\b"; ESC["\t"] = "\\t"; ESC["\n"] = "\\n"
    ESC["\f"] = "\\f"; ESC["\r"] = "\\r"; ESC["\""] = "\\\""; ESC["\\"] = "\\\\"
    split("PRIMARY FOREIGN UNIQUE CHECK CONSTRAINT", w)
    for (i in w) CON[w[i]] = 1
    P["t"] = P["c"] = P["p"] = 0
    base = -1; n = 0
}

# A JSON string, escaped the way SQLite's json_quote escapes it.
function q(s,   o, i, c) {
    o = ""
    for (i = 1; i <= length(s); i++) {
        c = substr(s, i, 1)
        o = o ((c in ESC) ? ESC[c] : c)
    }
    return "\"" o "\""
}

function add(col, doc) {
    OUT[++n] = "[" q(table) "," (col == "" ? "null" : q(col)) "," q(doc) "]"
}

# Wrapped prose joins with a space; indented lines are list items and start
# a new line; a blank comment line is a paragraph break.
function doc_line(k, s,   lead, cont) {
    if (substr(s, 1, 1) == " ") s = substr(s, 2)
    sub(/[ \r]+$/, "", s)
    match(s, /^ */)
    lead = RLENGTH
    if (lead == length(s)) { if (T[k] != "") P[k] = -1; return }
    cont = lead > 0 && P[k] > 0 && lead > P[k]
    if (T[k] != "") {
        if (P[k] < 0) T[k] = T[k] "\n\n"
        else if (cont) T[k] = T[k] " "
        else if (lead > 0 || P[k] > 0) T[k] = T[k] "\n"
        else T[k] = T[k] " "
    }
    T[k] = T[k] substr(s, lead + 1)
    if (!cont) P[k] = lead
}

function emit(col, k) {
    if (T[k] != "") add(col, T[k])
    T[k] = ""; P[k] = 0
}

function col_flush() {
    if (col != "") emit(col, "c")
    else T["c"] = ""
    col = ""
}

function ident(s) {
    return match(s, /^[_A-Za-z][_A-Za-z0-9]*/) ? substr(s, 1, RLENGTH > 63 ? 63 : RLENGTH) : ""
}

function hi(c) { return c != "" && index(ASCII, c) == 0 }

# Offset (0-based) of a trailing "--" outside quotes, or -1.
function comment_at(s,   i, c, inq) {
    inq = 0
    for (i = 1; i < length(s); i++) {
        c = substr(s, i, 1)
        if (c == "'") inq = !inq
        else if (!inq && c == "-" && substr(s, i + 1, 1) == "-") return i - 1
    }
    return -1
}

function comment(indent, s,   a) {
    if (!in_table) {
        if (index(s, "\342\225\220")) {          # ═ : a section header
            a = s
            while (substr(a, 1, 1) == " " || hi(substr(a, 1, 1))) a = substr(a, 2)
            while (a != "" && (substr(a, length(a)) ~ /[ \r]/ || hi(substr(a, length(a)))))
                a = substr(a, 1, length(a) - 1)
            header = substr(a, 1, 95)
            return
        }
        doc_line("t", s)
        return
    }
    if (base >= 0 && indent > base && col != "") doc_line("c", s)
    else doc_line("p", s)
}

function table_start(s,   name) {
    name = ident(substr(s, 28))
    if (name == "") return
    table = name; in_table = 1; base = -1; col = ""
    if (T["t"] != "") emit("", "t")
    else if (header != "") add("", header)
    header = ""
}

function body_line(indent, s,   w, c) {
    if (base < 0) base = indent
    if (indent == base) {
        col_flush()
        w = ident(s)
        if (w != "" && !(toupper(w) in CON)) {
            col = w
            T["c"] = T["p"]; P["c"] = P["p"]     # comments above it explain it
            T["p"] = ""; P["p"] = 0
        } else {
            T["p"] = ""; P["p"] = 0
        }
    }
    c = comment_at(s)
    if (c >= 0 && col != "") doc_line("c", substr(s, c + 3))
}

{
    match($0, /^ */)
    indent = RLENGTH
    s = substr($0, indent + 1)
    if (substr(s, 1, 2) == "--") { comment(indent, substr(s, 3)); next }
    if (!in_table) {
        if (length(s) > 27 && substr(s, 1, 27) == "CREATE TABLE IF NOT EXISTS ") table_start(s)
        else { T["t"] = ""; P["t"] = 0; header = "" }
        next
    }
    if (substr(s, 1, 1) == ")") { col_flush(); T["p"] = ""; in_table = 0; next }
    if (s != "") body_line(indent, s)
}

END {
    if (n == 0) { print "[]"; exit }
    o = "["
    for (i = 1; i <= n; i++) o = o (i > 1 ? "," : "") OUT[i]
    print o "]"
}
