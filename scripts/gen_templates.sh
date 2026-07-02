#!/bin/sh
# Generate build/templates.h (extern declarations) and build/templates.c
# (definitions) from templates/* files.
# Each file becomes a const char[] (null-terminated C string).
# Variable name: foo_bar.md -> TPL_FOO_BAR_MD

HFILE="$1"
TMPL_DIR="$2"
CFILE="${HFILE%.h}.c"

# --- Header: extern declarations ---
{
printf '/* Auto-generated from templates/ — do not edit */\n'
printf '#ifndef CCLAW_TEMPLATES_H\n#define CCLAW_TEMPLATES_H\n\n'

for f in "$TMPL_DIR"/*; do
    [ -f "$f" ] || continue
    base=$(basename "$f")
    varname="TPL_$(printf '%s' "$base" | tr '[:lower:]' '[:upper:]' | tr '.-' '__')"
    printf 'extern const char %s[];\n' "$varname"
done

printf '\n#endif /* CCLAW_TEMPLATES_H */\n'
} > "$HFILE"

# --- C file: definitions ---
{
printf '/* Auto-generated from templates/ — do not edit */\n'
printf '#include "templates.h"\n\n'

for f in "$TMPL_DIR"/*; do
    [ -f "$f" ] || continue
    base=$(basename "$f")
    varname="TPL_$(printf '%s' "$base" | tr '[:lower:]' '[:upper:]' | tr '.-' '__')"
    printf 'const char %s[] =\n' "$varname"
    while IFS= read -r line; do
        escaped=$(printf '%s' "$line" | sed 's/\\/\\\\/g; s/"/\\"/g')
        printf '    "%s\\n"\n' "$escaped"
    done < "$f"
    printf ';\n\n'
done
} > "$CFILE"
