#!/bin/sh

set -eu

usage() {
    echo "usage: $0 INPUT.yuv WIDTH HEIGHT [RUNS] [RESULTS.csv]" >&2
    exit 2
}

[ "$#" -ge 3 ] && [ "$#" -le 5 ] || usage

caller_dir=$(pwd)
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)

case $1 in
    /*) input=$1 ;;
    *) input=$caller_dir/$1 ;;
esac

width=$2
height=$3
runs=${4:-3}

case ${5:-} in
    "") results=$script_dir/encoder_results.csv ;;
    /*) results=$5 ;;
    *) results=$caller_dir/$5 ;;
esac

case $width:$height:$runs in
    *[!0-9:]* | :* | *::*) usage ;;
esac

[ "$width" -gt 0 ] && [ "$height" -gt 0 ] && [ "$runs" -ge 3 ] || usage
[ -f "$input" ] || {
    echo "input file not found: $input" >&2
    exit 1
}
[ -x "$project_dir/video_compressor" ] || {
    echo "video_compressor is missing; run 'make' first" >&2
    exit 1
}
[ -d "$(dirname -- "$results")" ] || {
    echo "results directory does not exist: $(dirname -- "$results")" >&2
    exit 1
}

temp_dir=$(mktemp -d "${TMPDIR:-/tmp}/ospro-benchmark.XXXXXX")
cleanup() {
    rm -f "$temp_dir"/encoded.bin "$temp_dir"/run.log \
        "$temp_dir"/timing.txt "$temp_dir"/raw.csv "$temp_dir"/averages.csv
    rmdir "$temp_dir"
}
trap cleanup EXIT HUP INT TERM

raw=$temp_dir/raw.csv
averages=$temp_dir/averages.csv
: > "$raw"
: > "$averages"

echo "Benchmarking $(basename -- "$input") (${width}x${height}), $runs runs per thread count"
echo

for threads in 1 2 4 8; do
    printf '  %d thread(s), warm-up (not timed)\n' "$threads"
    if ! "$project_dir/video_compressor" \
        "$input" "$temp_dir/encoded.bin" "$width" "$height" "$threads" \
        > "$temp_dir/run.log" 2>&1; then
        cat "$temp_dir/run.log" >&2
        exit 1
    fi

    run=1
    while [ "$run" -le "$runs" ]; do
        if ! /usr/bin/time -p "$project_dir/video_compressor" \
            "$input" "$temp_dir/encoded.bin" "$width" "$height" "$threads" \
            > "$temp_dir/run.log" 2> "$temp_dir/timing.txt"; then
            cat "$temp_dir/run.log" >&2
            cat "$temp_dir/timing.txt" >&2
            exit 1
        fi

        seconds=$(awk '$1 == "real" { print $2; found = 1 } END { if (!found) exit 1 }' \
            "$temp_dir/timing.txt")
        printf '%s,%s,%s\n' "$threads" "$run" "$seconds" >> "$raw"
        printf '  %d thread(s), run %d: %.3f s\n' "$threads" "$run" "$seconds"
        run=$((run + 1))
    done

    average=$(awk -F, -v wanted="$threads" \
        '$1 == wanted { sum += $3; count++ } END { printf "%.6f", sum / count }' \
        "$raw")
    printf '%s,%s\n' "$threads" "$average" >> "$averages"
done

baseline=$(awk -F, '$1 == 1 { print $2 }' "$averages")

printf 'threads,measurement,seconds,speedup_vs_1_thread\n' > "$results"
for threads in 1 2 4 8; do
    awk -F, -v wanted="$threads" \
        '$1 == wanted { printf "%s,run_%s,%s,\n", $1, $2, $3 }' "$raw" \
        >> "$results"
    average=$(awk -F, -v wanted="$threads" '$1 == wanted { print $2 }' "$averages")
    speedup=$(awk -v base="$baseline" -v current="$average" \
        'BEGIN { printf "%.3f", base / current }')
    printf '%s,average,%s,%s\n' "$threads" "$average" "$speedup" >> "$results"
done

echo
printf '%-9s %12s %12s\n' "Threads" "Average (s)" "Speedup"
printf '%-9s %12s %12s\n' "-------" "-----------" "-------"
while IFS=, read -r threads average; do
    speedup=$(awk -v base="$baseline" -v current="$average" \
        'BEGIN { printf "%.3f", base / current }')
    printf '%-9s %12.3f %11sx\n' "$threads" "$average" "$speedup"
done < "$averages"

echo
echo "Saved raw runs and averages to $results"
