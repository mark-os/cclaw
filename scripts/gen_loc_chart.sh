#!/bin/bash
# Generate an SVG chart of code / comment / blank line counts (src/, include/
# C sources) over commits. Unlike numstat deltas, every point is a true count:
# each commit's tree is materialized (git archive) and classified by a small
# C lexer in awk — line/block comments, string/char literals so "http://x"
# never reads as a comment. A line with any code counts as code, so the three
# series sum to the raw wc -l total.
set -eu
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"

outdir="docs"
mkdir -p "$outdir"

lexer=$(mktemp)
trap 'rm -f "$lexer"' EXIT
cat > "$lexer" <<'AWK'
{
    line = $0
    has_code = 0; has_comment = 0
    p = 1; n = length(line)
    while (p <= n) {
        if (inblock) {
            has_comment = 1
            e = index(substr(line, p), "*/")
            if (e == 0) break
            p += e + 1; inblock = 0
            continue
        }
        rest = substr(line, p)
        tl = index(rest, "//"); tb = index(rest, "/*")
        ts = index(rest, "\""); tc = index(rest, "'")
        m = 0
        if (tl) m = tl
        if (tb && (!m || tb < m)) m = tb
        if (ts && (!m || ts < m)) m = ts
        if (tc && (!m || tc < m)) m = tc
        if (!m) {
            if (rest ~ /[^ \t\r]/) has_code = 1
            break
        }
        if (m > 1 && substr(rest, 1, m - 1) ~ /[^ \t\r]/) has_code = 1
        if (m == tb) {              # block comment opens
            has_comment = 1; inblock = 1
            p += m + 1
        } else if (m == tl) {       # line comment eats the rest
            has_comment = 1
            break
        } else {                    # string or char literal: consume it
            has_code = 1
            q = substr(rest, m, 1)
            pos = p + m
            while (pos <= n) {
                c = substr(line, pos, 1)
                if (c == "\\") { pos += 2; continue }
                pos++
                if (c == q) break
            }
            p = pos
        }
    }
    if (has_code) code++
    else if (has_comment) comment++
    else blank++
}
END { printf "%d %d %d\n", code + 0, comment + 0, blank + 0 }
AWK

count_tree() { # <rev> — classify all .c/.h under src/ (+ include/ if present)
    local rev=$1 paths="src"
    git rev-parse -q --verify "$rev:include" >/dev/null 2>&1 && paths="src include"
    # shellcheck disable=SC2086
    git archive "$rev" $paths 2>/dev/null \
        | tar -xO --wildcards '*.c' '*.h' 2>/dev/null \
        | awk -f "$lexer"
}

{
    i=0
    while read -r hash; do
        i=$((i + 1))
        echo "$i $(count_tree "$hash")"
    done < <(git log --reverse --format=%H)
    # Final point: the working tree, so uncommitted changes show up
    echo "$((i + 1)) $(cat src/*.c src/*.h include/*.c include/*.h 2>/dev/null | awk -f "$lexer")"
} | awk '
{ c[NR]=$2; m[NR]=$3; b[NR]=$4; if($2>max)max=$2; if($3>max)max=$3; if($4>max)max=$4; n=NR }
function fmt(v) { return v >= 10000 ? sprintf("%.0fk", v/1000) : sprintf("%.1fk", v/1000) }
function py(v) { return H-PB-(v/max)*ph }
function px(i) { return PADL+((i-1)/(n-1))*pw }
END {
    W=860; H=400; PADL=60; PADR=100; PB=40; PT=52
    pw=W-PADL-PADR; ph=H-PB-PT
    if(max==0) max=1
    ink="#0b0b0b"; ink2="#52514e"; grid="#e8e7e3"
    col[1]="#2a78d6"; col[2]="#eb6834"; col[3]="#1baf7a"
    name[1]="code"; name[2]="comments"; name[3]="blank"

    printf "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" font-family=\"sans-serif\" style=\"background:#fcfcfb\">\n", W, H
    printf "<text x=\"%d\" y=\"20\" font-size=\"14\" text-anchor=\"middle\" font-weight=\"bold\" fill=\"%s\">Lines of Code Over Time</text>\n", W/2, ink
    lx=PADL
    for(s=1;s<=3;s++) {
        printf "<circle cx=\"%d\" cy=\"36\" r=\"5\" fill=\"%s\"/>\n", lx+5, col[s]
        printf "<text x=\"%d\" y=\"40\" font-size=\"11\" fill=\"%s\">%s</text>\n", lx+15, ink2, name[s]
        lx += 15 + 11 + length(name[s])*6
    }
    printf "<text x=\"%d\" y=\"%d\" font-size=\"11\" text-anchor=\"middle\" fill=\"%s\">Commit</text>\n", PADL+pw/2, H-5, ink2
    step=int(max/5/1000)*1000; if(step==0) step=1000
    for(v=0; v<=max; v+=step) {
        printf "<line x1=\"%d\" y1=\"%.1f\" x2=\"%d\" y2=\"%.1f\" stroke=\"%s\"/>\n", PADL, py(v), W-PADR, py(v), grid
        printf "<text x=\"%d\" y=\"%.1f\" font-size=\"10\" text-anchor=\"end\" fill=\"%s\">%dk</text>\n", PADL-5, py(v)+4, ink2, v/1000
    }
    for(i=1; i<=n; i+=int(n/6)) {
        printf "<text x=\"%.1f\" y=\"%d\" font-size=\"10\" text-anchor=\"middle\" fill=\"%s\">%d</text>\n", px(i), H-PB+15, ink2, i
    }
    for(s=1;s<=3;s++) {
        printf "<polyline fill=\"none\" stroke=\"%s\" stroke-width=\"2\" points=\"", col[s]
        for(i=1; i<=n; i++) printf "%.1f,%.1f ", px(i), py(s==1?c[i]:s==2?m[i]:b[i])
        printf "\"/>\n"
    }
    # Direct end labels (dot carries identity, text stays in ink), nudged apart
    for(s=1;s<=3;s++) yy[s] = py(s==1?c[n]:s==2?m[n]:b[n])
    for(s=1;s<=3;s++) { best=0; for(t=1;t<=3;t++) if(!used[t] && (best==0 || yy[t]<yy[best])) best=t; used[best]=1; seq[s]=best }
    for(s=2;s<=3;s++) if(yy[seq[s]] < yy[seq[s-1]]+14) yy[seq[s]] = yy[seq[s-1]]+14
    for(s=1;s<=3;s++) {
        v = (s==1?c[n]:s==2?m[n]:b[n])
        printf "<circle cx=\"%d\" cy=\"%.1f\" r=\"4\" fill=\"%s\"/>\n", W-PADR+8, yy[s], col[s]
        printf "<text x=\"%d\" y=\"%.1f\" font-size=\"11\" fill=\"%s\">%s %s</text>\n", W-PADR+16, yy[s]+4, ink2, name[s], fmt(v)
    }
    printf "</svg>\n"
}' > "$outdir/loc_chart.svg"

echo "Generated $outdir/loc_chart.svg ($(wc -l < "$outdir/loc_chart.svg") lines)"
