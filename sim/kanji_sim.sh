#!/usr/bin/env bash
# LVGL simulator: build -> render every screen at 648x480 -> BMP + PNG, and
# assert on the pixels. Exits non-zero if any layout or glyph check fails.
#
# Usage:  ./kanji_sim.sh                                             # the demo card
#         KANJI_URL=http://localhost:8123/kanji.json ./kanji_sim.sh  # the device's own fetch path
set -e
cd "$(dirname "$0")"

[ -d build ] || cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target kanji_sim -j8

mkdir -p shots
rm -f shots/*.bmp shots/*.png
./build/kanji_sim shots

# sips is macOS-only; skip the PNG convenience copies elsewhere.
if command -v sips >/dev/null 2>&1; then
  for f in shots/*.bmp; do
    sips -s format png "$f" --out "${f%.bmp}.png" >/dev/null
  done
fi
echo "screenshots in sim/shots/ — the question and answer sides, the grade dock"
echo "on three of its four ratings, the 설명 / 댓글 / FSRS sheets, and the"
echo "session-complete, long-headword, offline and Wi-Fi-setup states"
