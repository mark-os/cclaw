#!/bin/bash
# Generate an SVG chart of lines of code (src/*.c, src/*.h, include/*.h, include/*.c) over commits.
set -eu
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"

outdir="docs"
mkdir -p "$outdir"

# Accumulate LOC per commit using git diff --numstat
git log --reverse --format="%H" | {
    total=0; i=0
    while read -r hash; do
        i=$((i+1))
        delta=$(git diff --numstat "$hash~" "$hash" -- 'src/*.c' 'src/*.h' 'include/*.h' 'include/*.c' 2>/dev/null | awk '{s+=$1-$2} END{print s+0}')
        delta=${delta:-0}
        total=$((total+delta))
        echo "$i,$total"
    done
    # Final point: actual working-tree count, so uncommitted changes show up
    current=$(cat src/*.c src/*.h include/*.h include/*.c 2>/dev/null | wc -l)
    echo "$((i+1)),$current"
} | awk -F, '
{ x[NR]=$1; y[NR]=$2; if($2>max) max=$2; n=NR }
END {
    W=800; H=400; PAD=60; PB=40; PT=30
    pw=W-PAD-20; ph=H-PB-PT
    if(max==0) max=1

    printf "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" style=\"background:#fff\">\n", W, H
    printf "<text x=\"%d\" y=\"20\" font-family=\"sans-serif\" font-size=\"14\" text-anchor=\"middle\" font-weight=\"bold\">Lines of Code Over Time</text>\n", W/2
    printf "<text x=\"%d\" y=\"%d\" font-family=\"sans-serif\" font-size=\"11\" text-anchor=\"middle\">Commit</text>\n", PAD+pw/2, H-5
    printf "<text x=\"15\" y=\"%d\" font-family=\"sans-serif\" font-size=\"11\" text-anchor=\"middle\" transform=\"rotate(-90,15,%d)\">Lines of Code</text>\n", PT+ph/2, PT+ph/2

    step=int(max/5/1000)*1000; if(step==0) step=1000
    for(v=0; v<=max; v+=step) {
        yy=H-PB-(v/max)*ph
        printf "<line x1=\"%d\" y1=\"%.1f\" x2=\"%d\" y2=\"%.1f\" stroke=\"#ddd\"/>\n", PAD, yy, W-20, yy
        printf "<text x=\"%d\" y=\"%.1f\" font-family=\"sans-serif\" font-size=\"10\" text-anchor=\"end\">%dk</text>\n", PAD-5, yy+4, v/1000
    }
    for(i=1; i<=n; i+=int(n/6)) {
        xx=PAD+((i-1)/(n-1))*pw
        printf "<text x=\"%.1f\" y=\"%d\" font-family=\"sans-serif\" font-size=\"10\" text-anchor=\"middle\">%d</text>\n", xx, H-PB+15, i
    }

    printf "<polyline fill=\"none\" stroke=\"#2563eb\" stroke-width=\"1.5\" points=\""
    for(i=1; i<=n; i++) {
        xx=PAD+((i-1)/(n-1))*pw
        yy=H-PB-(y[i]/max)*ph
        printf "%.1f,%.1f ", xx, yy
    }
    printf "\"/>\n</svg>\n"
}' > "$outdir/loc_chart.svg"

echo "Generated $outdir/loc_chart.svg ($(wc -l < "$outdir/loc_chart.svg") lines)"
