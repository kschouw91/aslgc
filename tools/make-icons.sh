#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Regenerates the app icon set from resources/icons/aslgc-source.png.
# Needs ImageMagick 7 (`magick`) at development time only; the generated PNGs are committed.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
src="$root/resources/icons/aslgc-source.png"
out="$root/resources/icons/hicolor"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

read -r w h < <(magick identify -format '%w %h\n' "$src")
mx=$((w - 1)); my=$((h - 1))

# 1. Make the white background outside the rounded square transparent (flood fill from each corner).
# 2. Trim to the artwork, then centre it on a square canvas with a small margin.
magick "$src" -alpha set -fuzz 22% -fill none \
    -draw "alpha 0,0 floodfill" -draw "alpha $mx,0 floodfill" \
    -draw "alpha 0,$my floodfill" -draw "alpha $mx,$my floodfill" \
    -trim +repage "$tmp/trimmed.png"
read -r tw th < <(magick identify -format '%w %h\n' "$tmp/trimmed.png")
side=$(( (tw > th ? tw : th) * 104 / 100 ))
magick "$tmp/trimmed.png" -background none -gravity center -extent "${side}x${side}" "$tmp/master.png"

for size in 16 22 24 32 48 64 128 256 512; do
    dir="$out/${size}x${size}/apps"
    mkdir -p "$dir"
    sharpen=()
    if (( size <= 48 )); then sharpen=(-unsharp 0x0.6+0.6+0.02); fi
    magick "$tmp/master.png" -filter Lanczos -resize "${size}x${size}" "${sharpen[@]}" \
        -strip -define png:compression-level=9 "PNG32:$dir/aslgc.png"
    echo "wrote $dir/aslgc.png"
done

# Squeeze the generated PNGs further (lossless, pixel-identical).
"$root/tools/optimize-images.sh" "$out"/*/apps/aslgc.png
