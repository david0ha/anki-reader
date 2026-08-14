#!/usr/bin/env bash
# LVGL simulator: build -> render every screen at 648x480 -> BMP + PNG, and
# assert on the pixels. Exits non-zero if any layout or glyph check fails.
#
# Usage:  ./kanji_sim.sh                                             # the demo card
#         KANJI_URL=http://localhost:8123/kanji.json ./kanji_sim.sh  # the device's own fetch path
set -e
cd "$(dirname "$0")"

canonical=(
  01-question
  02-answer
  03-answer-easy
  04-answer-again
  04b-answer-three-examples
  05-description-shape
  05b-description-hook
  05c-description-parts
  06-comments
  07-comments-page2
  08-fsrs-1
  09-fsrs-2
  10-fsrs-3
  11-session-complete
  12-long-headword
  12b-ascii-headword
  13-offline
  14-setup
  15-no-data
  16-stale
)
auxiliary=(
  aux-description-shape-max
  aux-description-hook-max
  aux-description-parts-max
)

check_gallery() {
  local ext="$1"
  local canonical_count aux_count total_count name path unexpected expected
  canonical_count=$(find shots -maxdepth 1 -type f -name "*.${ext}" ! -name 'aux-*' | wc -l | tr -d ' ')
  aux_count=$(find shots -maxdepth 1 -type f -name "aux-*.${ext}" | wc -l | tr -d ' ')
  total_count=$(find shots -maxdepth 1 -type f -name "*.${ext}" | wc -l | tr -d ' ')

  [ "$canonical_count" -eq 20 ] || {
    echo "expected 20 canonical ${ext} files, found ${canonical_count}" >&2
    return 1
  }
  [ "$aux_count" -eq 3 ] || {
    echo "expected 3 auxiliary ${ext} files, found ${aux_count}" >&2
    return 1
  }
  [ "$total_count" -eq 23 ] || {
    echo "expected 23 total ${ext} files, found ${total_count}" >&2
    return 1
  }

  for name in "${canonical[@]}" "${auxiliary[@]}"; do
    path="shots/${name}.${ext}"
    [ -f "$path" ] || {
      echo "missing required gallery file: ${path}" >&2
      return 1
    }
  done

  for path in shots/*."${ext}"; do
    name=${path#shots/}
    name=${name%."${ext}"}
    unexpected=true
    for expected in "${canonical[@]}" "${auxiliary[@]}"; do
      if [ "$name" = "$expected" ]; then
        unexpected=false
        break
      fi
    done
    if [ "$unexpected" = true ]; then
      echo "unexpected gallery file: ${path}" >&2
      return 1
    fi
  done
  echo "gallery ${ext}: ${canonical_count} canonical + ${aux_count} auxiliary (${total_count} total)"
}

[ -d build ] || cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target kanji_sim -j8

mkdir -p shots
rm -f shots/*.bmp shots/*.png
./build/kanji_sim shots
check_gallery bmp

# PNGs are review artifacts on macOS, where sips is part of the base system.
if [ "$(uname -s)" = Darwin ]; then
  command -v sips >/dev/null 2>&1 || {
    echo "macOS simulator review requires sips for PNG conversion" >&2
    exit 1
  }
  for f in shots/*.bmp; do
    sips -s format png "$f" --out "${f%.bmp}.png" >/dev/null
  done
  check_gallery png
fi
echo "screenshots in sim/shots/ — 20 canonical states and 3 auxiliary max-content proofs"
