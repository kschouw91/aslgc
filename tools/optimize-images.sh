#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Recompresses PNG files with ImageMagick 7: tries every zlib filter and strategy, drops
# metadata and timestamps, and keeps a result only if it is smaller and pixel-identical.
#
#   tools/optimize-images.sh [file.png...]            lossless (default: all PNGs in resources/ and docs/)
#   tools/optimize-images.sh --palette file.png...    first reduce to a 256-colour palette (lossy,
#                                                     meant for documentation screenshots)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

palette=0
if [[ "${1:-}" == "--palette" ]]; then
    palette=1
    shift
fi
if (( $# )); then
    files=("$@")
else
    mapfile -t files < <(find "$root/resources" "$root/docs" -name '*.png' | sort)
fi

total_before=0
total_after=0
for file in "${files[@]}"; do
    before=$(stat -c %s "$file")
    reference="$file"
    if (( palette )); then
        reference="$tmp/palette.png"
        magick "$file" -dither None -colors 256 "$reference"
    fi
    best=""
    best_size=$before
    for filter in 0 1 2 3 4 5; do
        for strategy in 0 1 2 3 4; do
            candidate="$tmp/candidate-$filter-$strategy.png"
            magick "$reference" -strip \
                -define png:exclude-chunks=date,time \
                -define png:compression-level=9 \
                -define png:compression-filter="$filter" \
                -define png:compression-strategy="$strategy" \
                "$candidate"
            size=$(stat -c %s "$candidate")
            if (( size < best_size )) && magick compare -metric AE "$reference" "$candidate" null: 2>/dev/null; then
                best="$candidate"
                best_size=$size
            fi
        done
    done
    if [[ -n "$best" ]]; then
        cp "$best" "$file"
    fi
    rm -f "$tmp"/*.png
    after=$(stat -c %s "$file")
    total_before=$((total_before + before))
    total_after=$((total_after + after))
    printf '%9d → %9d  %3d%%  %s\n' "$before" "$after" $(( (before - after) * 100 / (before > 0 ? before : 1) )) "${file#"$root"/}"
done
printf '%9d → %9d  %3d%%  total\n' "$total_before" "$total_after" $(( (total_before - total_after) * 100 / (total_before > 0 ? total_before : 1) ))
