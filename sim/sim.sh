#!/usr/bin/env bash
# LVGL simulator: build -> render every screen at 122x250 -> BMP + PNG.
# Usage:  ./sim.sh            (sample weather)
#         LOCATION="Seoul" ./sim.sh   (live Open-Meteo, the device's own path)
set -e
cd "$(dirname "$0")"

[ -d build ] || cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8

mkdir -p shots
rm -f shots/*.bmp shots/*.png
./build/sim shots

# sips is macOS-only; skip the PNG convenience copies elsewhere.
if command -v sips >/dev/null 2>&1; then
  for f in shots/*.bmp; do
    sips -s format png "$f" --out "${f%.bmp}.png" >/dev/null
  done
fi
echo "screenshots in sim/shots/ — 7 omikuji ranks, home, setup overlay"
