#!/bin/sh
# Generate build/templates.h from templates/* files.
# Each file becomes a static const char[] (null-terminated C string).
# Variable name: foo_bar.md -> TPL_FOO_BAR_MD

OUTFILE="$1"
TMPL_DIR="$2"

{
printf '/* Auto-generated from templates/ — do not edit */\n'
printf '#ifndef CCLAW_TEMPLATES_H\n#define CCLAW_TEMPLATES_H\n\n'

for f in "$TMPL_DIR"/*; do
    [ -f "$f" ] || continue
    base=$(basename "$f")
    varname="TPL_$(printf '%s' "$base" | tr '[:lower:]' '[:upper:]' | tr '.-' '__')"
    printf 'static const char %s[] =\n' "$varname"
    while IFS= read -r line; do
        escaped=$(printf '%s' "$line" | sed 's/\\/\\\\/g; s/"/\\"/g')
        printf '    "%s\\n"\n' "$escaped"
    done < "$f"
    printf ';\n\n'
done

printf '#endif /* CCLAW_TEMPLATES_H */\n'
} > "$OUTFILE"
