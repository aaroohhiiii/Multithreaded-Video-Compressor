#!/bin/sh

set -eu

usage() {
    echo "usage: $0 INPUT_VIDEO [SECONDS] [OUTPUT_DIR]" >&2
    exit 2
}

[ "$#" -ge 1 ] && [ "$#" -le 3 ] || usage

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
case $1 in
    /*) input=$1 ;;
    *) input=$(pwd)/$1 ;;
esac
duration=${2:-5}
case ${3:-demo_output} in
    /*) output_dir=${3:-demo_output} ;;
    *) output_dir=$(pwd)/${3:-demo_output} ;;
esac

case $duration in
    '' | *[!0-9.]*) usage ;;
esac
awk -v duration="$duration" 'BEGIN { exit !(duration > 0) }' || usage
[ -f "$input" ] || { echo "video not found: $input" >&2; exit 1; }
command -v ffmpeg >/dev/null 2>&1 || { echo "ffmpeg is required" >&2; exit 1; }
command -v ffprobe >/dev/null 2>&1 || { echo "ffprobe is required" >&2; exit 1; }

width=$(ffprobe -v error -select_streams v:0 -show_entries stream=width \
    -of default=noprint_wrappers=1:nokey=1 "$input")
height=$(ffprobe -v error -select_streams v:0 -show_entries stream=height \
    -of default=noprint_wrappers=1:nokey=1 "$input")
frame_rate=$(ffprobe -v error -select_streams v:0 -show_entries stream=avg_frame_rate \
    -of default=noprint_wrappers=1:nokey=1 "$input")

width=$((width - width % 16))
height=$((height - height % 16))
[ "$width" -ge 16 ] && [ "$height" -ge 16 ] || {
    echo "video dimensions are too small" >&2
    exit 1
}
[ "$frame_rate" != "0/0" ] || frame_rate=30

mkdir -p "$output_dir"
raw=$output_dir/input.yuv
bitstream=$output_dir/compressed.bin
decoded=$output_dir/decoded.yuv
preview=$output_dir/decoded-preview.mp4
timing=$output_dir/encoder-time.txt

echo "Preparing ${duration}s of $(basename -- "$input") at ${width}x${height}..."
ffmpeg -y -v error -i "$input" -t "$duration" -an \
    -vf "crop=${width}:${height}:0:0" -pix_fmt yuv420p -f rawvideo "$raw"

if [ ! -x "$script_dir/video_compressor" ] || \
   [ ! -x "$script_dir/video_decoder" ]; then
    echo "Building encoder and decoder..."
    make -C "$script_dir"
fi

echo "Encoding with 4 pthread workers..."
if ! /usr/bin/time -p "$script_dir/video_compressor" \
    "$raw" "$bitstream" "$width" "$height" 4 2> "$timing"; then
    cat "$timing" >&2
    exit 1
fi
echo "Encoder wall-clock timing:"
sed -n '/^real /p' "$timing"

echo "Decoding compressed bitstream..."
"$script_dir/video_decoder" "$bitstream" "$decoded" "$width" "$height"

echo "Creating viewable MP4..."
ffmpeg -y -v error -f rawvideo -pixel_format yuv420p \
    -video_size "${width}x${height}" -framerate "$frame_rate" -i "$decoded" \
    -an -c:v libx264 -preset fast -crf 18 -pix_fmt yuv420p "$preview"

raw_bytes=$(wc -c < "$raw" | tr -d ' ')
compressed_bytes=$(wc -c < "$bitstream" | tr -d ' ')
ratio=$(awk -v raw="$raw_bytes" -v compressed="$compressed_bytes" \
    'BEGIN { printf "%.2f", raw / compressed }')
echo "Compressed $raw_bytes bytes to $compressed_bytes bytes (${ratio}:1)."
echo "Preview: $preview"

if [ "${DEMO_NO_OPEN:-0}" = 1 ]; then
    :
elif command -v open >/dev/null 2>&1; then
    open "$preview"
elif command -v xdg-open >/dev/null 2>&1; then
    xdg-open "$preview" >/dev/null 2>&1 &
elif command -v ffplay >/dev/null 2>&1; then
    ffplay -autoexit -loglevel warning "$preview"
else
    echo "No video viewer was found; open the preview path above manually."
fi
