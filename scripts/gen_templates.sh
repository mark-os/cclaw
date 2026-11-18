#!/bin/sh
# Generate build/templates.h (extern declarations) and build/templates.c
# (definitions) from templates/* files and from the extension bundles that
# ship in the binary.
# Each file becomes a const char[] (null-terminated C string).
# Variable name: templates/foo_bar.md        -> TPL_FOO_BAR_MD
#                extensions/compaction/x.qjs -> TPL_EXT_COMPACTION_X_QJS
# A bundle's README.md is documentation for the source tree, not shipped.
#
# Usage: gen_templates.sh <out.h> <templates dir> [bundle dir ...]

HFILE="$1"
TMPL_DIR="$2"
shift 2
CFILE="${HFILE%.h}.c"

upper() { printf '%s' "$1" | tr '[:lower:]' '[:upper:]' | tr '.-' '__'; }

# "<path> <varname>" per embedded file, templates first.
list() {
    for f in "$TMPL_DIR"/*; do
        [ -f "$f" ] || continue
        printf '%s TPL_%s\n' "$f" "$(upper "$(basename "$f")")"
    done
    for d in "$@"; do
        b=$(upper "$(basename "$d")")
        for f in "$d"/*; do
            [ -f "$f" ] || continue
            [ "$(basename "$f")" = README.md ] && continue
            printf '%s TPL_EXT_%s_%s\n' "$f" "$b" "$(upper "$(basename "$f")")"
        done
    done
}

# --- Header: extern declarations ---
{
printf '/* Auto-generated from templates/ and shipped extension bundles — do not edit */\n'
printf '#ifndef CCLAW_TEMPLATES_H\n#define CCLAW_TEMPLATES_H\n\n'
list "$@" | while read -r f varname; do
    printf 'extern const char %s[];\n' "$varname"
done
printf '\n#endif /* CCLAW_TEMPLATES_H */\n'
} > "$HFILE"

# --- C file: definitions ---
{
printf '/* Auto-generated from templates/ and shipped extension bundles — do not edit */\n'
printf '#include "templates.h"\n\n'
list "$@" | while read -r f varname; do
    printf 'const char %s[] =\n' "$varname"
    while IFS= read -r line; do
        escaped=$(printf '%s' "$line" | sed 's/\\/\\\\/g; s/"/\\"/g')
        printf '    "%s\\n"\n' "$escaped"
    done < "$f"
    printf ';\n\n'
done
} > "$CFILE"
